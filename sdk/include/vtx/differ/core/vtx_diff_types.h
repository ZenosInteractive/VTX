#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <span>
#include <functional>
#include <sstream>
#include <unordered_map>
#include <cstring>
#include <cmath>


namespace VtxDiff {

    // ==========================================================
    // Enum: WireFormat
    // ----------------------------------------------------------
    // Represents the binary serialization technology used for
    // reading/writing structured binary data (Protobuf, FlatBuffers, etc.).
    // ==========================================================
    enum class WireFormat : uint8_t { None = 0, Archive, Flatbuffers, Protobuf, MAX };

    // ==========================================================
    // Enum: DiffAlgorithmType
    // ----------------------------------------------------------
    // Identifies which diff algorithm implementation to use.
    // ==========================================================
    enum class DiffAlgorithmType : uint8_t {
        Default = 0,  // Tree-based recursive diff
        Experimental, // Placeholder for testing new ones
        MAX
    };


    // ===========================================================
    // Container types (maps directly to PropertyContainer)
    // ===========================================================
    enum class EVTXContainerType : uint8_t {
        Unknown = 0,

        BoolProperties,      // 1
        Int32Properties,     // 2
        Int64Properties,     // 3
        FloatProperties,     // 4
        DoubleProperties,    // 5
        StringProperties,    // 6
        TransformProperties, // 7
        VectorProperties,    // 8
        QuatProperties,      // 9
        RangeProperties,     // 10

        ByteArrayProperties, // 11 (reaplce  UInt8Arrays/Int8Arrays)
        Int32Arrays,         // 12
        Int64Arrays,         // 13
        FloatArrays,         // 14
        DoubleArrays,        // 15
        StringArrays,        // 16
        TransformArrays,     // 17
        VectorArrays,        // 18
        QuatArrays,          // 19
        RangeArrays,         // 20
        BoolArrays,          // 21

        AnyStructProperties, // 22
        AnyStructArrays,     // 23
        MapProperties,       // 24
        MapArrays            // 25
    };

    static std::string TypeToFieldName(VtxDiff::EVTXContainerType Type) {
        switch (Type) {
        case VtxDiff::EVTXContainerType::Unknown:
            return "Unknown";

        case VtxDiff::EVTXContainerType::BoolProperties:
            return "bool_properties";
        case VtxDiff::EVTXContainerType::Int32Properties:
            return "int32_properties";
        case VtxDiff::EVTXContainerType::Int64Properties:
            return "int64_properties";
        case VtxDiff::EVTXContainerType::FloatProperties:
            return "float_properties";
        case VtxDiff::EVTXContainerType::DoubleProperties:
            return "double_properties";
        case VtxDiff::EVTXContainerType::StringProperties:
            return "string_properties";
        case VtxDiff::EVTXContainerType::TransformProperties:
            return "transform_properties";
        case VtxDiff::EVTXContainerType::VectorProperties:
            return "vector_properties";
        case VtxDiff::EVTXContainerType::QuatProperties:
            return "quat_properties";
        case VtxDiff::EVTXContainerType::RangeProperties:
            return "range_properties";

        case VtxDiff::EVTXContainerType::ByteArrayProperties:
            return "byte_array_properties";
        case VtxDiff::EVTXContainerType::Int32Arrays:
            return "int32_arrays";
        case VtxDiff::EVTXContainerType::Int64Arrays:
            return "int64_arrays";
        case VtxDiff::EVTXContainerType::FloatArrays:
            return "float_arrays";
        case VtxDiff::EVTXContainerType::DoubleArrays:
            return "double_arrays";
        case VtxDiff::EVTXContainerType::StringArrays:
            return "string_arrays";
        case VtxDiff::EVTXContainerType::TransformArrays:
            return "transform_arrays";
        case VtxDiff::EVTXContainerType::VectorArrays:
            return "vector_arrays";
        case VtxDiff::EVTXContainerType::QuatArrays:
            return "quat_arrays";
        case VtxDiff::EVTXContainerType::RangeArrays:
            return "range_arrays";
        case VtxDiff::EVTXContainerType::BoolArrays:
            return "bool_arrays";

        case VtxDiff::EVTXContainerType::AnyStructProperties:
            return "any_struct_properties";
        case VtxDiff::EVTXContainerType::AnyStructArrays:
            return "any_struct_arrays";
        case VtxDiff::EVTXContainerType::MapProperties:
            return "map_properties";
        case VtxDiff::EVTXContainerType::MapArrays:
            return "map_arrays";

        default:
            return "";
        }
    }

