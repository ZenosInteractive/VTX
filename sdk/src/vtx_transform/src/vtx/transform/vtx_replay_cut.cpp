#include "vtx/transform/vtx_replay_cut.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "vtx_schema_generated.h" // complete fbsvtx/cppvtx types before the policy headers
#include "vtx_schema.pb.h"

#include "vtx/common/vtx_replay_framing.h"
#include "vtx/common/vtx_types.h"
#include "vtx/reader/core/vtx_reader_facade.h"
#include "vtx/writer/policies/formatters/flatbuffers_vtx_policy.h"
#include "vtx/writer/policies/formatters/protobuff_vtx_policy.h"

namespace VTX {

    namespace {

        // Duration of a sliced tick table (first..last nonzero stamp), or negative when
        // the table cannot answer.
        double DurationFromTicks(const std::vector<int64_t>& ticks) {
            int64_t first = 0;
            int64_t last = 0;
            for (const int64_t t : ticks) {
                if (t != 0) {
                    if (first == 0) {
                        first = t;
                    }
                    last = t;
                }
            }
            if (first == 0 || last < first) {
                return -1.0;
            }
            return static_cast<double>(last - first) / static_cast<double>(GameTime::TICKS_PER_SECOND);
        }

        ReplayCutPlan PlanForChunkSpan(const FileFooter& footer, int32_t first_chunk, int32_t last_chunk,
                                       int32_t first_frame, int32_t last_frame) {
            ReplayCutPlan plan;
            plan.first_chunk = first_chunk;
            plan.last_chunk = last_chunk;
            plan.first_frame = first_frame;
            plan.last_frame = last_frame;
            plan.trims_head = footer.chunk_index[static_cast<size_t>(first_chunk)].start_frame < first_frame;
            plan.trims_tail = footer.chunk_index[static_cast<size_t>(last_chunk)].end_frame > last_frame;
            for (int32_t i = first_chunk; i <= last_chunk; ++i) {
                plan.chunk_bytes += footer.chunk_index[static_cast<size_t>(i)].chunk_size_bytes;
            }
            plan.valid = true;
            return plan;
        }

        // Frames [kept_lo, kept_hi] of the source re-serialized as one chunk through the
        // shared framing: per-chunk zstd decision mirrored from the source chunk, fresh
        // seek-table entry.
        template <typename Policy>
        bool WriteRewrittenChunk(IVtxReaderFacade& reader, std::ofstream& out, int32_t kept_lo, int32_t kept_hi,
                                 int32_t new_chunk_index, int32_t rebase_frame, bool compress,
                                 ChunkIndexData& entry_out, std::string& error) {
            std::vector<std::unique_ptr<typename Policy::FrameType>> frames;
            frames.reserve(static_cast<size_t>(kept_hi - kept_lo + 1));
            for (int32_t frame_index = kept_lo; frame_index <= kept_hi; ++frame_index) {
                const Frame* source_frame = reader.GetFrameSync(frame_index);
                if (!source_frame) {
                    error = "Could not read frame " + std::to_string(frame_index) + " from the source replay.";
                    return false;
                }
                frames.push_back(Policy::FromNative(Frame(*source_frame)));
            }

            std::string payload = Policy::SerializeChunk(frames, new_chunk_index, compress);
            payload = Framing::CompressIfBeneficial(std::move(payload), compress, Framing::kDefaultCompressionLevel);

            entry_out.chunk_index = new_chunk_index;
            entry_out.file_offset = static_cast<int64_t>(out.tellp());
            entry_out.chunk_size_bytes = Framing::ChunkSizeOnDisk(payload.size());
            entry_out.start_frame = kept_lo - rebase_frame;
            entry_out.end_frame = kept_hi - rebase_frame;
            entry_out.checksum = Framing::PayloadChecksum(payload);

            const std::string block = Framing::ChunkBlock(payload);
            if (!out.write(block.data(), static_cast<std::streamsize>(block.size()))) {
                error = "Write failed for rewritten chunk " + std::to_string(new_chunk_index) + ".";
                return false;
            }
            return true;
        }

