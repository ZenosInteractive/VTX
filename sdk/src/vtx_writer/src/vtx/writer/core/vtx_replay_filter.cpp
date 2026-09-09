#include "vtx/writer/core/vtx_replay_filter.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <xxh3.h>
#include <zstd.h>

#include "vtx_schema_generated.h" // complete fbsvtx/cppvtx types before the policy headers
#include "vtx_schema.pb.h"

#include "vtx/common/vtx_types.h"
#include "vtx/reader/core/vtx_reader_facade.h"
#include "vtx/writer/policies/formatters/flatbuffers_vtx_policy.h"
#include "vtx/writer/policies/formatters/protobuff_vtx_policy.h"

namespace VTX {

    namespace {

        // Mirrors ChunkedFileSink::CompressIfBeneficial with the writer defaults:
        // level 10, tiny payloads left raw, raw kept when zstd does not shrink it.
        constexpr int kCompressionLevel = 10;
        constexpr size_t kCompressMinBytes = 512;
        // zstd frame magic, little-endian on disk: 28 B5 2F FD.
        constexpr unsigned char kZstdMagic[4] = {0x28, 0xB5, 0x2F, 0xFD};

        char ToLowerAscii(char c) {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }

        bool CharEquals(char a, char b, bool case_insensitive) {
            return case_insensitive ? ToLowerAscii(a) == ToLowerAscii(b) : a == b;
        }

        template <typename T>
        std::string ShortestDecimal(T value) {
            char buffer[64];
            const auto res = std::to_chars(buffer, buffer + sizeof(buffer), value);
            if (res.ec != std::errc {}) {
                return {};
            }
            return std::string(buffer, res.ptr);
        }

        bool IsMatchableField(const PropertyAddress& address) {
            if (address.container_type != FieldContainerType::None || address.index < 0) {
                return false;
            }
            switch (address.type_id) {
            case FieldType::Bool:
            case FieldType::Int32:
            case FieldType::Int64:
            case FieldType::Float:
            case FieldType::Double:
            case FieldType::String:
                return true;
            default:
                return false;
            }
        }

        // String form of one scalar property; false when the slot is absent.
        bool PropertyValueToString(const PropertyContainer& entity, const PropertyAddress& address, std::string& out) {
            const size_t i = static_cast<size_t>(address.index);
            switch (address.type_id) {
            case FieldType::Bool:
                if (i >= entity.bool_properties.size())
                    return false;
                out = entity.bool_properties[i] ? "true" : "false";
                return true;
            case FieldType::Int32:
                if (i >= entity.int32_properties.size())
                    return false;
                out = std::to_string(entity.int32_properties[i]);
                return true;
            case FieldType::Int64:
                if (i >= entity.int64_properties.size())
                    return false;
                out = std::to_string(entity.int64_properties[i]);
                return true;
            case FieldType::Float:
                if (i >= entity.float_properties.size())
                    return false;
                out = ShortestDecimal(entity.float_properties[i]);
                return true;
            case FieldType::Double:
                if (i >= entity.double_properties.size())
                    return false;
                out = ShortestDecimal(entity.double_properties[i]);
                return true;
            case FieldType::String:
                if (i >= entity.string_properties.size())
                    return false;
                out = entity.string_properties[i];
                return true;
            default:
                return false;
            }
        }

        const char* KindName(ReplayFilterRuleKind kind) {
            switch (kind) {
            case ReplayFilterRuleKind::UniqueId:
                return "unique id";
            case ReplayFilterRuleKind::StructName:
                return "struct";
            case ReplayFilterRuleKind::Property:
                return "property";
            }
            return "rule";
        }

        // ------------------------------------------------------------------
        // File plumbing (mirrors ChunkedFileSink framing: [magic][u32 header]
        // [u32 chunk]* [footer][u32][magic]; xxHash64 over the on-disk payload).
        // ------------------------------------------------------------------

        struct SourceLayout {
            uint64_t file_size = 0;
            uint64_t header_end = 0; ///< First byte after the header block.
            uint64_t footer_offset = 0;
            bool footer_compressed = false;
        };

        bool ReadBytesAt(std::ifstream& in, uint64_t offset, void* dst, size_t size) {
            in.clear();
            in.seekg(static_cast<std::streamoff>(offset));
            return static_cast<bool>(in.read(static_cast<char*>(dst), static_cast<std::streamsize>(size)));
        }

