
/**
 * @file vtx_types_helper.h
 * @brief Helper fucntions to interact with vtx property container
 * @author Zenos Interactive
 */
#pragma once
#include "vtx/common/vtx_types.h"
#include "vtx/common/readers/schema_reader/game_schema_types.h"
#include <cstring>
#include <string>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <xxh3.h>

using int64 = std::int64_t;
namespace VTX {
    namespace Helpers {

        // Grow-only: ensures each FlatArray holds at least the schema-declared number of
        // (possibly empty) subarrays. Never shrinks; leaves scalars and maps untouched.
        // Reused by ResizeContainerToMaxIndices (write/ingest) and by the reader to
        // restore declared-but-empty array fields that were not serialized.
        inline void EnsureDeclaredArrays(PropertyContainer& container, const std::vector<int32_t>& array_max_indices) {
            auto GetNeeded = [&](FieldType type) -> int32_t {
                size_t typeIdx = static_cast<size_t>(type);
                return (typeIdx < array_max_indices.size()) ? array_max_indices[typeIdx] : 0;
            };

            if (int32_t n = GetNeeded(FieldType::Bool))
                container.bool_arrays.EnsureSubArrayCount(n);
            if (int32_t n = GetNeeded(FieldType::Int32))
                container.int32_arrays.EnsureSubArrayCount(n);
            if (int32_t n = GetNeeded(FieldType::Int64))
                container.int64_arrays.EnsureSubArrayCount(n);
            if (int32_t n = GetNeeded(FieldType::Float))
                container.float_arrays.EnsureSubArrayCount(n);
            if (int32_t n = GetNeeded(FieldType::Double))
                container.double_arrays.EnsureSubArrayCount(n);
            if (int32_t n = GetNeeded(FieldType::String))
                container.string_arrays.EnsureSubArrayCount(n);

            if (int32_t n = GetNeeded(FieldType::Vector))
                container.vector_arrays.EnsureSubArrayCount(n);
            if (int32_t n = GetNeeded(FieldType::Quat))
                container.quat_arrays.EnsureSubArrayCount(n);
            if (int32_t n = GetNeeded(FieldType::Transform))
                container.transform_arrays.EnsureSubArrayCount(n);
            if (int32_t n = GetNeeded(FieldType::FloatRange))
                container.range_arrays.EnsureSubArrayCount(n);

            if (int32_t n = GetNeeded(FieldType::Struct))
                container.any_struct_arrays.EnsureSubArrayCount(n);
        }

        inline void ResizeContainerToMaxIndices(PropertyContainer& container,
                                                const std::vector<int32_t>& type_max_indices,
                                                const std::vector<int32_t>& array_max_indices = {},
                                                int32_t map_max_index = 0) {
            auto GetNeeded = [&](FieldType type) -> int32_t {
                size_t typeIdx = static_cast<size_t>(type);
                return (typeIdx < type_max_indices.size()) ? type_max_indices[typeIdx] : 0;
            };

            // --- Scalars: default-initialized slot per declared field. ---
            if (int32_t n = GetNeeded(FieldType::Bool))
                container.bool_properties.resize(n);
            if (int32_t n = GetNeeded(FieldType::Int32))
                container.int32_properties.resize(n);
            if (int32_t n = GetNeeded(FieldType::Int64))
                container.int64_properties.resize(n);
            if (int32_t n = GetNeeded(FieldType::Float))
                container.float_properties.resize(n);
            if (int32_t n = GetNeeded(FieldType::Double))
                container.double_properties.resize(n);
            if (int32_t n = GetNeeded(FieldType::String))
                container.string_properties.resize(n);

            if (int32_t n = GetNeeded(FieldType::Vector))
                container.vector_properties.resize(n);
            if (int32_t n = GetNeeded(FieldType::Quat))
                container.quat_properties.resize(n);
            if (int32_t n = GetNeeded(FieldType::Transform))
                container.transform_properties.resize(n);
            if (int32_t n = GetNeeded(FieldType::FloatRange))
                container.range_properties.resize(n);

            if (int32_t n = GetNeeded(FieldType::Struct))
                container.any_struct_properties.resize(n);

            // --- Arrays: one empty subarray per declared array field of each type. ---
            EnsureDeclaredArrays(container, array_max_indices);

            // --- Maps: all Struct-valued, a single contiguous index space. ---
            if (map_max_index > 0)
                container.map_properties.resize(static_cast<size_t>(map_max_index));
        }