        template <typename Policy>
        void RunCut(const std::string& source_path, const FileFooter& footer, const ReplayCutPlan& plan,
                    const std::string& dest_path, ReplayCutResult& result) {
            namespace fs = std::filesystem;
            std::string error;

            // Edge chunks that start/end inside the kept range are re-serialized frame
            // by frame; open a private reader for that (a caller's reader may be busy on
            // its own thread). Whole-chunk cuts never decode a frame.
            ReaderContext reader_context;
            if (plan.trims_head || plan.trims_tail) {
                reader_context = OpenReplayFile(source_path);
                if (!reader_context.Loaded()) {
                    result.error = "Could not reopen the source replay for edge-chunk rewriting: " +
                                   reader_context.GetError().message;
                    return;
                }
                if (!reader_context.WaitUntilReady()) {
                    result.error = "The source replay failed to load: " + reader_context.GetReadyError().message;
                    return;
                }
            }

            std::ifstream in(source_path, std::ios::binary);
            if (!in) {
                result.error = "Could not open the source replay: " + source_path;
                return;
            }
            Framing::FileLayout layout;
            if (!Framing::ProbeLayout(in, Policy::GetMagicBytes(), layout, error)) {
                result.error = error;
                return;
            }
            for (int32_t i = plan.first_chunk; i <= plan.last_chunk; ++i) {
                const ChunkIndexEntry& chunk = footer.chunk_index[static_cast<size_t>(i)];
                if (chunk.chunk_size_bytes <= Framing::kSizePrefixSize || chunk.file_offset < layout.header_end ||
                    chunk.file_offset + chunk.chunk_size_bytes > layout.footer_offset) {
                    result.error = "The source replay seek table is inconsistent with the file layout (chunk " +
                                   std::to_string(chunk.chunk_index) + ").";
                    return;
                }
            }

            std::ofstream out(dest_path, std::ios::binary | std::ios::trunc);
            if (!out) {
                result.error = "Could not create the output file: " + dest_path;
                return;
            }
            const auto abort_output = [&](std::string message) {
                out.close();
                std::error_code ec;
                fs::remove(dest_path, ec);
                result.error = std::move(message);
            };

            // 1) Header block, verbatim: it carries no frame-dependent data, so copying
            // it keeps the schema and recording metadata bit-identical.
            if (!Framing::CopyRange(in, out, 0, layout.header_end, error)) {
                abort_output(error);
                return;
            }

            // 2) Chunks. Whole chunks are copied verbatim (their seek entry is rebased);
            // partial edge chunks are rebuilt with only the kept frames.
            std::vector<ChunkIndexData> seek_table;
            seek_table.reserve(static_cast<size_t>(plan.last_chunk - plan.first_chunk + 1));
            for (int32_t i = plan.first_chunk; i <= plan.last_chunk; ++i) {
                const ChunkIndexEntry& entry = footer.chunk_index[static_cast<size_t>(i)];
                const int32_t kept_lo = std::max(entry.start_frame, plan.first_frame);
                const int32_t kept_hi = std::min(entry.end_frame, plan.last_frame);
                const int32_t new_index = static_cast<int32_t>(seek_table.size());

                if (kept_lo == entry.start_frame && kept_hi == entry.end_frame) {
                    ChunkIndexData rebased;
                    rebased.chunk_index = new_index;
                    rebased.file_offset = static_cast<int64_t>(out.tellp());
                    rebased.chunk_size_bytes = entry.chunk_size_bytes;
                    rebased.start_frame = entry.start_frame - plan.first_frame;
                    rebased.end_frame = entry.end_frame - plan.first_frame;
                    rebased.checksum = entry.checksum;
                    if (!Framing::CopyRange(in, out, entry.file_offset, entry.chunk_size_bytes, error)) {
                        abort_output(error);
                        return;
                    }
                    seek_table.push_back(rebased);
                } else {
                    const bool compress = Framing::IsZstdAt(in, entry.file_offset + Framing::kSizePrefixSize,
                                                            entry.chunk_size_bytes - Framing::kSizePrefixSize);
                    ChunkIndexData rewritten;
                    if (!WriteRewrittenChunk<Policy>(*reader_context.reader, out, kept_lo, kept_hi, new_index,
                                                     plan.first_frame, compress, rewritten, error)) {
                        abort_output(error);
                        return;
                    }
                    seek_table.push_back(rewritten);
                    ++result.chunks_rewritten;
                }
            }

            // 3) Footer: time table sliced to the kept frames (tick values stay absolute,
            // only frame indices are rebased), duration recomputed, fresh seek table.
            const auto slice_ticks = [&](const std::vector<uint64_t>& ticks) {
                std::vector<int64_t> sliced;
                const size_t begin = std::min<size_t>(static_cast<size_t>(plan.first_frame), ticks.size());
                const size_t end = std::min<size_t>(static_cast<size_t>(plan.last_frame) + 1, ticks.size());
                sliced.reserve(end - begin);
                for (size_t i = begin; i < end; ++i) {
                    sliced.push_back(static_cast<int64_t>(ticks[i]));
                }
                return sliced;
            };
            const auto slice_frame_indices = [&](const std::vector<uint32_t>& values) {
                std::vector<int32_t> sliced;
                for (const uint32_t value : values) {
                    const int32_t frame = static_cast<int32_t>(value);
                    if (frame >= plan.first_frame && frame <= plan.last_frame) {
                        sliced.push_back(frame - plan.first_frame);
                    }
                }
                return sliced;
            };

            const std::vector<int64_t> game_times = slice_ticks(footer.times.game_time);
            const std::vector<int64_t> created_utc = slice_ticks(footer.times.created_utc);
            const std::vector<int32_t> gaps = slice_frame_indices(footer.times.gaps);
            const std::vector<int32_t> segments = slice_frame_indices(footer.times.segments);

            SessionFooter session_footer;
            session_footer.total_frames = plan.FrameCount();
            double duration = DurationFromTicks(created_utc);
            if (duration < 0.0) {
                duration = DurationFromTicks(game_times);
            }
            if (duration < 0.0 && footer.total_frames > 0) {
                duration =
                    static_cast<double>(footer.duration_seconds) * session_footer.total_frames / footer.total_frames;
            }
            session_footer.duration_seconds = std::max(duration, 0.0);
            session_footer.game_times = &game_times;
            session_footer.created_utc = &created_utc;
            session_footer.gaps = &gaps;
            session_footer.segments = &segments;

            std::string footer_payload = Policy::SerializeFooter(seek_table, session_footer);
            footer_payload = Framing::CompressIfBeneficial(std::move(footer_payload), layout.footer_compressed,
                                                           Framing::kDefaultCompressionLevel);
            const std::string footer_block = Framing::FooterBlock(footer_payload, Policy::GetMagicBytes());
            if (!out.write(footer_block.data(), static_cast<std::streamsize>(footer_block.size()))) {
                abort_output("Write failed while finishing the footer.");
                return;
            }
            out.flush();
            if (!out) {
                abort_output("Flush failed for the output file.");
                return;
            }
            out.close();

            result.total_frames = session_footer.total_frames;
            result.chunks_written = static_cast<int32_t>(seek_table.size());
            std::error_code ec;
            result.output_bytes = fs::file_size(dest_path, ec);
        }

    } // namespace