        bool IsZstdAt(std::ifstream& in, uint64_t offset, uint64_t available) {
            unsigned char magic[4];
            if (available < sizeof(magic) || !ReadBytesAt(in, offset, magic, sizeof(magic))) {
                return false;
            }
            return std::memcmp(magic, kZstdMagic, sizeof(magic)) == 0;
        }

        bool ProbeSource(std::ifstream& in, const std::string& expected_magic, SourceLayout& layout,
                         std::string& error) {
            in.clear();
            in.seekg(0, std::ios::end);
            const std::streamoff end = in.tellg();
            if (end < 0) {
                error = "Could not determine the source replay size.";
                return false;
            }
            layout.file_size = static_cast<uint64_t>(end);
            const size_t magic_size = expected_magic.size();
            // magic + header size + footer size + trailing magic.
            if (layout.file_size < magic_size * 2 + sizeof(uint32_t) * 2) {
                error = "The source replay is too small to be a valid .vtx file.";
                return false;
            }

            std::string magic(magic_size, '\0');
            if (!ReadBytesAt(in, 0, magic.data(), magic_size) || magic != expected_magic) {
                error = "The source replay does not start with the expected magic bytes.";
                return false;
            }
            uint32_t header_size = 0;
            if (!ReadBytesAt(in, magic_size, &header_size, sizeof(header_size))) {
                error = "Could not read the header size of the source replay.";
                return false;
            }
            layout.header_end = magic_size + sizeof(uint32_t) + header_size;

            std::string trailing(magic_size, '\0');
            if (!ReadBytesAt(in, layout.file_size - magic_size, trailing.data(), magic_size) ||
                trailing != expected_magic) {
                error = "The source replay has no footer (incomplete recording?). Repair it first.";
                return false;
            }
            uint32_t footer_size = 0;
            if (!ReadBytesAt(in, layout.file_size - magic_size - sizeof(uint32_t), &footer_size, sizeof(footer_size))) {
                error = "Could not read the footer size of the source replay.";
                return false;
            }
            const uint64_t footer_block = static_cast<uint64_t>(footer_size) + sizeof(uint32_t) + magic_size;
            if (footer_block > layout.file_size || layout.file_size - footer_block < layout.header_end) {
                error = "The source replay header and footer overlap; the file is corrupt.";
                return false;
            }
            layout.footer_offset = layout.file_size - footer_block;
            layout.footer_compressed = IsZstdAt(in, layout.footer_offset, footer_size);
            return true;
        }

        bool CopyRange(std::ifstream& in, std::ofstream& out, uint64_t offset, uint64_t bytes, std::string& error) {
            constexpr size_t kBufferSize = 4 * 1024 * 1024;
            std::vector<char> buffer(
                static_cast<size_t>(std::min<uint64_t>(std::max<uint64_t>(bytes, 1), kBufferSize)));
            in.clear();
            in.seekg(static_cast<std::streamoff>(offset));
            uint64_t remaining = bytes;
            while (remaining > 0) {
                const size_t step = static_cast<size_t>(std::min<uint64_t>(remaining, buffer.size()));
                if (!in.read(buffer.data(), static_cast<std::streamsize>(step))) {
                    error = "Read failed at offset " + std::to_string(offset + (bytes - remaining)) + ".";
                    return false;
                }
                if (!out.write(buffer.data(), static_cast<std::streamsize>(step))) {
                    error = "Write failed while copying the header block.";
                    return false;
                }
                remaining -= step;
            }
            return true;
        }

        std::string CompressIfBeneficial(std::string payload) {
            if (payload.size() < kCompressMinBytes) {
                return payload;
            }
            const size_t max_size = ZSTD_compressBound(payload.size());
            std::string compressed(max_size, '\0');
            const size_t compressed_size =
                ZSTD_compress(compressed.data(), max_size, payload.data(), payload.size(), kCompressionLevel);
            if (ZSTD_isError(compressed_size) || compressed_size >= payload.size()) {
                return payload;
            }
            compressed.resize(compressed_size);
            return compressed;
        }