    // ==========================================================
    // Enum: DiffOperation
    // ----------------------------------------------------------
    // Represents one kind of change in a diff result.
    // ==========================================================
    enum class DiffOperation : uint8_t { Add, Remove, Replace, ReplaceRange };


    struct EnumHash {
        template <class T>
        size_t operator()(T v) const noexcept {
            return static_cast<size_t>(v);
        }
    };


    // ===========================================================
    // Binary path (no strings)
    // ===========================================================
    struct DiffIndexPath {
        int32_t indices[16];
        uint8_t count = 0;

        void push_back(int32_t v) {
            if (count < 16) {
                indices[count++] = v;
            }
        }
        void pop_back() {
            if (count > 0) {
                count--;
            }
        }
        DiffIndexPath Append(int32_t Index) const {
            DiffIndexPath p = *this;
            p.push_back(Index);
            return p;
        }
        bool IsEmpty() const { return count == 0; }

        size_t size() const { return count; }
        int32_t operator[](size_t i) const { return indices[i]; }
        bool operator==(const DiffIndexPath& o) const {
            if (count != o.count)
                return false;
            return std::memcmp(indices, o.indices, count * sizeof(int32_t)) == 0;
        }
    };

    struct DiffIndexOp {
        DiffOperation Operation;
        EVTXContainerType ContainerType;
        DiffIndexPath Path;
        std::string ActorId;
        std::string MapKey;
        int32_t ReplaceRangeCount;
    };

    // ===========================================================
    // Patch container
    // ===========================================================
    struct PatchIndex {
        std::vector<DiffIndexOp> operations;
        std::unordered_map<int32_t, std::string> actor_id_by_key;

        void Clear() { operations.clear(); }
    };

    // ==========================================================
    // Struct: FieldDesc
    // ----------------------------------------------------------
    // Describes a field's metadata: type, name, and flags.
    // ==========================================================
    struct FieldDesc {
        std::string name;
        EVTXContainerType type {EVTXContainerType::Int32Properties};
        bool is_array_like {false};
        bool is_actors_field = false;
        bool is_map_like {false};
        uint32_t array_size {0};
        auto operator<=>(const FieldDesc&) const = default;
    };

    // ==========================================================
    // Struct: PatchOp
    // ----------------------------------------------------------
    // Represents a single patch operation (add/remove/replace).
    // ==========================================================
    struct PatchOp {
        DiffOperation Operation {DiffOperation::Replace};
        std::string Path;
        EVTXContainerType FieldType {EVTXContainerType::Int32Properties};
        std::vector<std::byte> Data;
    };

    // ==========================================================
    // Struct: DiffOptions
    // ----------------------------------------------------------
    // Configuration for controlling diff precision, verbosity,
    // and comparison behavior.
    // ==========================================================
    struct DiffOptions {
        bool compare_floats_with_epsilon {true};
        float float_epsilon {1e-5f};
        bool verbose {false};
    };


    // ==========================================================
    // Struct: PathS
    // ----------------------------------------------------------
    // Hierarchical path representation used throughout the diff
    // engine to identify nested field locations in a binary tree.
    // ==========================================================
    struct PathS {
        std::vector<std::string> segments;

        PathS() = default;
        explicit PathS(std::string Root) { segments.push_back(std::move(Root)); }

        // Append a field segment (dot-separated)
        PathS Append(const std::string& Segment) const {
            PathS copy = *this;
            copy.segments.push_back(Segment);
            return copy;
        }

        // Append an index [n]
        PathS Index(size_t I) const {
            PathS copy = *this;
            copy.segments.push_back("[" + std::to_string(I) + "]");
            return copy;
        }

        // Append a map key {key}
        PathS Key(const std::string& key_name) const {
            PathS copy = *this;
            copy.segments.push_back("{" + key_name + "}");
            return copy;
        }