        inline void PreparePropertyContainer(PropertyContainer& container, const SchemaStruct& schema) {
            ResizeContainerToMaxIndices(container, schema.type_max_indices, schema.array_max_indices,
                                        schema.map_max_index);
        }

        /// @brief Makes @p container what PreparePropertyContainer makes of a default one for @p schema -- every
        ///        declared slot present, holding its default value or an empty subarray, nothing else -- while
        ///        keeping the memory it holds, so a writer can refill one container frame after frame. Its type
        ///        id is @p type_id and its content hash 0. Nested containers in struct slots are KEPT (only the
        ///        slot count is the schema's), so the caller must refill every struct slot, as a writer setting
        ///        every field does; a slot it left alone would keep the previous frame's container.
        inline void ResetToPrepared(PropertyContainer& container, const SchemaStruct& schema, int32_t type_id) {
            auto needed = [](const std::vector<int32_t>& max, FieldType type) -> size_t {
                const size_t index = static_cast<size_t>(type);
                return index < max.size() && max[index] > 0 ? static_cast<size_t>(max[index]) : 0;
            };
            const auto& scalars = schema.type_max_indices;
            container.entity_type_id = type_id;
            container.content_hash = 0;
            container.bool_properties.assign(needed(scalars, FieldType::Bool), false);
            container.int32_properties.assign(needed(scalars, FieldType::Int32), 0);
            container.int64_properties.assign(needed(scalars, FieldType::Int64), 0);
            container.float_properties.assign(needed(scalars, FieldType::Float), 0.0f);
            container.double_properties.assign(needed(scalars, FieldType::Double), 0.0);
            container.string_properties.resize(needed(scalars, FieldType::String));
            for (auto& text : container.string_properties)
                text.clear();
            container.vector_properties.assign(needed(scalars, FieldType::Vector), Vector {});
            container.quat_properties.assign(needed(scalars, FieldType::Quat), Quat {});
            container.transform_properties.assign(needed(scalars, FieldType::Transform), Transform {});
            container.range_properties.assign(needed(scalars, FieldType::FloatRange), FloatRange {});
            container.any_struct_properties.resize(needed(scalars, FieldType::Struct));

            // Every array field an empty subarray; the arrays no field declares, empty.
            const auto& arrays = schema.array_max_indices;
            auto reset = [&](auto& flat, FieldType type) {
                flat.data.clear();
                flat.offsets.assign(needed(arrays, type), 0u);
            };
            reset(container.bool_arrays, FieldType::Bool);
            reset(container.int32_arrays, FieldType::Int32);
            reset(container.int64_arrays, FieldType::Int64);
            reset(container.float_arrays, FieldType::Float);
            reset(container.double_arrays, FieldType::Double);
            reset(container.string_arrays, FieldType::String);
            reset(container.vector_arrays, FieldType::Vector);
            reset(container.quat_arrays, FieldType::Quat);
            reset(container.transform_arrays, FieldType::Transform);
            reset(container.range_arrays, FieldType::FloatRange);
            reset(container.any_struct_arrays, FieldType::Struct);
            container.byte_array_properties.Clear();
            container.map_arrays.Clear();
            container.map_properties.clear();
            if (schema.map_max_index > 0)
                container.map_properties.resize(static_cast<size_t>(schema.map_max_index));
        }

        inline uint64_t CalculateContainerHash(const PropertyContainer& container);

        namespace detail {
            // XXH3's streaming digest depends only on the bytes fed to it, never on how they are split
            // across updates. A container is mostly small fields (one byte per bool, one update per
            // string), so they are gathered in a buffer and fed in large updates: the same hash, far
            // fewer calls into XXH3.
            struct BufferedXxh3 {
                alignas(64) XXH_INLINE_XXH3_state_t state;
                unsigned char buffer[512];
                size_t used = 0;