        template <typename Policy>
        void RunFilter(const std::string& source_path, const std::string& dest_path, IVtxReaderFacade& reader,
                       const FileFooter& footer, const ReplayFilterMatcher& matcher, bool prune_events,
                       const ReplayFilterProgress& progress, ReplayFilterResult& result) {
            namespace fs = std::filesystem;
            std::string error;
            const auto fail = [&](std::string message) {
                result.error = std::move(message);
            };

            std::ifstream in(source_path, std::ios::binary);
            if (!in) {
                fail("Could not open the source replay: " + source_path);
                return;
            }
            SourceLayout layout;
            if (!ProbeSource(in, Policy::GetMagicBytes(), layout, error)) {
                fail(error);
                return;
            }

            // Chunks in frame order. Frame ranges and per-chunk compression are kept
            // exactly; only the bytes (and therefore offsets/sizes/checksums) change.
            std::vector<ChunkIndexEntry> chunks = footer.chunk_index;
            std::sort(chunks.begin(), chunks.end(),
                      [](const ChunkIndexEntry& a, const ChunkIndexEntry& b) { return a.start_frame < b.start_frame; });
            for (const ChunkIndexEntry& chunk : chunks) {
                if (chunk.end_frame < chunk.start_frame || chunk.chunk_size_bytes <= sizeof(uint32_t) ||
                    chunk.file_offset < layout.header_end ||
                    chunk.file_offset + chunk.chunk_size_bytes > layout.footer_offset) {
                    fail("The source replay seek table is inconsistent with the file layout (chunk " +
                         std::to_string(chunk.chunk_index) + ").");
                    return;
                }
            }
            if (!chunks.empty() && chunks.front().file_offset != layout.header_end) {
                fail("The source replay has an unexpected layout between its header and first chunk.");
                return;
            }

            std::ofstream out(dest_path, std::ios::binary | std::ios::trunc);
            if (!out) {
                fail("Could not create the output file: " + dest_path);
                return;
            }
            const auto abort_output = [&](std::string message) {
                out.close();
                std::error_code ec;
                fs::remove(dest_path, ec);
                fail(std::move(message));
            };

            // 1) Header block, verbatim (schema, uuid, name, recording timestamp,
            // metadata). Nothing in it depends on the entity payload.
            if (!CopyRange(in, out, 0, layout.header_end, error)) {
                abort_output(error);
                return;
            }

            const int32_t chunk_total = static_cast<int32_t>(chunks.size());
            if (progress && !progress(0, chunk_total)) {
                abort_output("Filter cancelled.");
                return;
            }

            const bool track_survivors = prune_events && !footer.events.empty();
            std::unordered_set<std::string> survivors;

            // 2) Every chunk re-serialized with the surviving entities.
            std::vector<ChunkIndexData> seek_table;
            seek_table.reserve(chunks.size());
            for (int32_t i = 0; i < chunk_total; ++i) {
                const ChunkIndexEntry& chunk = chunks[static_cast<size_t>(i)];
                std::vector<std::unique_ptr<typename Policy::FrameType>> frames;
                frames.reserve(static_cast<size_t>(chunk.end_frame - chunk.start_frame + 1));

                for (int32_t frame_index = chunk.start_frame; frame_index <= chunk.end_frame; ++frame_index) {
                    const Frame* source_frame = reader.GetFrameSync(frame_index);
                    if (!source_frame) {
                        abort_output("Could not read frame " + std::to_string(frame_index) +
                                     " from the source replay.");
                        return;
                    }
                    Frame frame(*source_frame);
                    for (Bucket& bucket : frame.GetMutableBuckets()) {
                        const size_t before = bucket.entities.size();
                        const size_t dropped = matcher.Apply(bucket, [&](const Bucket& b, size_t index) {
                            ++result.dropped_by_struct[matcher.TypeName(b.entities[index].entity_type_id)];
                        });
                        result.entities_seen += before;
                        result.entities_dropped += dropped;
                        result.entities_kept += before - dropped;
                        if (track_survivors) {
                            for (const std::string& id : bucket.unique_ids) {
                                survivors.insert(id);
                            }
                        }
                    }
                    frames.push_back(Policy::FromNative(std::move(frame)));
                }

                const bool compress =
                    IsZstdAt(in, chunk.file_offset + sizeof(uint32_t), chunk.chunk_size_bytes - sizeof(uint32_t));
                std::string payload = Policy::SerializeChunk(frames, i, compress);
                if (compress) {
                    payload = CompressIfBeneficial(std::move(payload));
                }

                ChunkIndexData entry;
                entry.chunk_index = i;
                entry.file_offset = static_cast<int64_t>(out.tellp());
                entry.chunk_size_bytes = static_cast<uint32_t>(payload.size() + sizeof(uint32_t));
                entry.start_frame = chunk.start_frame;
                entry.end_frame = chunk.end_frame;
                entry.checksum = XXH3_64bits(payload.data(), payload.size());

                const uint32_t payload_size = static_cast<uint32_t>(payload.size());
                if (!out.write(reinterpret_cast<const char*>(&payload_size), sizeof(payload_size)) ||
                    !out.write(payload.data(), static_cast<std::streamsize>(payload.size()))) {
                    abort_output("Write failed for chunk " + std::to_string(i) + ".");
                    return;
                }
                seek_table.push_back(entry);
                ++result.chunks_rewritten;

                if (progress && !progress(i + 1, chunk_total)) {
                    abort_output("Filter cancelled.");
                    return;
                }
            }

            // 3) Footer: same frames, same duration, same per-frame time table;
            // fresh seek table; events carried over (pruned to surviving entities).
            std::vector<int64_t> game_times(footer.times.game_time.begin(), footer.times.game_time.end());
            std::vector<int64_t> created_utc(footer.times.created_utc.begin(), footer.times.created_utc.end());
            std::vector<int32_t> gaps(footer.times.gaps.begin(), footer.times.gaps.end());
            std::vector<int32_t> segments(footer.times.segments.begin(), footer.times.segments.end());

            std::vector<TimelineEvent> events;
            events.reserve(footer.events.size());
            for (const TimelineEvent& event : footer.events) {
                if (track_survivors && !event.entity_unique_id.empty() && !survivors.contains(event.entity_unique_id)) {
                    ++result.events_dropped;
                    continue;
                }
                events.push_back(event);
                ++result.events_kept;
            }

            SessionFooter session_footer;
            session_footer.total_frames = footer.total_frames;
            session_footer.duration_seconds = footer.duration_seconds;
            const bool has_times = !game_times.empty() || !created_utc.empty() || !gaps.empty() || !segments.empty();
            if (has_times) {
                session_footer.game_times = &game_times;
                session_footer.created_utc = &created_utc;
                session_footer.gaps = &gaps;
                session_footer.segments = &segments;
            }
            session_footer.events = &events;

            std::string footer_payload = Policy::SerializeFooter(seek_table, session_footer);
            if (layout.footer_compressed) {
                footer_payload = CompressIfBeneficial(std::move(footer_payload));
            }
            const uint32_t footer_size = static_cast<uint32_t>(footer_payload.size());
            const std::string magic = Policy::GetMagicBytes();
            if (!out.write(footer_payload.data(), static_cast<std::streamsize>(footer_payload.size())) ||
                !out.write(reinterpret_cast<const char*>(&footer_size), sizeof(footer_size)) ||
                !out.write(magic.data(), static_cast<std::streamsize>(magic.size()))) {
                abort_output("Write failed while finishing the footer.");
                return;
            }
            out.flush();
            if (!out) {
                abort_output("Flush failed for the output file.");
                return;
            }
            out.close();

            result.total_frames = footer.total_frames;
            std::error_code ec;
            result.output_bytes = fs::file_size(dest_path, ec);
        }

    } // namespace