    ReplayCutPlan PlanCutFrames(const FileFooter& footer, int32_t start_frame, int32_t end_frame) {
        ReplayCutPlan plan;
        if (footer.chunk_index.empty() || footer.total_frames <= 0) {
            plan.error = "The replay has no chunk index.";
            return plan;
        }
        start_frame = std::clamp(start_frame, 0, footer.total_frames - 1);
        end_frame = std::clamp(end_frame, 0, footer.total_frames - 1);
        if (end_frame < start_frame) {
            plan.error = "End of range is before its start.";
            return plan;
        }

        int32_t first_chunk = -1;
        int32_t last_chunk = -1;
        for (int32_t i = 0; i < static_cast<int32_t>(footer.chunk_index.size()); ++i) {
            const auto& entry = footer.chunk_index[static_cast<size_t>(i)];
            if (first_chunk < 0 && start_frame <= entry.end_frame) {
                first_chunk = i;
            }
            if (end_frame >= entry.start_frame) {
                last_chunk = i;
            }
        }
        if (first_chunk < 0 || last_chunk < first_chunk) {
            plan.error = "The requested range does not overlap any chunk.";
            return plan;
        }
        // The exact bounds are clamped into the covered span (a start that falls in a
        // gap between chunks snaps to the first covered frame).
        const int32_t lo = std::max(start_frame, footer.chunk_index[static_cast<size_t>(first_chunk)].start_frame);
        const int32_t hi = std::min(end_frame, footer.chunk_index[static_cast<size_t>(last_chunk)].end_frame);
        return PlanForChunkSpan(footer, first_chunk, last_chunk, lo, hi);
    }