                BufferedXxh3() { XXH3_64bits_reset(&state); }

                void Flush() {
                    if (used > 0) {
                        XXH3_64bits_update(&state, buffer, used);
                        used = 0;
                    }
                }

                void Update(const void* data, size_t size) {
                    if (size == 0 || data == nullptr)
                        return;
                    if (size > sizeof(buffer) - used) {
                        Flush();
                        if (size >= sizeof(buffer)) {
                            XXH3_64bits_update(&state, data, size);
                            return;
                        }
                    }
                    std::memcpy(buffer + used, data, size);
                    used += size;
                }

                uint64_t Digest() {
                    Flush();
                    return XXH3_64bits_digest(&state);
                }
            };

            // The one definition of a container's hash. sub_hash gives a nested container's hash: computed
            // (CalculateContainerHash) or already stored in its content_hash (StoreContainerHashes).
            template <typename SubHash>
            uint64_t HashContainer(const PropertyContainer& container, SubHash&& sub_hash) {
                // Per-call state: nested containers are hashed by their own call.
                BufferedXxh3 hasher;
                auto hash_update = [&](const void* data, size_t size) {
                    hasher.Update(data, size);
                };

                hash_update(&container.entity_type_id, sizeof(container.entity_type_id));

                //special case, std::vector<bool> does not have .data(), fast loop instead
                for (const bool& b : container.bool_properties) {
                    uint8_t val = b ? 1 : 0;
                    hash_update(&val, sizeof(val));
                }

                hash_update(container.int32_properties.data(), container.int32_properties.size() * sizeof(int32_t));
                hash_update(container.int64_properties.data(), container.int64_properties.size() * sizeof(int64_t));
                hash_update(container.float_properties.data(), container.float_properties.size() * sizeof(float));
                hash_update(container.double_properties.data(), container.double_properties.size() * sizeof(double));

                for (const auto& str : container.string_properties) {
                    hash_update(str.data(), str.size());
                }

                hash_update(container.transform_properties.data(),
                            container.transform_properties.size() * sizeof(Transform));
                hash_update(container.vector_properties.data(), container.vector_properties.size() * sizeof(Vector));
                hash_update(container.quat_properties.data(), container.quat_properties.size() * sizeof(Quat));
                hash_update(container.range_properties.data(), container.range_properties.size() * sizeof(FloatRange));


                //arrays

                if (!container.byte_array_properties.data.empty()) {
                    hash_update(container.byte_array_properties.data.data(),
                                container.byte_array_properties.data.size() * sizeof(uint8_t));
                    hash_update(container.byte_array_properties.offsets.data(),
                                container.byte_array_properties.offsets.size() * sizeof(uint32_t));
                }

                if (!container.int32_arrays.data.empty()) {
                    hash_update(container.int32_arrays.data.data(),
                                container.int32_arrays.data.size() * sizeof(int32_t));
                    hash_update(container.int32_arrays.offsets.data(),
                                container.int32_arrays.offsets.size() * sizeof(uint32_t));
                }

                if (!container.int64_arrays.data.empty()) {
                    hash_update(container.int64_arrays.data.data(),
                                container.int64_arrays.data.size() * sizeof(int64_t));
                    hash_update(container.int64_arrays.offsets.data(),
                                container.int64_arrays.offsets.size() * sizeof(uint32_t));
                }

                if (!container.float_arrays.data.empty()) {
                    hash_update(container.float_arrays.data.data(), container.float_arrays.data.size() * sizeof(float));
                    hash_update(container.float_arrays.offsets.data(),
                                container.float_arrays.offsets.size() * sizeof(uint32_t));
                }

                if (!container.double_arrays.data.empty()) {
                    hash_update(container.double_arrays.data.data(),
                                container.double_arrays.data.size() * sizeof(double));
                    hash_update(container.double_arrays.offsets.data(),
                                container.double_arrays.offsets.size() * sizeof(uint32_t));
                }

                // Nested structs (scalar)
                for (const auto& sc : container.any_struct_properties) {
                    uint64_t sub = sub_hash(sc);
                    hash_update(&sub, sizeof(sub));
                }

                // Nested struct arrays
                if (!container.any_struct_arrays.data.empty()) {
                    for (const auto& sc : container.any_struct_arrays.data) {
                        uint64_t sub = sub_hash(sc);
                        hash_update(&sub, sizeof(sub));
                    }
                    hash_update(container.any_struct_arrays.offsets.data(),
                                container.any_struct_arrays.offsets.size() * sizeof(uint32_t));
                }

                // Maps (scalar)
                for (const auto& mc : container.map_properties) {
                    for (const auto& key : mc.keys) {
                        hash_update(key.data(), key.size());
                    }
                    for (const auto& val : mc.values) {
                        uint64_t sub = sub_hash(val);
                        hash_update(&sub, sizeof(sub));
                    }
                }

                // Map arrays
                if (!container.map_arrays.data.empty()) {
                    for (const auto& mc : container.map_arrays.data) {
                        for (const auto& key : mc.keys) {
                            hash_update(key.data(), key.size());
                        }
                        for (const auto& val : mc.values) {
                            uint64_t sub = sub_hash(val);
                            hash_update(&sub, sizeof(sub));
                        }
                    }
                    hash_update(container.map_arrays.offsets.data(),
                                container.map_arrays.offsets.size() * sizeof(uint32_t));
                }

                if (!container.vector_arrays.data.empty()) {
                    hash_update(container.vector_arrays.data.data(),
                                container.vector_arrays.data.size() * sizeof(VTX::Vector));
                    hash_update(container.vector_arrays.offsets.data(),
                                container.vector_arrays.offsets.size() * sizeof(uint32_t));
                }

                if (!container.quat_arrays.data.empty()) {
                    hash_update(container.quat_arrays.data.data(),
                                container.quat_arrays.data.size() * sizeof(VTX::Quat));
                    hash_update(container.quat_arrays.offsets.data(),
                                container.quat_arrays.offsets.size() * sizeof(uint32_t));
                }

                if (!container.transform_arrays.data.empty()) {
                    hash_update(container.transform_arrays.data.data(),
                                container.transform_arrays.data.size() * sizeof(VTX::Transform));
                    hash_update(container.transform_arrays.offsets.data(),
                                container.transform_arrays.offsets.size() * sizeof(uint32_t));
                }

                if (!container.range_arrays.data.empty()) {
                    hash_update(container.range_arrays.data.data(),
                                container.range_arrays.data.size() * sizeof(VTX::FloatRange));
                    hash_update(container.range_arrays.offsets.data(),
                                container.range_arrays.offsets.size() * sizeof(uint32_t));
                }

                if (!container.bool_arrays.data.empty()) {
                    hash_update(container.bool_arrays.data.data(), container.bool_arrays.data.size() * sizeof(uint8_t));
                    hash_update(container.bool_arrays.offsets.data(),
                                container.bool_arrays.offsets.size() * sizeof(uint32_t));
                }

                if (!container.string_arrays.data.empty()) {
                    for (const auto& str : container.string_arrays.data) {
                        hash_update(str.data(), str.size());
                    }
                    hash_update(container.string_arrays.offsets.data(),
                                container.string_arrays.offsets.size() * sizeof(uint32_t));
                }


                return hasher.Digest();
            }
        } // namespace detail