    // ----------------------------------------------------------------------
    // Rules
    // ----------------------------------------------------------------------

    ReplayFilterRule ReplayFilterRule::UniqueId(std::string pattern, bool case_insensitive) {
        ReplayFilterRule rule;
        rule.kind = ReplayFilterRuleKind::UniqueId;
        rule.pattern = std::move(pattern);
        rule.case_insensitive = case_insensitive;
        return rule;
    }

    ReplayFilterRule ReplayFilterRule::StructName(std::string pattern, bool case_insensitive) {
        ReplayFilterRule rule;
        rule.kind = ReplayFilterRuleKind::StructName;
        rule.pattern = std::move(pattern);
        rule.case_insensitive = case_insensitive;
        return rule;
    }

    ReplayFilterRule ReplayFilterRule::Property(std::string struct_name, std::string field_name,
                                                std::string value_pattern, bool case_insensitive) {
        ReplayFilterRule rule;
        rule.kind = ReplayFilterRuleKind::Property;
        rule.struct_name = std::move(struct_name);
        rule.field_name = std::move(field_name);
        rule.pattern = std::move(value_pattern);
        rule.case_insensitive = case_insensitive;
        return rule;
    }

    bool GlobMatch(std::string_view pattern, std::string_view text, bool case_insensitive) {
        size_t p = 0;
        size_t t = 0;
        size_t star = std::string_view::npos;
        size_t star_text = 0;
        while (t < text.size()) {
            if (p < pattern.size() && (pattern[p] == '?' || CharEquals(pattern[p], text[t], case_insensitive))) {
                ++p;
                ++t;
            } else if (p < pattern.size() && pattern[p] == '*') {
                star = p++;
                star_text = t;
            } else if (star != std::string_view::npos) {
                p = star + 1;
                t = ++star_text;
            } else {
                return false;
            }
        }
        while (p < pattern.size() && pattern[p] == '*') {
            ++p;
        }
        return p == pattern.size();
    }