        // Convert to canonical string
        std::string ToString() const {
            std::ostringstream OSS;
            for (size_t I = 0; I < segments.size(); ++I) {
                const std::string& S = segments[I];
                if (I > 0 && !S.empty() && S[0] != '[' && S[0] != '{')
                    OSS << '.';
                OSS << S;
            }
            return OSS.str();
        }
    };

    // ==========================================================
    // Utility helpers for path building and byte copying.
    // ==========================================================
    inline std::vector<std::byte> CopyBytes(std::span<const std::byte> data) {
        return {data.begin(), data.end()};
    }

    inline std::string MakeFieldPath(const std::string& base, const std::string& field_name) {
        if (base.empty())
            return field_name;
        return base + "." + field_name;
    }

    inline std::string MakeIndexPath(const std::string& base, size_t index) {
        return base + "[" + std::to_string(index) + "]";
    }

    inline std::string MakeMapKeyPath(const std::string& base, const std::string& key) {
        return base + "{" + key + "}";
    }

    inline constexpr bool IsArraysType(EVTXContainerType type) {
        switch (type) {
        case EVTXContainerType::ByteArrayProperties:
        case EVTXContainerType::Int32Arrays:
        case EVTXContainerType::Int64Arrays:
        case EVTXContainerType::FloatArrays:
        case EVTXContainerType::DoubleArrays:
        case EVTXContainerType::StringArrays:
        case EVTXContainerType::TransformArrays:
        case EVTXContainerType::VectorArrays:
        case EVTXContainerType::QuatArrays:
        case EVTXContainerType::RangeArrays:
        case EVTXContainerType::BoolArrays:
        case EVTXContainerType::AnyStructArrays:
        case EVTXContainerType::MapArrays:
            return true;
        default:
            return false;
        }
    }


    static bool EndsWith(const std::string& s, const char* suffix) {
        const size_t n = s.size();
        const size_t m = std::strlen(suffix);
        if (n < m)
            return false;
        return std::memcmp(s.data() + (n - m), suffix, m) == 0;
    }


    static bool AreScalarsEqual(VtxDiff::EVTXContainerType type, std::span<const std::byte> A,
                                std::span<const std::byte> B, const VtxDiff::DiffOptions& Opt) {
        if (!Opt.compare_floats_with_epsilon)
            return std::memcmp(A.data(), B.data(), A.size()) == 0;

        auto IsNear = [&](float a, float b) {
            return std::abs(a - b) <= Opt.float_epsilon;
        };

        switch (type) {
        case VtxDiff::EVTXContainerType::FloatArrays:
        case VtxDiff::EVTXContainerType::FloatProperties: {
            float fa, fb;
            std::memcpy(&fa, A.data(), sizeof(float));
            std::memcpy(&fb, B.data(), sizeof(float));
            return IsNear(fa, fb);
        }


        case VtxDiff::EVTXContainerType::VectorArrays:
        case VtxDiff::EVTXContainerType::VectorProperties: {
            if (A.size() < sizeof(float) * 3)
                return false;
            const float* va = reinterpret_cast<const float*>(A.data());
            const float* vb = reinterpret_cast<const float*>(B.data());
            return IsNear(va[0], vb[0]) && IsNear(va[1], vb[1]) && IsNear(va[2], vb[2]);
        }


        case VtxDiff::EVTXContainerType::QuatArrays:
        case VtxDiff::EVTXContainerType::QuatProperties: {
            if (A.size() < sizeof(float) * 4)
                return false;
            const float* qa = reinterpret_cast<const float*>(A.data());
            const float* qb = reinterpret_cast<const float*>(B.data());
            return IsNear(qa[0], qb[0]) && IsNear(qa[1], qb[1]) && IsNear(qa[2], qb[2]) && IsNear(qa[3], qb[3]);
        }

        case VtxDiff::EVTXContainerType::TransformArrays:
        case VtxDiff::EVTXContainerType::TransformProperties: {
            if (A.size() < sizeof(float) * 10)
                return false;
            const float* ta = reinterpret_cast<const float*>(A.data());
            const float* tb = reinterpret_cast<const float*>(B.data());
            for (int k = 0; k < 10; ++k)
                if (!IsNear(ta[k], tb[k]))
                    return false;
            return true;
        }
        default:;
        }
        return std::memcmp(A.data(), B.data(), A.size()) == 0;
    }


} // namespace VtxDiff