        /// @brief The content hash of a container and, recursively, of everything nested in it.
        inline uint64_t CalculateContainerHash(const PropertyContainer& container) {
            return detail::HashContainer(
                container, [](const PropertyContainer& nested) { return CalculateContainerHash(nested); });
        }

        /// @brief Stores the content hash of every container in the tree, nested ones first, hashing each
        ///        container exactly once; returns the root's. Equal to CalculateContainerHash(container).
        inline uint64_t StoreContainerHashes(PropertyContainer& container) {
            for (auto& nested : container.any_struct_properties)
                StoreContainerHashes(nested);
            for (auto& nested : container.any_struct_arrays.data)
                StoreContainerHashes(nested);
            for (auto& map : container.map_properties)
                for (auto& nested : map.values)
                    StoreContainerHashes(nested);
            for (auto& map : container.map_arrays.data)
                for (auto& nested : map.values)
                    StoreContainerHashes(nested);
            container.content_hash =
                detail::HashContainer(container, [](const PropertyContainer& nested) { return nested.content_hash; });
            return container.content_hash;
        }
    } // namespace Helpers

    namespace TimeUtils {
        constexpr int64 TICKS_PER_SECOND = 10'000'000;
        constexpr int64 TICKS_PER_MILLISECOND = 10'000;