    // ----------------------------------------------------------------------
    // Matcher
    // ----------------------------------------------------------------------

    ReplayFilterMatcher::ReplayFilterMatcher(const ReplayFilterSpec& spec, const PropertyAddressCache& cache)
        : mode_(spec.mode) {
        for (const auto& [type_id, struct_cache] : cache.structs) {
            type_names_[type_id] = struct_cache.name;
        }
        if (spec.rules.empty()) {
            error_ = "The filter has no rules; add at least one unique id, struct or property rule.";
            return;
        }
        for (const ReplayFilterRule& rule : spec.rules) {
            if (rule.pattern.empty()) {
                error_ = std::string("A ") + KindName(rule.kind) + " rule has an empty pattern.";
                return;
            }
            ResolvedRule resolved;
            resolved.kind = rule.kind;
            resolved.pattern = rule.pattern;
            resolved.case_insensitive = rule.case_insensitive;
            if (rule.kind == ReplayFilterRuleKind::Property) {
                if (rule.field_name.empty()) {
                    error_ = "A property rule has no field name.";
                    return;
                }
                if (!rule.struct_name.empty()) {
                    const auto id_it = cache.name_to_id.find(rule.struct_name);
                    const auto struct_it =
                        id_it == cache.name_to_id.end() ? cache.structs.end() : cache.structs.find(id_it->second);
                    if (struct_it == cache.structs.end()) {
                        error_ = "Property rule: struct '" + rule.struct_name + "' is not in the schema.";
                        return;
                    }
                    const auto field_it = struct_it->second.properties.find(rule.field_name);
                    if (field_it == struct_it->second.properties.end()) {
                        error_ =
                            "Property rule: struct '" + rule.struct_name + "' has no field '" + rule.field_name + "'.";
                        return;
                    }
                    if (!IsMatchableField(field_it->second)) {
                        error_ = "Property rule: '" + rule.struct_name + "." + rule.field_name +
                                 "' is not a scalar bool/int/float/double/string field.";
                        return;
                    }
                    resolved.slots[id_it->second] = field_it->second;
                } else {
                    for (const auto& [type_id, struct_cache] : cache.structs) {
                        const auto field_it = struct_cache.properties.find(rule.field_name);
                        if (field_it != struct_cache.properties.end() && IsMatchableField(field_it->second)) {
                            resolved.slots[type_id] = field_it->second;
                        }
                    }
                    if (resolved.slots.empty()) {
                        error_ = "Property rule: no struct declares a scalar field named '" + rule.field_name + "'.";
                        return;
                    }
                }
            }
            rules_.push_back(std::move(resolved));
        }
    }

    std::string ReplayFilterMatcher::TypeName(int32_t type_id) const {
        const auto it = type_names_.find(type_id);
        if (it != type_names_.end()) {
            return it->second;
        }
        return "type#" + std::to_string(type_id);
    }

    bool ReplayFilterMatcher::RuleMatches(const ResolvedRule& rule, const Bucket& bucket, size_t index) const {
        switch (rule.kind) {
        case ReplayFilterRuleKind::UniqueId: {
            const std::string_view id =
                index < bucket.unique_ids.size() ? std::string_view(bucket.unique_ids[index]) : std::string_view();
            return GlobMatch(rule.pattern, id, rule.case_insensitive);
        }
        case ReplayFilterRuleKind::StructName: {
            const auto it = type_names_.find(bucket.entities[index].entity_type_id);
            if (it == type_names_.end()) {
                return false;
            }
            return GlobMatch(rule.pattern, it->second, rule.case_insensitive);
        }
        case ReplayFilterRuleKind::Property: {
            const PropertyContainer& entity = bucket.entities[index];
            const auto slot = rule.slots.find(entity.entity_type_id);
            if (slot == rule.slots.end()) {
                return false;
            }
            std::string value;
            if (!PropertyValueToString(entity, slot->second, value)) {
                return false;
            }
            return GlobMatch(rule.pattern, value, rule.case_insensitive);
        }
        }
        return false;
    }

