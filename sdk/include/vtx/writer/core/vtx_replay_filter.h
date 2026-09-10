/**
 * @file vtx_replay_filter.h
 * @brief Write a copy of a .vtx replay with some entities removed.
 *
 * @details FilterReplayFile() rewrites an existing replay into a new file, keeping
 * every frame but dropping (or keeping only) the entities selected by a small rule
 * set: whole buckets, unique id globs, schema struct names, or the value of one
 * scalar property. The header block (schema, uuid, name, recording timestamp,
 * metadata) is copied byte for byte, every chunk is re-serialized with the
 * surviving entities, and the footer is rebuilt with the same frame count, duration
 * and per-frame time table; only the seek table (offsets, sizes, checksums)
 * changes. Timeline events are carried over, optionally pruned to the entities
 * that still exist.
 *
 *   VTX::ReplayFilterSpec spec;
 *   spec.mode = VTX::ReplayFilterMode::Drop;
 *   spec.rules.push_back(VTX::ReplayFilterRule::Bucket("SystemHealth"));
 *   spec.rules.push_back(VTX::ReplayFilterRule::StructName("Vehicle*"));
 *   spec.rules.push_back(VTX::ReplayFilterRule::UniqueId("keyboard"));
 *   const auto r = VTX::FilterReplayFile("in.vtx", "out.vtx", spec);
 *   if (!r.ok()) { ... r.error ... }
 *
 * Buckets are positional (the schema names them by index), so a dropped bucket is
 * emptied in every frame rather than removed: readers still see it, with no
 * entities.
 *
 * Requires the reader module (VTX_BUILD_READER): the source replay is read through
 * the standard reader, so the function is only compiled when vtx_reader is built.
 *
 * @author Zenos Interactive
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "vtx/common/vtx_property_cache.h"
#include "vtx/common/vtx_types.h"

namespace VTX {

    /// How the rule set selects the entities that survive.
    enum class ReplayFilterMode : uint8_t {
        Drop, ///< Blacklist: an entity matching any rule is removed.
        Keep, ///< Whitelist: only entities matching at least one rule are kept.
    };

    enum class ReplayFilterRuleKind : uint8_t {
        UniqueId,   ///< Glob against the entity's unique id (Bucket::unique_ids).
        StructName, ///< Glob against the schema struct name of the entity's type id.
        Property,   ///< Glob against the string form of one scalar property value.
        Bucket,     ///< Every entity of the buckets whose schema name matches the glob (or whose index equals it).
    };

    /**
     * @brief One selection rule. Patterns are globs: '*' matches any run of
     *        characters, '?' exactly one; everything else is literal.
     * @details Property rules resolve `struct_name.field_name` through the file's
     *          schema; an empty struct_name means "every struct declaring that
     *          field". Supported field types: Bool ("true"/"false"), Int32, Int64,
     *          Float, Double (shortest round-trip decimal) and String. Arrays, maps,
     *          nested structs and the compound value types are not matchable.
     *          Bucket rules resolve against the schema's bucket names; a pattern made
     *          only of digits selects the bucket at that index instead (also for
     *          buckets the schema does not name). A bucket rule matching no bucket
     *          is an error.
     */
    struct ReplayFilterRule {
        ReplayFilterRuleKind kind = ReplayFilterRuleKind::UniqueId;
        std::string pattern;           ///< Glob matched against the id / struct name / value / bucket name.
        std::string struct_name;       ///< Property only. Empty = any struct with that field.
        std::string field_name;        ///< Property only.
        bool case_insensitive = false; ///< ASCII case-insensitive match.

        static ReplayFilterRule UniqueId(std::string pattern, bool case_insensitive = false);
        static ReplayFilterRule StructName(std::string pattern, bool case_insensitive = false);
        static ReplayFilterRule Property(std::string struct_name, std::string field_name, std::string value_pattern,
                                         bool case_insensitive = false);
        static ReplayFilterRule Bucket(std::string pattern, bool case_insensitive = false);
    };

    struct ReplayFilterSpec {
        ReplayFilterMode mode = ReplayFilterMode::Drop;
        std::vector<ReplayFilterRule> rules; ///< Combined as a union: an entity matches if any rule matches.
        bool prune_events = true; ///< Drop timeline events whose entity_unique_id no longer appears in any frame.
    };

    struct ReplayFilterResult {
        std::string error; ///< Non-empty on failure (the destination is removed).

        int32_t total_frames = 0;     ///< Frames in the output (same as the source).
        int32_t chunks_rewritten = 0; ///< Chunks re-serialized (all of them).
        uint64_t entities_seen = 0;   ///< Top-level bucket entities visited, summed over all frames.
        uint64_t entities_kept = 0;
        uint64_t entities_dropped = 0;
        uint64_t events_kept = 0;
        uint64_t events_dropped = 0;
        uint64_t source_bytes = 0;
        uint64_t output_bytes = 0;
        double elapsed_seconds = 0.0;
        /// Dropped entity count per struct name (entities of an unknown type id are
        /// listed as "type#<id>").
        std::map<std::string, uint64_t> dropped_by_struct;
        /// Dropped entity count per bucket (schema name, or "bucket#<index>" when the
        /// schema does not name it).
        std::map<std::string, uint64_t> dropped_by_bucket;

        bool ok() const { return error.empty(); }
    };

    /// Called before the first chunk (0, total) and after every rewritten chunk;
    /// return false to cancel (the partial destination is removed and the result
    /// carries an error).
    using ReplayFilterProgress = std::function<bool(int32_t chunks_done, int32_t chunks_total)>;

    /// Glob matcher used by the rules ('*' any run, '?' one character).
    bool GlobMatch(std::string_view pattern, std::string_view text, bool case_insensitive = false);

    /**
     * @brief Resolves a rule set against a file's schema and evaluates entities.
     * @details Built once per file; used by FilterReplayFile() and available to
     *          callers that want to preview a spec on in-memory frames. Every query
     *          takes the bucket's index within the frame, which is what bucket rules
     *          and the schema's bucket names key on.
     */
    class ReplayFilterMatcher {
    public:
        ReplayFilterMatcher(const ReplayFilterSpec& spec, const PropertyAddressCache& cache);

        bool valid() const { return error_.empty(); }
        const std::string& error() const { return error_; }
        ReplayFilterMode mode() const { return mode_; }

        /// True if entity @p index of @p bucket (the frame's bucket number
        /// @p bucket_index) matches at least one rule.
        bool Matches(int32_t bucket_index, const Bucket& bucket, size_t index) const;
        /// Applies the mode: true if the entity survives the filter.
        bool Keeps(int32_t bucket_index, const Bucket& bucket, size_t index) const {
            return Matches(bucket_index, bucket, index) == (mode_ == ReplayFilterMode::Keep);
        }
        /// True if a bucket rule selects the whole bucket @p bucket_index.
        bool BucketMatches(int32_t bucket_index) const;

        /// Struct name for a type id ("type#<id>" when the schema does not know it).
        std::string TypeName(int32_t type_id) const;
        /// Schema name of a bucket ("bucket#<index>" when the schema does not name it).
        std::string BucketName(int32_t bucket_index) const;

        /**
         * @brief Removes every entity Keeps() rejects, in place.
         * @param bucket_index The bucket's number within its frame.
         * @param on_drop Optional; called with the bucket and the index of each
         *        dropped entity before it is removed.
         * @return Number of removed entities. When anything was removed the bucket's
         *         type_ranges are cleared (serialization rebuilds them from
         *         entity_type_id).
         */
        size_t Apply(int32_t bucket_index, Bucket& bucket,
                     const std::function<void(const Bucket&, size_t)>& on_drop = nullptr) const;

    private:
        struct ResolvedRule {
            ReplayFilterRuleKind kind = ReplayFilterRuleKind::UniqueId;
            std::string pattern;
            bool case_insensitive = false;
            std::unordered_map<int32_t, PropertyAddress> slots; ///< Property: type id -> address.
            std::vector<int32_t> bucket_indices;                ///< Bucket: schema buckets matched by name.
            int32_t bucket_ordinal = -1;                        ///< Bucket: numeric pattern, matched by index.
        };

        bool RuleMatches(const ResolvedRule& rule, int32_t bucket_index, const Bucket& bucket, size_t index) const;

        ReplayFilterMode mode_ = ReplayFilterMode::Drop;
        std::vector<ResolvedRule> rules_;
        std::unordered_map<int32_t, std::string> type_names_;
        std::vector<std::string> bucket_names_;
        std::string error_;
    };

    /**
     * @brief Writes @p dest_path as a copy of @p source_path with the entities
     *        selected by @p spec removed (Drop) or exclusively kept (Keep).
     * @details The source is opened with the standard reader (both FlatBuffers and
     *          Protobuf files are supported; the output keeps the source format).
     *          Header bytes are copied verbatim, chunks keep their frame ranges and
     *          per-chunk compression, the footer keeps total_frames, duration and the
     *          per-frame time table. Nested containers inside a kept entity are never
     *          touched; buckets that end up empty (including dropped buckets) stay in
     *          place, since bucket index is positional. dest_path must differ from
     *          source_path.
     */
    ReplayFilterResult FilterReplayFile(const std::string& source_path, const std::string& dest_path,
                                        const ReplayFilterSpec& spec, const ReplayFilterProgress& progress = nullptr);

} // namespace VTX