        // (Days between 0001 and 1970) * SecondsPerDay * TicksPerSecond
        constexpr int64 TICKS_AT_UNIX_EPOCH = 621'355'968'000'000'000;

        enum class TimeFormat {
            UeUTC,     // Ticks  (since 0001)
            UnixUTC,   // Secs since 1970
            UnixMsUTC, // Ms since 1970
            ISO8601,   // String date
            Seconds
        };

        inline int64 ConvertToUeTicks(TimeFormat in_time_format, int64 value) {
            switch (in_time_format) {
            case TimeFormat::UeUTC:
                return value;
            case TimeFormat::UnixUTC:
                return (value * TICKS_PER_SECOND) + TICKS_AT_UNIX_EPOCH;
            case TimeFormat::UnixMsUTC:
                return (value * TICKS_PER_MILLISECOND) + TICKS_AT_UNIX_EPOCH;
            case TimeFormat::ISO8601:
            case TimeFormat::Seconds:
                return 0;
            default:;
            }
            return 0;
        }

        inline int64 ConvertToUeTicks(TimeFormat in_time_format, float seconds) {
            if (in_time_format != TimeFormat::Seconds) {
                return 0;
            }

            return seconds * TICKS_PER_SECOND;
        }

        inline int64 ConvertToUeTicks(TimeFormat in_time_format, const std::string& ISO8601UTC) {
            if (in_time_format != TimeFormat::ISO8601)
                return 0;
            if (ISO8601UTC.empty())
                return 0;

            std::tm tm = {};
            std::istringstream ss(ISO8601UTC);
            ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
            if (ss.fail())
                return 0;


            auto time_point =
                std::chrono::sys_days {std::chrono::year(tm.tm_year + 1900) / (tm.tm_mon + 1) / tm.tm_mday};
            auto tp_seconds = std::chrono::time_point_cast<std::chrono::seconds>(time_point) +
                              std::chrono::hours(tm.tm_hour) + std::chrono::minutes(tm.tm_min) +
                              std::chrono::seconds(tm.tm_sec);

            int64 unix_seconds = tp_seconds.time_since_epoch().count();

            return (unix_seconds * TICKS_PER_SECOND) + TICKS_AT_UNIX_EPOCH;
        }

        // ----- Display formatting (replaces duplicated TimeDisplayService) -----

        struct Duration {
            int32_t hours = 0;
            int32_t minutes = 0;
            int32_t seconds = 0;
            int32_t milliseconds = 0;
        };