    ReplayCutPlan PlanCutChunks(const FileFooter& footer, int32_t first_chunk, int32_t last_chunk) {
        ReplayCutPlan plan;
        if (footer.chunk_index.empty()) {
            plan.error = "The replay has no chunk index.";
            return plan;
        }
        const int32_t max_chunk = static_cast<int32_t>(footer.chunk_index.size()) - 1;
        first_chunk = std::clamp(first_chunk, 0, max_chunk);
        last_chunk = std::clamp(last_chunk, 0, max_chunk);
        if (last_chunk < first_chunk) {
            plan.error = "End chunk is before start chunk.";
            return plan;
        }
        return PlanForChunkSpan(footer, first_chunk, last_chunk,
                                footer.chunk_index[static_cast<size_t>(first_chunk)].start_frame,
                                footer.chunk_index[static_cast<size_t>(last_chunk)].end_frame);
    }

    ReplayCutResult CutReplayFile(const std::string& source_path, const FileFooter& footer, const ReplayCutPlan& plan,
                                  const std::string& dest_path) {
        namespace fs = std::filesystem;
        ReplayCutResult result;

        if (!plan.valid) {
            result.error = plan.error.empty() ? "Invalid cut plan." : plan.error;
            return result;
        }
        if (source_path.empty() || dest_path.empty()) {
            result.error = "Source and destination paths are required.";
            return result;
        }
        if (plan.first_chunk < 0 || plan.last_chunk < plan.first_chunk ||
            static_cast<size_t>(plan.last_chunk) >= footer.chunk_index.size()) {
            result.error = "The cut plan does not match the footer's chunk index.";
            return result;
        }
        std::error_code ec;
        if (!fs::is_regular_file(source_path, ec)) {
            result.error = "Source replay does not exist: " + source_path;
            return result;
        }
        const fs::path source_abs = fs::weakly_canonical(fs::path(source_path), ec);
        const fs::path dest_abs = fs::weakly_canonical(fs::path(dest_path), ec);
        if (source_abs == dest_abs || (fs::exists(dest_path, ec) && fs::equivalent(source_path, dest_path, ec))) {
            result.error = "Destination must be a different file than the source.";
            return result;
        }

        // The format is decided by the source's leading magic.
        std::string magic(Framing::kMagicSize, '\0');
        {
            std::ifstream in(source_path, std::ios::binary);
            if (!in || !in.read(magic.data(), static_cast<std::streamsize>(magic.size()))) {
                result.error = "Could not read the source replay: " + source_path;
                return result;
            }
        }
        if (magic == FlatBuffersVtxPolicy::GetMagicBytes()) {
            RunCut<FlatBuffersVtxPolicy>(source_path, footer, plan, dest_path, result);
        } else if (magic == ProtobufVtxPolicy::GetMagicBytes()) {
            RunCut<ProtobufVtxPolicy>(source_path, footer, plan, dest_path, result);
        } else {
            result.error = "The source replay does not start with a known VTX magic.";
        }
        return result;
    }

} // namespace VTX