    bool ReplayFilterMatcher::Matches(const Bucket& bucket, size_t index) const {
        if (index >= bucket.entities.size()) {
            return false;
        }
        for (const ResolvedRule& rule : rules_) {
            if (RuleMatches(rule, bucket, index)) {
                return true;
            }
        }
        return false;
    }

    size_t ReplayFilterMatcher::Apply(Bucket& bucket, const std::function<void(const Bucket&, size_t)>& on_drop) const {
        const size_t count = bucket.entities.size();
        size_t write = 0;
        for (size_t read = 0; read < count; ++read) {
            if (!Keeps(bucket, read)) {
                if (on_drop) {
                    on_drop(bucket, read);
                }
                continue;
            }
            if (write != read) {
                bucket.entities[write] = std::move(bucket.entities[read]);
                if (read < bucket.unique_ids.size() && write < bucket.unique_ids.size()) {
                    bucket.unique_ids[write] = std::move(bucket.unique_ids[read]);
                }
            }
            ++write;
        }
        if (write == count) {
            return 0;
        }
        bucket.entities.resize(write);
        if (bucket.unique_ids.size() > write) {
            bucket.unique_ids.resize(write);
        }
        bucket.type_ranges.clear();
        return count - write;
    }

    // ----------------------------------------------------------------------
    // Entry point
    // ----------------------------------------------------------------------

    ReplayFilterResult FilterReplayFile(const std::string& source_path, const std::string& dest_path,
                                        const ReplayFilterSpec& spec, const ReplayFilterProgress& progress) {
        namespace fs = std::filesystem;
        const auto started = std::chrono::steady_clock::now();
        ReplayFilterResult result;
        const auto finish = [&]() -> ReplayFilterResult& {
            result.elapsed_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            return result;
        };
        const auto fail = [&](std::string message) -> ReplayFilterResult& {
            result.error = std::move(message);
            return finish();
        };

        if (spec.rules.empty()) {
            return fail("The filter has no rules; add at least one unique id, struct or property rule.");
        }
        if (source_path.empty() || dest_path.empty()) {
            return fail("Source and destination paths are required.");
        }
        std::error_code ec;
        if (!fs::is_regular_file(source_path, ec)) {
            return fail("Source replay does not exist: " + source_path);
        }
        const fs::path source_abs = fs::weakly_canonical(fs::path(source_path), ec);
        const fs::path dest_abs = fs::weakly_canonical(fs::path(dest_path), ec);
        if (source_abs == dest_abs || (fs::exists(dest_path, ec) && fs::equivalent(source_path, dest_path, ec))) {
            return fail("Destination must be a different file than the source.");
        }

        ReaderContext context = OpenReplayFile(source_path);
        if (!context.Loaded()) {
            return fail("Could not open the source replay: " + context.GetError().message);
        }
        if (!context.WaitUntilReady()) {
            return fail("The source replay failed to load: " + context.GetReadyError().message);
        }

        const FileFooter footer = context->GetFooter();
        const PropertyAddressCache cache = context->GetPropertyAddressCache();
        const ReplayFilterMatcher matcher(spec, cache);
        if (!matcher.valid()) {
            return fail(matcher.error());
        }
        result.source_bytes = fs::file_size(source_path, ec);

        switch (context.format) {
        case VtxFormat::FlatBuffers:
            RunFilter<FlatBuffersVtxPolicy>(source_path, dest_path, *context.reader, footer, matcher, spec.prune_events,
                                            progress, result);
            break;
        case VtxFormat::Protobuf:
            RunFilter<ProtobufVtxPolicy>(source_path, dest_path, *context.reader, footer, matcher, spec.prune_events,
                                         progress, result);
            break;
        default:
            return fail("Unknown replay format.");
        }
        return finish();
    }

} // namespace VTX