        inline Duration TicksToDuration(int64 ticks) {
            const int64 total_ms = (ticks + (TICKS_PER_MILLISECOND / 2)) / TICKS_PER_MILLISECOND;
            Duration d;
            d.hours = static_cast<int32_t>(total_ms / 3'600'000);
            d.minutes = static_cast<int32_t>((total_ms % 3'600'000) / 60'000);
            d.seconds = static_cast<int32_t>((total_ms % 60'000) / 1'000);
            d.milliseconds = static_cast<int32_t>(total_ms % 1'000);
            return d;
        }

        inline std::string FormatDuration(int64 ticks) {
            auto d = TicksToDuration(ticks);
            std::ostringstream s;
            if (d.hours > 0)
                s << d.hours << "h ";
            if (d.minutes > 0 || d.hours > 0)
                s << d.minutes << "m ";
            s << d.seconds << "s " << d.milliseconds << "ms";
            return s.str();
        }

        inline double TicksToSeconds(int64 ticks) {
            return static_cast<double>(ticks) / static_cast<double>(TICKS_PER_SECOND);
        }

        inline std::string FormatUtcTicks(int64 ticks) {
            // Normalize: if ticks are in UE epoch (since 0001), convert to unix-relative
            const int64 unix_ticks = (ticks >= TICKS_AT_UNIX_EPOCH) ? ticks - TICKS_AT_UNIX_EPOCH : ticks;
            const int64 unix_seconds = unix_ticks / TICKS_PER_SECOND;
            const int32_t ms = static_cast<int32_t>((unix_ticks % TICKS_PER_SECOND) / TICKS_PER_MILLISECOND);

            const std::time_t time_val = static_cast<std::time_t>(unix_seconds);
            std::tm utc_time {};
#if defined(_WIN32)
            if (gmtime_s(&utc_time, &time_val) != 0)
                return "Invalid UTC";
#else
            if (gmtime_r(&time_val, &utc_time) == nullptr)
                return "Invalid UTC";
#endif
            std::ostringstream s;
            s << std::put_time(&utc_time, "%d %b %Y %H:%M:%S") << "." << std::setw(3) << std::setfill('0')
              << (ms >= 0 ? ms : ms + 1000) << " UTC";
            return s.str();
        }

        /// Machine-readable ISO-8601 variant of FormatUtcTicks ("2026-07-23T14:03:22.123Z").
        inline std::string FormatUtcTicksIso8601(int64 ticks) {
            // Normalize: if ticks are in UE epoch (since 0001), convert to unix-relative
            const int64 unix_ticks = (ticks >= TICKS_AT_UNIX_EPOCH) ? ticks - TICKS_AT_UNIX_EPOCH : ticks;
            const int64 unix_seconds = unix_ticks / TICKS_PER_SECOND;
            const int32_t ms = static_cast<int32_t>((unix_ticks % TICKS_PER_SECOND) / TICKS_PER_MILLISECOND);

            const std::time_t time_val = static_cast<std::time_t>(unix_seconds);
            std::tm utc_time {};
#if defined(_WIN32)
            if (gmtime_s(&utc_time, &time_val) != 0)
                return "Invalid UTC";
#else
            if (gmtime_r(&time_val, &utc_time) == nullptr)
                return "Invalid UTC";
#endif
            std::ostringstream s;
            s << std::put_time(&utc_time, "%Y-%m-%dT%H:%M:%S") << "." << std::setw(3) << std::setfill('0')
              << (ms >= 0 ? ms : ms + 1000) << "Z";
            return s.str();
        }

        /// Normalize an absolute UTC stamp of unknown legacy unit to UE ticks
        /// (100ns since year 0001).  The writer has historically stamped the file
        /// header's recorded_utc_timestamp in unix seconds while the footer's
        /// per-frame created_utc uses UE ticks; this maps either (plus unix
        /// milliseconds and unix-relative ticks) onto the tick timeline.  The
        /// unit bands are disjoint by many orders of magnitude, so detection is
        /// unambiguous for any date between 1971 and ~5000 AD.
        inline int64 NormalizeUtcToUeTicks(int64 value) {
            if (value <= 0)
                return 0;
            if (value >= TICKS_AT_UNIX_EPOCH)
                return value;                                     // already UE ticks
            constexpr int64 kUnixSecondsMax = 100'000'000'000;    // unix seconds until ~5138 AD
            constexpr int64 kUnixMillisMax = 100'000'000'000'000; // unix ms in the same range
            if (value < kUnixSecondsMax)
                return value * TICKS_PER_SECOND + TICKS_AT_UNIX_EPOCH;
            if (value < kUnixMillisMax)
                return value * TICKS_PER_MILLISECOND + TICKS_AT_UNIX_EPOCH;
            return value + TICKS_AT_UNIX_EPOCH; // unix-relative ticks
        }
    } // namespace TimeUtils
} // namespace VTX
