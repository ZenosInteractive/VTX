#include "services/replay_cut_service.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

#include "vtx/writer/policies/formatters/flatbuffers_vtx_policy.h"

namespace {

    constexpr double kTicksPerSecond = 10'000'000.0;

    // Streams [offset, offset+bytes) from `in` to `out` through a bounded buffer.
    bool CopyRange(std::ifstream& in, std::ofstream& out, uint64_t offset, uint64_t bytes, std::string& error) {
        constexpr size_t kBufferSize = 4 * 1024 * 1024;
        std::vector<char> buffer(std::min<uint64_t>(bytes, kBufferSize));
        in.seekg(static_cast<std::streamoff>(offset));
        uint64_t remaining = bytes;
        while (remaining > 0) {
            const size_t step = static_cast<size_t>(std::min<uint64_t>(remaining, buffer.size()));
            if (!in.read(buffer.data(), static_cast<std::streamsize>(step))) {
                error = "Read failed at offset " + std::to_string(offset + (bytes - remaining));
                return false;
            }
            if (!out.write(buffer.data(), static_cast<std::streamsize>(step))) {
                error = "Write failed";
                return false;
            }
            remaining -= step;
        }
        return true;
    }

    // Duration of the sliced range from a tick table (first..last nonzero), or a
    // negative value when the table cannot answer.
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
        return static_cast<double>(last - first) / kTicksPerSecond;
    }

} // namespace

namespace VtxServices {

    ReplayCutPlan ReplayCutService::PlanCut(const VTX::FileFooter& footer, int start_frame, int end_frame) {
        ReplayCutPlan plan;
        if (footer.chunk_index.empty() || footer.total_frames <= 0) {
            plan.error = "The loaded replay has no chunk index.";
            return plan;
        }
        start_frame = std::clamp(start_frame, 0, footer.total_frames - 1);
        end_frame = std::clamp(end_frame, 0, footer.total_frames - 1);
        if (end_frame < start_frame) {
            plan.error = "End of range is before its start.";
            return plan;
        }

        int first_chunk = -1;
        int last_chunk = -1;
        for (int i = 0; i < static_cast<int>(footer.chunk_index.size()); ++i) {
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

        plan.first_chunk = first_chunk;
        plan.last_chunk = last_chunk;
        plan.first_frame = footer.chunk_index[static_cast<size_t>(first_chunk)].start_frame;
        plan.last_frame = footer.chunk_index[static_cast<size_t>(last_chunk)].end_frame;
        for (int i = first_chunk; i <= last_chunk; ++i) {
            plan.chunk_bytes += footer.chunk_index[static_cast<size_t>(i)].chunk_size_bytes;
        }
        plan.valid = true;
        return plan;
    }

    bool ReplayCutService::ExecuteCut(const std::string& source_path, const VTX::FileFooter& footer,
                                      const ReplayCutPlan& plan, const std::string& dest_path, std::string& error) {
        if (!plan.valid) {
            error = plan.error.empty() ? "Invalid cut plan." : plan.error;
            return false;
        }
        namespace fs = std::filesystem;
        std::error_code ec;
        if (fs::equivalent(fs::path(source_path), fs::path(dest_path), ec)) {
            error = "Destination must be a different file than the source.";
            return false;
        }

        std::ifstream in(source_path, std::ios::binary);
        if (!in) {
            error = "Could not open the source replay: " + source_path;
            return false;
        }
        std::ofstream out(dest_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "Could not create the output file: " + dest_path;
            return false;
        }

        // 1) Header region, verbatim: everything before the first chunk of the
        // ORIGINAL file (magic + length-prefixed header payload). The header
        // carries no frame-dependent data; copying it keeps the schema and
        // recording metadata bit-identical.
        const uint64_t header_end = footer.chunk_index.front().file_offset;
        if (!CopyRange(in, out, 0, header_end, error)) {
            return false;
        }

        // 2) Kept chunks, verbatim, while rebuilding the seek table with new
        // offsets and frame numbering rebased so the cut starts at frame 0.
        std::vector<VTX::ChunkIndexData> seek_table;
        seek_table.reserve(static_cast<size_t>(plan.last_chunk - plan.first_chunk + 1));
        for (int i = plan.first_chunk; i <= plan.last_chunk; ++i) {
            const auto& entry = footer.chunk_index[static_cast<size_t>(i)];
            VTX::ChunkIndexData rebased;
            rebased.chunk_index = i - plan.first_chunk;
            rebased.file_offset = static_cast<int64_t>(out.tellp());
            rebased.chunk_size_bytes = entry.chunk_size_bytes;
            rebased.start_frame = entry.start_frame - plan.first_frame;
            rebased.end_frame = entry.end_frame - plan.first_frame;
            rebased.checksum = entry.checksum;
            seek_table.push_back(rebased);
            if (!CopyRange(in, out, entry.file_offset, entry.chunk_size_bytes, error)) {
                return false;
            }
        }

        // 3) Footer: time table sliced to the kept frames (tick values stay
        // absolute; only frame indices are rebased), duration recomputed.
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
                const int frame = static_cast<int>(value);
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

        VTX::SessionFooter session_footer;
        session_footer.total_frames = plan.last_frame - plan.first_frame + 1;
        double duration = DurationFromTicks(created_utc);
        if (duration < 0.0) {
            duration = DurationFromTicks(game_times);
        }
        if (duration < 0.0 && footer.total_frames > 0) {
            duration = static_cast<double>(footer.duration_seconds) * session_footer.total_frames / footer.total_frames;
        }
        session_footer.duration_seconds = std::max(duration, 0.0);
        session_footer.game_times = &game_times;
        session_footer.created_utc = &created_utc;
        session_footer.gaps = &gaps;
        session_footer.segments = &segments;

        // Written uncompressed; the reader sniffs the zstd magic and passes
        // uncompressed payloads through.
        const std::string footer_payload = VTX::FlatBuffersVtxPolicy::SerializeFooter(seek_table, session_footer);
        const uint32_t footer_size = static_cast<uint32_t>(footer_payload.size());
        const std::string magic = VTX::FlatBuffersVtxPolicy::GetMagicBytes();
        if (!out.write(footer_payload.data(), static_cast<std::streamsize>(footer_payload.size())) ||
            !out.write(reinterpret_cast<const char*>(&footer_size), sizeof(footer_size)) ||
            !out.write(magic.data(), static_cast<std::streamsize>(magic.size()))) {
            error = "Write failed while finishing the footer.";
            return false;
        }
        out.flush();
        if (!out) {
            error = "Flush failed for the output file.";
            return false;
        }
        return true;
    }

} // namespace VtxServices
