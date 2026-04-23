/**
 * @file vtx_types.h
 *
 * @brief Fundamental type definitions and data containers for the VTX engine.
 * @details This file defines the core data structures used for serialization,
 * memory representation, and math operations within the VTX replay system.
 * It utilizes Structure of Arrays (SoA) patterns for performance optimization.
 *
 * * @namespace VTX
 * * @author Zenos Interative
 */
#pragma once

#include "vtx_logger.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace VTX {
    /**
     * @brief Identifies the binary serialization format of a VTX file.
     */
    enum class VtxFormat : uint8_t { Unknown = 0, FlatBuffers, Protobuf };

    /**
     * @brief Represents a double-precision 3D vector.
     * @note Used for high-precision world positions.
     */
    struct Vector {
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
        bool operator==(const Vector&) const = default;
    };

    /**
     * @brief Represents a rotation quaternion (single precision).
     */
    struct Quat {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float w = 1.0f;
        bool operator==(const Quat&) const = default;
    };

    /**
     * @brief Composite structure representing a spatial transformation.
     * @details Combines translation, rotation, and scale.
     */
    struct Transform {
        Vector translation;
        Quat rotation;
        Vector scale = {1.0, 1.0, 1.0};
        bool operator==(const Transform&) const = default;
    };

    /**
     * @brief Represents a floating-point range with a normalized value.
     * @details Useful for progress bars, sliders, or clamped values (e.g., Health: 50/100).
     */
    struct FloatRange {
        float min = 0.0f;
        float max = 0.0f;
        float value_normalized = 0.0f;
        bool operator==(const FloatRange&) const = default;
    };


    /**
     * @brief Generic container for Structure of Arrays (SoA) layout.
     * @details optimized storage for lists of values. Unlike `std::vector<std::vector<T>>`,
     * this flattens data into a single contiguous buffer (`Data`) and uses `Offsets`
     * to delimit sub-arrays or map indices.
     * * @tparam T The type of data stored in the flat array.
     */
    template <typename T>
    struct FlatArray {
        std::vector<T> data;           ///< Contiguous block of data values.
        std::vector<uint32_t> offsets; ///< Indices or offsets defining boundaries/mapping.

        /**
         * @brief Clears both data and offsets vectors.
         */
        void Clear() {
            data.clear();
            offsets.clear();
        }

        /** Number of logical subarrays. */
        size_t SubArrayCount() const { return offsets.size(); }

        /** Total number of elements across all subarrays. */
        size_t TotalElementCount() const { return data.size(); }

        /** Returns a read-only span for subarray at Index (empty on out-of-bounds). */
        std::span<const T> GetSubArray(size_t Index) const {
            if (Index >= offsets.size())
                return {};
            const size_t Start = offsets[Index];
            const size_t End = (Index + 1 < offsets.size()) ? offsets[Index + 1] : data.size();
            return std::span<const T>(&data[Start], End - Start);
        }

        /** Returns a mutable span for subarray at Index (empty on out-of-bounds). */
        std::span<T> GetMutableSubArray(size_t Index) {
            if (Index >= offsets.size())
                return {};
            const size_t Start = offsets[Index];
            const size_t End = (Index + 1 < offsets.size()) ? offsets[Index + 1] : data.size();
            return std::span<T>(&data[Start], End - Start);
        }


        /** Creates an empty subarray at the end (i.e., a new marker pointing to current Bucket.size()). */
        void CreateEmptySubArray() { offsets.push_back(data.size()); }

        /** Appends a subarray with given elements at the end. */
        void AppendSubArray(std::span<const T> Items) {
            offsets.push_back(data.size());
            data.insert(data.end(), Items.begin(), Items.end());
        }

        /** Appends a subarray with an initializer_list. */
        void AppendSubArray(std::initializer_list<T> Items) {
            AppendSubArray(std::span<const T>(Items.begin(), Items.size()));
        }

        /**
         * Inserts a subarray BEFORE SubIndex. Returns false if SubIndex is out of range (SubIndex > offsets.size()).
         * SubIndex == offsets.size() is equivalent to AppendSubArray.
         */
        bool InsertSubArray(size_t SubIndex, std::span<const T> Items) {
            if (SubIndex > offsets.size())
                return false;

            // Compute insertion position in Bucket
            const size_t InsertPos = (SubIndex < offsets.size()) ? offsets[SubIndex] : data.size();

            // Insert elements into Bucket
            data.insert(data.begin() + InsertPos, Items.begin(), Items.end());

            // Insert new offset marker
            offsets.insert(offsets.begin() + SubIndex, InsertPos);

            // Rebase subsequent offsets by +Items.size()
            for (size_t i = SubIndex + 1; i < offsets.size(); ++i)
                offsets[i] += Items.size();

            return true;
        }

        /** Inserts a subarray from initializer_list BEFORE SubIndex. */
        bool InsertSubArray(size_t SubIndex, std::initializer_list<T> Items) {
            return InsertSubArray(SubIndex, std::span<const T>(Items.begin(), Items.size()));
        }

        /**
         * Erases the entire subarray at SubIndex. Returns false on out-of-bounds.
         * Elements are removed from Bucket and offsets are rebased.
         */
        bool EraseSubArray(size_t SubIndex) {
            if (SubIndex >= offsets.size())
                return false;

            const size_t Start = offsets[SubIndex];
            const size_t End = (SubIndex + 1 < offsets.size()) ? offsets[SubIndex + 1] : data.size();
            const size_t Count = End - Start;

            // Remove range from Bucket
            data.erase(data.begin() + Start, data.begin() + End);

            // Remove the offset entry
            offsets.erase(offsets.begin() + SubIndex);

            // Rebase subsequent offsets by -count
            for (size_t i = SubIndex; i < offsets.size(); ++i)
                offsets[i] -= Count;

            return true;
        }

        // ---- Element-level operations inside a subarray ----

        /**
         * Pushes Value at the end of subarray SubIndex.
         * Returns false on out-of-bounds.
         */
        bool PushBack(size_t SubIndex, const T& Value) {
            if (SubIndex >= offsets.size()) {
                size_t old_size = offsets.size();
                offsets.resize(SubIndex + 1);

                for (size_t i = old_size; i <= SubIndex; ++i) {
                    offsets[i] = data.size();
                }
            }

            const size_t InsertPos = (SubIndex + 1 < offsets.size()) ? offsets[SubIndex + 1] : data.size();

            data.insert(data.begin() + InsertPos, Value);

            // Rebase offsets after SubIndex (i.e., from SubIndex+1 onward)
            for (size_t i = SubIndex + 1; i < offsets.size(); ++i)
                offsets[i] += 1;

            return true;
        }

        /**
         * Inserts Value at position ItemIndex INSIDE subarray SubIndex.
         * ItemIndex is 0..SubArraySize; ItemIndex==SubArraySize inserts at the end.
         */
        bool Insert(size_t SubIndex, size_t ItemIndex, const T& Value) {
            if (SubIndex >= offsets.size())
                return false;

            const size_t Start = offsets[SubIndex];
            const size_t End = (SubIndex + 1 < offsets.size()) ? offsets[SubIndex + 1] : data.size();
            const size_t Sz = End - Start;
            if (ItemIndex > Sz)
                return false;

            const size_t InsertPos = Start + ItemIndex;
            data.insert(data.begin() + InsertPos, Value);

            // Rebase subsequent offsets (after SubIndex)
            for (size_t i = SubIndex + 1; i < offsets.size(); ++i)
                offsets[i] += 1;

            return true;
        }

        /**
         * Replaces the element at ItemIndex INSIDE subarray SubIndex with Value.
         */
        bool Replace(size_t SubIndex, size_t ItemIndex, const T& Value) {
            if (SubIndex >= offsets.size())
                return false;

            const size_t Start = offsets[SubIndex];
            const size_t End = (SubIndex + 1 < offsets.size()) ? offsets[SubIndex + 1] : data.size();
            const size_t Sz = End - Start;
            if (ItemIndex >= Sz)
                return false;

            data[Start + ItemIndex] = Value;
            return true;
        }

        /**
         * Erases the element at ItemIndex INSIDE subarray SubIndex.
         * Returns false on out-of-bounds.
         */
        bool Erase(size_t SubIndex, size_t ItemIndex) {
            if (SubIndex >= offsets.size())
                return false;

            const size_t Start = offsets[SubIndex];
            const size_t End = (SubIndex + 1 < offsets.size()) ? offsets[SubIndex + 1] : data.size();
            const size_t Sz = End - Start;
            if (ItemIndex >= Sz)
                return false;

            data.erase(data.begin() + (Start + ItemIndex));

            // Rebase subsequent offsets
            for (size_t i = SubIndex + 1; i < offsets.size(); ++i)
                offsets[i] -= 1;

            return true;
        }

        /**
         * Removes a range [First, Last) INSIDE subarray SubIndex.
         * Returns false if the range is invalid.
         */
        bool EraseRange(size_t SubIndex, size_t First, size_t Last) {
            if (SubIndex >= offsets.size())
                return false;

            const size_t Start = offsets[SubIndex];
            const size_t End = (SubIndex + 1 < offsets.size()) ? offsets[SubIndex + 1] : data.size();
            const size_t Sz = End - Start;

            if (First > Last || Last > Sz)
                return false;
            const size_t GlobalFirst = Start + First;
            const size_t GlobalLast = Start + Last;

            const size_t Count = GlobalLast - GlobalFirst;
            if (Count == 0)
                return true;

            data.erase(data.begin() + GlobalFirst, data.begin() + GlobalLast);

            for (size_t i = SubIndex + 1; i < offsets.size(); ++i)
                offsets[i] -= Count;

            return true;
        }

        /**
         * Replaces the entire subarray SubIndex with Items.
         * Equivalent to EraseSubArray + InsertSubArray at the same position.
         */
        bool ReplaceSubArray(size_t SubIndex, std::span<const T> Items) {
            if (SubIndex >= offsets.size())
                return false;

            const size_t Start = offsets[SubIndex];
            const size_t End = (SubIndex + 1 < offsets.size()) ? offsets[SubIndex + 1] : data.size();
            const size_t OldCount = End - Start;
            const size_t NewCount = Items.size();

            // Replace in place: erase old, insert new
            data.erase(data.begin() + Start, data.begin() + End);
            data.insert(data.begin() + Start, Items.begin(), Items.end());

            // Rebase subsequent offsets by (NewCount - OldCount)
            const ptrdiff_t Delta = static_cast<ptrdiff_t>(NewCount) - static_cast<ptrdiff_t>(OldCount);
            if (Delta != 0) {
                for (size_t i = SubIndex + 1; i < offsets.size(); ++i)
                    offsets[i] = static_cast<size_t>(static_cast<ptrdiff_t>(offsets[i]) + Delta);
            }

            return true;
        }

        bool ReplaceSubArray(size_t SubIndex, std::initializer_list<T> Items) {
            return ReplaceSubArray(SubIndex, std::span<const T>(Items.begin(), Items.size()));
        }
    };

    // --- Alias definitions for FlatArrays ---

    /// @brief Flat array for boolean values.
    using FlatBoolArray = FlatArray<uint8_t>;
    /// @brief Flat array for raw byte buffers (e.g., images, binary blobs).
    using FlatBytesArray = FlatArray<uint8_t>;
    /// @brief Flat array for 32-bit integers.
    using FlatIntArray = FlatArray<int32_t>;
    /// @brief Flat array for 64-bit integers.
    using Flatint64_tArray = FlatArray<int64_t>;
    /// @brief Flat array for single-precision floats.
    using FlatFloatArray = FlatArray<float>;
    /// @brief Flat array for double-precision floats.
    using FlatDoubleArray = FlatArray<double>;
    /// @brief Flat array for strings.
    using FlatStringArray = FlatArray<std::string>;
    /// @brief Flat array for 3D Vectors.
    using FlatVectorArray = FlatArray<Vector>;
    /// @brief Flat array for Quaternions.
    using FlatQuatArray = FlatArray<Quat>;
    /// @brief Flat array for Transforms.
    using FlatTransformArray = FlatArray<Transform>;
    /// @brief Flat array for FloatRanges.
    using FlatRangeArray = FlatArray<FloatRange>;


    // Forward declarations
    struct PropertyContainer;
    struct MapContainer;

    /// @brief Flat array for nested PropertyContainers (recursive structures).
    using FlatAnyStructArray = FlatArray<PropertyContainer>;

    /// @brief Flat array for nested custom maps
    using FlatMapArray = FlatArray<MapContainer>;


    /**
     * @brief The core dynamic container for all entity properties.
     * @details This "Mega-Struct" acts as a variant-like container capable of holding
     * lists of typed properties. It avoids type erasure (void*) by having explicit
     * vectors for every supported type.
     */
    struct PropertyContainer {
        /**
         * @brief Type identifier for this container (maps to the autogenerated EntityType enum).
         * If -1, the type is unknown or anonymous.
        */
        int32_t entity_type_id = -1;

        /**
         * @brief Stores the sign of this entity for fast diffing, if two frames have the same hash, both are identical.
         * Used for fast diffing
        */
        uint64_t content_hash = 0;

        // These vectors hold the values for properties mapped by index in the Schema.

        std::vector<bool> bool_properties;          ///< List of boolean property values.
        std::vector<int32_t> int32_properties;      ///< List of int32_t property values.
        std::vector<int64_t> int64_properties;      ///< List of int64_t property values.
        std::vector<float> float_properties;        ///< List of float property values.
        std::vector<double> double_properties;      ///< List of double property values.
        std::vector<std::string> string_properties; ///< List of string property values.

        std::vector<Transform> transform_properties; ///< List of Transform values.
        std::vector<Vector> vector_properties;       ///< List of Vector values.
        std::vector<Quat> quat_properties;           ///< List of Quaternion values.
        std::vector<FloatRange> range_properties;    ///< List of FloatRange values.

        // --- Optimized Flat Arrays (SoA) ---
        // These store arrays of arrays (e.g., an inventory list per entity).

        FlatBytesArray byte_array_properties; ///< Arrays of bytes.
        FlatIntArray int32_arrays;            ///< Arrays of int32_ts.
        Flatint64_tArray int64_arrays;        ///< Arrays of int64_ts.
        FlatFloatArray float_arrays;          ///< Arrays of floats.
        FlatDoubleArray double_arrays;        ///< Arrays of doubles.

        FlatVectorArray vector_arrays;       ///< Arrays of Vectors.
        FlatQuatArray quat_arrays;           ///< Arrays of Quaternions.
        FlatTransformArray transform_arrays; ///< Arrays of Transforms.
        FlatRangeArray range_arrays;         ///< Arrays of FloatRanges.

        FlatBoolArray bool_arrays;     ///< Arrays of booleans.
        FlatStringArray string_arrays; ///< Arrays of strings.


        /**
         * @brief Nested structures.
         * @details Recursively holds other PropertyContainers.
         */
        std::vector<PropertyContainer> any_struct_properties;

        /**
         * @brief Arrays of nested structures.
         */
        FlatAnyStructArray any_struct_arrays;

        /**
         * @brief Map properties (Key-Value pairs).
         */
        std::vector<MapContainer> map_properties;

        /**
         * @brief Arrays of Maps.
         */
        FlatMapArray map_arrays;
    };

    /**
     * @brief Represents a Dictionary/Map structure.
     * @details Maps string keys to PropertyContainer values.
     */
    struct MapContainer {
        std::vector<std::string> keys;         ///< List of keys.
        std::vector<PropertyContainer> values; ///< Corresponding list of values.
    };


    struct EntityRange {
        int32_t start_index = 0;
        int32_t count = 0;
    };

    /**
     * @brief Represents the data payload for a specific grouping (e.g., a Team or Actor list).
     */
    struct Bucket {
        std::vector<std::string> unique_ids;     ///< Unique identifiers for the entities in this data block.
        std::vector<PropertyContainer> entities; ///< The actual properties for each entity/ID.
        std::vector<EntityRange>
            type_ranges; //< Ranges to know were a type starts and ends, assumes entites are ordered by type

        std::span<const PropertyContainer> GetEntitiesOfType(int32_t typeId) const {
            if (typeId >= 0 && typeId < type_ranges.size()) {
                const auto& range = type_ranges[typeId];
                if (range.count > 0) {
                    return std::span<const PropertyContainer>(entities.data() + range.start_index, range.count);
                }
            }
            return {};
        }

        template <typename TEnum>
        std::span<const PropertyContainer> GetEntitiesOfType(TEnum type) const {
            return GetEntitiesOfType(static_cast<int32_t>(type));
        }
    };

    /**
     * @brief Represents a single simulation frame or tick.
     */
    struct Frame {
        Frame() {}

        Bucket& CreateBucket(const std::string& name) {
            auto it = bucket_map.find(name);
            if (it != bucket_map.end()) {
                return buckets[it->second];
            }

            buckets.emplace_back();
            bucket_map[name] = buckets.size() - 1;
            return buckets.back();
        }

        Bucket& GetBucket(const std::string& name) { return CreateBucket(name); }

        Bucket& GetBucket(int32_t bucket_index) { return buckets.at(bucket_index); }

        const Bucket& GetBucket(const std::string& name) const {
            static const Bucket Dummy {};

            auto it = bucket_map.find(name);
            if (it != bucket_map.end()) {
                return buckets[it->second];
            }
            return Dummy;
        }

        std::vector<Bucket>& GetMutableBuckets() { return buckets; }
        const std::vector<Bucket>& GetBuckets() const { return buckets; }

        std::map<std::string, size_t> bucket_map;
        std::vector<Bucket> buckets;
    };


    /**
     * @brief Versioning information for the replay file.
     */
    struct VersionInfo {
        uint32_t format_major = 1;   ///< Major binary format version.
        uint32_t format_minor = 2;   ///< Minor binary format version.
        uint32_t schema_version = 3; ///< Logic schema version (gameplay data structure).
    };

    /**
     * @brief Defines the mapping between Property Names and their internal indices.
     * @details Used to look up where a property like "Health" is stored within the
     * `PropertyContainer` vectors (e.g., int32_tProperties[5]).
     */
    struct PropertySchema {
        std::unordered_map<std::string, uint32_t> bool_mapping;   ///< Name -> Index for Bool.
        std::unordered_map<std::string, uint32_t> int_mapping;    ///< Name -> Index for int32_t.
        std::unordered_map<std::string, uint32_t> int64_mapping;  ///< Name -> Index for int64_t.
        std::unordered_map<std::string, uint32_t> float_mapping;  ///< Name -> Index for Float.
        std::unordered_map<std::string, uint32_t> double_mapping; ///< Name -> Index for Double.
        std::unordered_map<std::string, uint32_t> string_mapping; ///< Name -> Index for String.

        std::unordered_map<std::string, uint32_t> vector_mapping;    ///< Name -> Index for Vector.
        std::unordered_map<std::string, uint32_t> quat_mapping;      ///< Name -> Index for Quat.
        std::unordered_map<std::string, uint32_t> transform_mapping; ///< Name -> Index for Transform.
        std::unordered_map<std::string, uint32_t> range_mapping;     ///< Name -> Index for FloatRange.
    };

    struct ContextualSchema {
        std::string data_identifier;
        int32_t data_version;            // 0
        std::string data_version_string; //data_version 0 = 15.23 prod
        std::string property_mapping;    //This is the full schema in json format
    };

    /**
     * @brief Represents a significant event on the timeline (Kill, Goal, Marker).
     * @details Used for UI navigation and scrubbing.
     */
    struct TimelineEvent {
        float game_time = 0.0f;       ///< Time in seconds when the event occurred.
        std::string event_type;       ///< type_ category (e.g., "Kill").
        std::string label;            ///< Display label (e.g., "Player A killed Player B").
        Vector location;              ///< 3D location of the event.
        std::string entity_unique_id; ///< ID of the primary entity involved.
    };


    /**
     * @brief Header structure located at the beginning of a VTX file.
     */
    struct FileHeader {
        VersionInfo version;        ///< Versioning info.
        PropertySchema prop_schema; ///< Schema for property lookup.
        ContextualSchema contextual_schema;
        std::string replay_uuid;            ///< Unique ID for the replay session.
        std::string replay_name;            ///< Display name of the replay.
        int64_t recorded_utc_timestamp = 0; ///< UTC Timestamp of recording start.

        std::string custom_json_metadata; ///< Arbitrary JSON string for extra metadata.
    };

    /**
     * @brief A chunk of frames stored in the file.
     * @details Frames are grouped into chunks to allow for compression and partial loading.
     */
    struct Chunk {
        int32_t chunk_index = 0;    ///< Sequential index of the chunk.
        bool is_compressed = false; ///< Flag indicating if `compressed_frames` contains valid data.

        std::vector<Frame> frames;              ///< Raw frames (if not compressed / after decompression).
        std::vector<uint8_t> compressed_frames; ///< Compressed binary blob of frames.
    };

    /**
     * @brief Entry in the seek table for random access.
     */
    struct ChunkIndexEntry {
        int32_t chunk_index = 0; ///< Index of the chunk.
        int32_t start_frame = 0; ///< First frame number in this chunk.
        int32_t end_frame = 0;   ///< Last frame number in this chunk.

        uint64_t file_offset = 0;      ///< Byte offset in the file where the chunk begins.
        uint32_t chunk_size_bytes = 0; ///< Size of the chunk in bytes.
    };

    /**
     * @brief Statistical time data for the replay.
     */
    struct ReplayTimeData {
        std::vector<uint64_t> game_time;   ///< List of game timestamps.
        std::vector<uint64_t> created_utc; ///< List of wall-clock timestamps.
        std::vector<uint32_t> gaps;        ///< Information about time gaps/pauses.
        std::vector<uint32_t> segments;    ///< Segmentation data.
    };

    /**
     * @brief Footer structure located at the end of a VTX file.
     * @details Contains the index and summary data needed to navigate the file efficiently.
     */
    struct FileFooter {
        int32_t total_frames = 0;      ///< Total number of frames in the replay.
        float duration_seconds = 0.0f; ///< Total duration in seconds.

        ReplayTimeData times;                     ///< Detailed time data.
        std::vector<ChunkIndexEntry> chunk_index; ///< Seek table.
        std::vector<TimelineEvent> events;        ///< List of significant events.

        uint64_t payload_checksum = 0; ///< Integrity checksum of the payload.
    };

    namespace GameTime {
        enum class EGameTimeType : uint8_t { None, OnlyGameTime, OnlyCreatedUtc, Both };

        enum class EFilterType : uint8_t {
            None,
            OnlyIncreasing,
            OnlyDecreasing,
        };

        struct GameTimeRegister {
            std::optional<float> game_time = std::nullopt;
            std::optional<int64_t> created_utc_time = std::nullopt;
            EFilterType FrameFilterType = EFilterType::None;
        };

        using int64 = std::int64_t;
        using int32 = std::int32_t;
        constexpr int64 TICKS_PER_SECOND = 10'000'000;

        enum class GameTimeType { None, OnlyGameTime, OnlyCreatedUtc, Both };

        struct VTXGameTimes {
            struct StateSnapshot {
                size_t game_time_size = 0;
                size_t created_utc_size = 0;
                size_t gaps_size = 0;
                size_t segments_size = 0;
                int64 offset = 0;
                EGameTimeType type = EGameTimeType::None;
                bool add_used = false;
                bool resolve_success = false;
            };

            StateSnapshot current_snapshot;

            void CreateSnapshot() {
                current_snapshot = {game_time_.size(),
                                    created_utc_.size(),
                                    timeline_gaps_.size(),
                                    game_segments_.size(),
                                    offset_,
                                    type_,
                                    add_used_,
                                    resolve_success_};
            }

            void Rollback() {
                game_time_.resize(current_snapshot.game_time_size);
                created_utc_.resize(current_snapshot.created_utc_size);
                timeline_gaps_.resize(current_snapshot.gaps_size);
                game_segments_.resize(current_snapshot.segments_size);

                offset_ = current_snapshot.offset;
                type_ = current_snapshot.type;
                add_used_ = current_snapshot.add_used;
                resolve_success_ = current_snapshot.resolve_success;
            }

            // --- Constructor ---
            //
            // start_utc_ is intentionally left at 0.  It gets overwritten
            // either by SetStartUtc()/reset paths, or implicitly when the
            // first frame arrives via AddTimeRegistry (historical or live).
            // Initialising it to system_clock::now() here used to reject any
            // replay whose timestamps predate the writer process.
            VTXGameTimes()
                : fps_(0)
                , fps_inverse_(0)
                , is_increasing_(true)
                , start_utc_(0)
                , offset_(0)
                , type_(EGameTimeType::None)
                , add_used_(false)
                , resolve_success_(false)
                , chunk_start_index_(0) {
                SetFPS(30);
            }


            static int64 GetUtcNowTicks() {
                auto now = std::chrono::system_clock::now();
                auto duration = now.time_since_epoch();
                return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count() / 100;
            }

            static int64 SecondsToTicks(float Seconds) {
                // Simulates FTimespan::FromSeconds(Seconds).GetTicks()
                return static_cast<int64>(Seconds * TICKS_PER_SECOND);
            }

            static std::string TicksToString(int64 Ticks) {
                // Simulates FTimespan(...).ToString()
                double seconds = static_cast<double>(Ticks) / TICKS_PER_SECOND;
                return std::to_string(seconds) + "s";
            }


            void CopyFrom(const VTXGameTimes& GameTimes) {
                game_time_ = GameTimes.GetLastChunkGameTime();
                created_utc_ = GameTimes.GetLastChunkCreatedUtc();
                receive_utc_ = GameTimes.GetLastChunkReceiveUtc();
                game_times_sorted_as_ticks_ = GameTimes.GetLastChunkGameTimesSortedAsTicks();
                sorted_game_time_indexes_ = GameTimes.GetLastChunkSortedGameTimeIndexes();
                timeline_gaps_ = GameTimes.GetLastChunkTimelineGaps();
                game_segments_ = GameTimes.GetLastChunkGameSegments();
            }

            void Clear() {
                game_time_.clear();
                created_utc_.clear();
                receive_utc_.clear();
                game_times_sorted_as_ticks_.clear();
                sorted_game_time_indexes_.clear();
                timeline_gaps_.clear();
                game_segments_.clear();
                start_utc_ = 0; // set when the first real frame arrives
                offset_ = 0;
                type_ = EGameTimeType::None;
                SetFPS(30);
                is_increasing_ = true;
                add_used_ = false;
                chunk_start_index_ = 0;
                resolve_success_ = false;
            }

            void Setup(const float InFPS, const bool InIsIncreasing, const int64 InStartUtc = 0) {
                SetFPS(InFPS);

                is_increasing_ = InIsIncreasing;

                if (InStartUtc == 0) {
                    start_utc_ = GetUtcNowTicks();
                } else {
                    start_utc_ = InStartUtc;
                }
            }

            void InsertLiveChunkTimes(const VTXGameTimes& NewChunkGameTime) {
                game_time_.insert(game_time_.end(), NewChunkGameTime.game_time_.begin(),
                                  NewChunkGameTime.game_time_.end());
                created_utc_.insert(created_utc_.end(), NewChunkGameTime.created_utc_.begin(),
                                    NewChunkGameTime.created_utc_.end());
                receive_utc_.insert(receive_utc_.end(), NewChunkGameTime.receive_utc_.begin(),
                                    NewChunkGameTime.receive_utc_.end());
                game_times_sorted_as_ticks_.insert(game_times_sorted_as_ticks_.end(),
                                                   NewChunkGameTime.game_times_sorted_as_ticks_.begin(),
                                                   NewChunkGameTime.game_times_sorted_as_ticks_.end());
                sorted_game_time_indexes_.insert(sorted_game_time_indexes_.end(),
                                                 NewChunkGameTime.sorted_game_time_indexes_.begin(),
                                                 NewChunkGameTime.sorted_game_time_indexes_.end());
                timeline_gaps_.insert(timeline_gaps_.end(), NewChunkGameTime.timeline_gaps_.begin(),
                                      NewChunkGameTime.timeline_gaps_.end());
                game_segments_.insert(game_segments_.end(), NewChunkGameTime.game_segments_.begin(),
                                      NewChunkGameTime.game_segments_.end());
            }

            bool AddTimeRegistry(const GameTimeRegister& time_registry) {
                // UTC regression check only makes sense once we already have a
                // prior frame.  Comparing against start_utc_ (system_clock::now
                // at construction time) was wrong -- it rejected valid
                // historical replays whose first timestamp predates the writer
                // process.  See samples/generate_replay.cpp for the bug this
                // fixes.
                if (time_registry.created_utc_time.has_value() && !created_utc_.empty() &&
                    *time_registry.created_utc_time <= created_utc_.back()) {
                    VTX_WARN("CreatedUTC {} (new) is <= {} (last)", *time_registry.created_utc_time,
                             created_utc_.back());
                    return false;
                }

                if (time_registry.game_time.has_value()) {
                    // Monotonicity filters only apply once we already have a
                    // prior frame -- the first frame is always accepted,
                    // otherwise a replay that legitimately starts at t=0 would
                    // be rejected against the default-constructed "last" of 0.
                    const bool has_prior_game_time = !game_time_.empty();

                    switch (time_registry.FrameFilterType) {
                    case EFilterType::OnlyIncreasing:
                        if (has_prior_game_time && time_registry.game_time <= GetLastGameTimeSeconds()) {
                            VTX_WARN("OnlyIncreasing filter rejected game_time {} (new) <= {} (last)",
                                     *time_registry.game_time, GetLastGameTimeSeconds());
                            return false;
                        }
                        break;
                    case EFilterType::OnlyDecreasing:
                        if (has_prior_game_time && time_registry.game_time >= GetLastGameTimeSeconds()) {
                            VTX_WARN("OnlyDecreasing filter rejected game_time {} (new) >= {} (last)",
                                     *time_registry.game_time, GetLastGameTimeSeconds());
                            return false;
                        }
                        break;
                    case EFilterType::None:
                    default:
                        break;
                    }
                }

                if (time_registry.game_time.has_value() && time_registry.created_utc_time.has_value()) {
                    return AddBoth(*time_registry.game_time, *time_registry.created_utc_time);
                } else if (time_registry.created_utc_time.has_value()) {
                    return AddCreatedUtc(*time_registry.created_utc_time);
                } else if (time_registry.game_time.has_value()) {
                    return AddGameTime(*time_registry.game_time);
                }
                return true;
            }

            bool AddAll(const float InGameTime, const int64 InCreatedUtc, const int64 InReceiveUtc) {
                if (add_used_) {
                    VTX_ERROR("Another Add method has already been used for this frame!");
                    return false;
                }

                game_time_.push_back(SecondsToTicks(InGameTime));
                created_utc_.push_back(InCreatedUtc);
                receive_utc_.push_back(InReceiveUtc);
                type_ = EGameTimeType::Both;
                add_used_ = true;

                return true;
            }

            bool AddBoth(const float InGameTime, const int64 InCreatedUtc) {
                if (add_used_) {
                    VTX_ERROR("Another Add method has already been used for this frame!");
                    return false;
                }

                game_time_.push_back(SecondsToTicks(InGameTime));
                created_utc_.push_back(InCreatedUtc);
                type_ = EGameTimeType::Both;
                add_used_ = true;

                return true;
            }

            bool AddGameTime(const float InGameTime) {
                if (add_used_) {
                    VTX_ERROR("Another Add method has already been used for this frame!");
                    return false;
                }

                game_time_.push_back(SecondsToTicks(InGameTime));
                type_ = EGameTimeType::OnlyGameTime;
                add_used_ = true;

                return true;
            }

            bool AddCreatedUtc(const int64 InCreatedUtc) {
                if (add_used_) {
                    VTX_ERROR("Another Add method has already been used for this frame!");
                    return false;
                }

                created_utc_.push_back(InCreatedUtc);
                type_ = EGameTimeType::OnlyCreatedUtc;
                add_used_ = true;

                return true;
            }

            bool ResolveGameTimes(const int32 FrameAmountSoFar) {
                add_used_ = false;
                resolve_success_ = Resolve(FrameAmountSoFar);
                return resolve_success_;
            }

            void ManuallyMarkGameSegmentStart(const int32 FrameNumber) { game_segments_.push_back(FrameNumber); }

            void ManuallyMarkGameSegmentStart() { game_segments_.push_back(GetFrameNumber()); }

            bool IsEmpty() const { return game_time_.empty() && created_utc_.empty() && receive_utc_.empty(); }

            int64 LastGameTime() const {
                if (!game_time_.empty()) {
                    return game_time_.back();
                }
                return 0;
            }

            double GetLastGameTimeSeconds() const {
                const int64 Time = LastGameTime();
                return static_cast<double>(Time) / static_cast<double>(TICKS_PER_SECOND);
            }

            int64 FirstGameTime() const {
                if (!game_time_.empty()) {
                    return game_time_.front();
                }
                return 0;
            }

            double FirstGameTimeSeconds() const {
                const int64 Time = FirstGameTime();
                return static_cast<double>(Time) / static_cast<double>(TICKS_PER_SECOND);
            }

            int64 LastCreatedUtc() const {
                if (!created_utc_.empty()) {
                    return created_utc_.back();
                }
                return start_utc_;
            }

            int64 StartUtc() const { return start_utc_; }

            int64 FirstCreatedUtc() const {
                if (!created_utc_.empty()) {
                    return created_utc_.front();
                }
                return start_utc_;
            }

            float GetDuration() const {
                return static_cast<float>(LastCreatedUtc() - FirstCreatedUtc()) / static_cast<double>(TICKS_PER_SECOND);
            }

            void DebugPrintGameTimes() {
                for (size_t i = 0; i < game_time_.size(); ++i) {
                    VTX_DEBUG("Frame {}: game_time_ = {}, CreatedUTC = {}", i, TicksToString(game_time_[i]),
                              TicksToString(created_utc_[i]));
                }
            }

            void UpdateChunkStartIndex() { chunk_start_index_ = GetFrameNumber(); }

            // Getters
            const std::vector<int64>& GetGameTime() const { return game_time_; }
            const std::vector<int64>& GetCreatedUtc() const { return created_utc_; }
            const std::vector<int64>& GetReceiveUtc() const { return receive_utc_; }
            const std::vector<int32>& GetSortedGameTimeIndexes() const { return sorted_game_time_indexes_; }
            const std::vector<int64>& GetGameTimesSortedAsTicks() const { return game_times_sorted_as_ticks_; }
            const std::vector<int32>& GetTimelineGaps() const { return timeline_gaps_; }
            const std::vector<int32>& GetGameSegments() const { return game_segments_; }

            // Chunk Getters
            std::vector<int64> GetLastChunkGameTime() const { return GetLastChunk(game_time_); }
            std::vector<int64> GetLastChunkCreatedUtc() const { return GetLastChunk(created_utc_); }
            std::vector<int64> GetLastChunkReceiveUtc() const { return GetLastChunk(receive_utc_); }
            std::vector<int32> GetLastChunkSortedGameTimeIndexes() const {
                return GetLastChunk(sorted_game_time_indexes_);
            }
            std::vector<int64> GetLastChunkGameTimesSortedAsTicks() const {
                return GetLastChunk(game_times_sorted_as_ticks_);
            }
            std::vector<int32> GetLastChunkTimelineGaps() const { return GetLastChunkPartial(timeline_gaps_); }
            std::vector<int32> GetLastChunkGameSegments() const { return GetLastChunkPartial(game_segments_); }

            void SetGameTime(std::vector<int64_t> times) { game_time_ = std::move(times); }
            void SetCreatedUtc(std::vector<int64_t> utc) { created_utc_ = std::move(utc); }
            void SetReceiveUtc(std::vector<int64_t> utc) { receive_utc_ = std::move(utc); }
            void SetGameTimesSortedAsTicks(std::vector<int64_t> ticks) {
                game_times_sorted_as_ticks_ = std::move(ticks);
            }
            void SetSortedGameTimeIndexes(std::vector<int32_t> indexes) {
                sorted_game_time_indexes_ = std::move(indexes);
            }
            void SetTimelineGaps(std::vector<int32_t> gaps) { timeline_gaps_ = std::move(gaps); }
            void SetGameSegments(std::vector<int32_t> segments) { game_segments_ = std::move(segments); }

        private:
            std::vector<int64> game_time_;
            std::vector<int64> created_utc_;
            std::vector<int64> receive_utc_;
            std::vector<int64> game_times_sorted_as_ticks_;
            std::vector<int32> sorted_game_time_indexes_;
            std::vector<int32> timeline_gaps_;
            std::vector<int32> game_segments_;

            float fps_;
            int64 fps_inverse_;
            bool is_increasing_;
            int64 start_utc_;
            int64 offset_;
            EGameTimeType type_;
            bool add_used_;
            bool resolve_success_;
            int32 chunk_start_index_;

            void SetFPS(const float InFPS) {
                fps_ = InFPS;
                fps_inverse_ = static_cast<int64>((1.0f / fps_) * TICKS_PER_SECOND);
            }

            int32 GetFrameNumber() const {
                return static_cast<int32>(std::max({game_time_.size(), created_utc_.size(), receive_utc_.size()}));
            }

            bool Resolve(const int32 FrameAmountSoFar) {
                if (FrameAmountSoFar <= 0) {
                    VTX_ERROR("Quitting because there are 0 frames while trying to resolve GameTimes!");
                    return false;
                }

                switch (type_) {
                case EGameTimeType::None:
                    FakeBothTimesFromFPS();
                    break;
                case EGameTimeType::OnlyCreatedUtc:
                    FakeGameTimeFromUtc();
                    break;
                case EGameTimeType::OnlyGameTime:
                    FakeUtcFromGameTime();
                    break;
                case EGameTimeType::Both:
                    // Dont do anything.
                    break;
                }

                if (game_time_.size() == created_utc_.size() &&
                    game_time_.size() == static_cast<size_t>(FrameAmountSoFar)) {
                    if (!game_time_.empty()) {
                        SortGameTime();
                        DetectGap();
                        DetectGameSegment();
                    }
                    return true;
                }

                return false;
            }

            void FakeBothTimesFromFPS() {
                if (created_utc_.empty()) {
                    created_utc_.push_back(start_utc_);
                    game_time_.push_back(0);
                } else {
                    game_time_.push_back(game_time_.back() + fps_inverse_);
                    created_utc_.push_back(created_utc_.back() + fps_inverse_);
                }
            }

            void FakeGameTimeFromUtc() { game_time_.push_back(created_utc_.back() - created_utc_[0]); }

            void FakeUtcFromGameTime() {
                int64 FakedUtc;
                const int64 RawTicks = start_utc_ + game_time_.back();

                if (created_utc_.empty()) {
                    FakedUtc = RawTicks;
                } else {
                    const int64 LastFakeUtc = created_utc_.back();
                    FakedUtc = RawTicks + offset_;

                    if (is_increasing_) {
                        if (FakedUtc <= LastFakeUtc) {
                            const int64 Bump = LastFakeUtc + fps_inverse_ - FakedUtc;
                            offset_ += Bump;
                            FakedUtc = RawTicks + offset_;
                        }
                    } else {
                        if (FakedUtc >= LastFakeUtc) {
                            const int64 Bump = LastFakeUtc - fps_inverse_ - FakedUtc;
                            offset_ += Bump;
                            FakedUtc = RawTicks + offset_;
                        }
                    }
                }

                created_utc_.push_back(FakedUtc);
            }

            void SortGameTime() {
                const int32 NewIdx = static_cast<int32>(game_time_.size()) - 1;
                const int64 NewTime = game_time_.back();

                auto it =
                    std::lower_bound(game_times_sorted_as_ticks_.begin(), game_times_sorted_as_ticks_.end(), NewTime);
                auto insertPos = std::distance(game_times_sorted_as_ticks_.begin(), it);

                sorted_game_time_indexes_.insert(sorted_game_time_indexes_.begin() + insertPos, NewIdx);
                game_times_sorted_as_ticks_.insert(it, NewTime);
            }

            void DetectGap() {
                if (fps_ > 0 && created_utc_.size() > 1) {
                    const int64 Threshold = 3 * fps_inverse_;

                    int64 last = created_utc_.back();
                    int64 prev = created_utc_[created_utc_.size() - 2];

                    if ((last - prev) > Threshold) {
                        timeline_gaps_.push_back(GetFrameNumber());
                    }
                }
            }

            void DetectGameSegment() {
                int32 currentFrame = GetFrameNumber();
                bool bContains = false;
                for (int32 val : game_segments_) {
                    if (val == currentFrame) {
                        bContains = true;
                        break;
                    }
                }

                if (bContains) {
                    return;
                }

                if (game_time_.size() > 1) {
                    const int64 Delta = game_time_.back() - game_time_[game_time_.size() - 2];
                    if ((is_increasing_ && Delta < 0) || (!is_increasing_ && Delta > 0)) {
                        game_segments_.push_back(GetFrameNumber());
                    }
                }
            }

            std::vector<int32> GetLastChunkPartial(const std::vector<int32>& InArray) const {
                std::vector<int32> Result;
                for (const int32 Elem : InArray) {
                    if (Elem > chunk_start_index_) {
                        Result.push_back(Elem);
                    }
                }
                return Result;
            }

            template <typename T>
            std::vector<T> GetLastChunk(const std::vector<T>& InArray) const {
                const int32 Count = static_cast<int32>(InArray.size()) - chunk_start_index_;
                std::vector<T> Result;
                if (Count > 0) {
                    Result.reserve(Count);
                    Result.insert(Result.end(), InArray.begin() + chunk_start_index_, InArray.end());
                }
                return Result;
            }
        };
    } // namespace GameTime

    struct SessionConfig {
        std::string replay_name;
        std::string replay_uuid;
        std::string custom_json_metadata = "{}";
        int32_t schema_version = 0;
    };

    struct SessionFooter {
        SessionFooter()
            : total_frames(0)
            , duration_seconds(0) {}

        int32_t total_frames;
        double duration_seconds;

        const std::vector<int64_t>* game_times = nullptr;
        const std::vector<int64_t>* created_utc = nullptr;
        const std::vector<int32_t>* gaps = nullptr;
        const std::vector<int32_t>* segments = nullptr;
    };

    struct ChunkIndexData {
        ChunkIndexData()
            : chunk_index(0)
            , file_offset(0)
            , chunk_size_bytes(0)
            , start_frame(0)
            , end_frame(0) {}

        int32_t chunk_index;
        int64_t file_offset;
        uint32_t chunk_size_bytes;
        int32_t start_frame;
        int32_t end_frame;
    };
} // namespace VTX
