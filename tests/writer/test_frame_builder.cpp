// FrameBuilder, PrepareForOverwrite and the property slots: a frame refilled in place through slots is, byte for
// byte, the frame a writer building fresh prepared containers would make -- whatever the reused memory held.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "vtx_schema_generated.h" // complete fbsvtx/cppvtx types before the policy headers
#include "vtx_schema.pb.h"

#include "vtx/common/vtx_property_slots.h"
#include "vtx/common/vtx_types.h"
#include "vtx/common/vtx_types_helpers.h"
#include "vtx/writer/core/frame_builder.h"
#include "vtx/writer/policies/formatters/protobuff_vtx_policy.h"

namespace {

    constexpr int32_t kOuterType = 0;
    constexpr int32_t kInnerType = 1;

    VTX::SchemaField Field(std::string name, VTX::FieldType type, VTX::FieldContainerType container, int32_t index,
                           std::string struct_type = {}) {
        VTX::SchemaField field {};
        field.name = std::move(name);
        field.struct_type = std::move(struct_type);
        field.type_id = type;
        field.key_id = VTX::FieldType::None;
        field.container_type = container;
        field.index = index;
        return field;
    }

    struct Schemas {
        VTX::SchemaStruct outer, inner;
    };

    // Outer: two of every scalar kind, one nested struct, a double array, a struct array and a map.
    // Inner: one double.
    Schemas MakeSchemas() {
        Schemas s;
        const size_t kinds = static_cast<size_t>(VTX::FieldType::Enum) + 1;
        s.outer.struct_name = "Outer";
        s.outer.type_max_indices.assign(kinds, 0);
        s.outer.array_max_indices.assign(kinds, 0);
        for (VTX::FieldType t :
             {VTX::FieldType::Bool, VTX::FieldType::Int32, VTX::FieldType::Int64, VTX::FieldType::Float,
              VTX::FieldType::Double, VTX::FieldType::String, VTX::FieldType::Vector, VTX::FieldType::Quat,
              VTX::FieldType::Transform, VTX::FieldType::FloatRange}) {
            for (int32_t i = 0; i < 2; ++i) {
                s.outer.fields.push_back(Field("f" + std::to_string(static_cast<int>(t)) + "_" + std::to_string(i), t,
                                               VTX::FieldContainerType::None, i));
            }
            s.outer.type_max_indices[static_cast<size_t>(t)] = 2;
        }
        s.outer.fields.push_back(Field("nested", VTX::FieldType::Struct, VTX::FieldContainerType::None, 0, "Inner"));
        s.outer.type_max_indices[static_cast<size_t>(VTX::FieldType::Struct)] = 1;
        s.outer.fields.push_back(Field("doubles", VTX::FieldType::Double, VTX::FieldContainerType::Array, 0));
        s.outer.array_max_indices[static_cast<size_t>(VTX::FieldType::Double)] = 1;
        s.outer.fields.push_back(Field("inners", VTX::FieldType::Struct, VTX::FieldContainerType::Array, 0, "Inner"));
        s.outer.array_max_indices[static_cast<size_t>(VTX::FieldType::Struct)] = 1;
        s.outer.map_max_index = 1;

        s.inner.struct_name = "Inner";
        s.inner.type_max_indices.assign(kinds, 0);
        s.inner.array_max_indices.assign(kinds, 0);
        s.inner.fields.push_back(Field("value", VTX::FieldType::Double, VTX::FieldContainerType::None, 0));
        s.inner.type_max_indices[static_cast<size_t>(VTX::FieldType::Double)] = 1;
        return s;
    }

    void FillInner(VTX::PropertyContainer& c, const Schemas& s, double value) {
        VTX::Helpers::PrepareForOverwrite(c, s.inner, kInnerType);
        VTX::Set(c, *VTX::ScalarSlotOf<double>(*VTX::FindField(s.inner, "value")), value);
    }

    // Every scalar and struct slot set, through slots only; the arrays `seed`-long.
    void FillOuter(VTX::PropertyContainer& c, const Schemas& s, int seed) {
        for (const VTX::SchemaField& f : s.outer.fields) {
            const double v = seed * 10.0 + f.index;
            if (f.container_type == VTX::FieldContainerType::Array) {
                if (f.type_id == VTX::FieldType::Double) {
                    std::vector<double> values(static_cast<size_t>(seed % 4), v);
                    VTX::Assign(c, *VTX::ArraySlotOf<double>(f), std::span<const double>(values));
                } else {
                    auto elements = VTX::Resize(c, *VTX::StructArraySlotOf(f), static_cast<size_t>(seed % 3));
                    for (size_t e = 0; e < elements.size(); ++e)
                        FillInner(elements[e], s, v + e);
                }
                continue;
            }
            switch (f.type_id) {
            case VTX::FieldType::Bool:
                VTX::Set(c, *VTX::ScalarSlotOf<bool>(f), seed % 2 == 0);
                break;
            case VTX::FieldType::Int32:
                VTX::Set(c, *VTX::ScalarSlotOf<int32_t>(f), seed);
                break;
            case VTX::FieldType::Int64:
                VTX::Set(c, *VTX::ScalarSlotOf<int64_t>(f), int64_t {seed} << 33);
                break;
            case VTX::FieldType::Float:
                VTX::Set(c, *VTX::ScalarSlotOf<float>(f), static_cast<float>(v));
                break;
            case VTX::FieldType::Double:
                VTX::Set(c, *VTX::ScalarSlotOf<double>(f), seed % 3 == 0 ? 0.0 : v);
                break;
            case VTX::FieldType::String:
                VTX::Set(c, *VTX::ScalarSlotOf<std::string>(f), seed % 2 ? std::string() : "s" + std::to_string(seed));
                break;
            case VTX::FieldType::Vector:
                VTX::Set(c, *VTX::ScalarSlotOf<VTX::Vector>(f), VTX::Vector {v, -v, 0.0});
                break;
            case VTX::FieldType::Quat:
                VTX::Set(c, *VTX::ScalarSlotOf<VTX::Quat>(f), VTX::Quat {0.0f, 0.0f, 0.0f, 1.0f});
                break;
            case VTX::FieldType::Transform:
                VTX::Set(c, *VTX::ScalarSlotOf<VTX::Transform>(f), VTX::Transform {});
                break;
            case VTX::FieldType::FloatRange:
                VTX::Set(c, *VTX::ScalarSlotOf<VTX::FloatRange>(f), VTX::FloatRange {});
                break;
            case VTX::FieldType::Struct:
                FillInner(VTX::Child(c, *VTX::StructSlotOf(f)), s, v);
                break;
            default:
                break;
            }
        }
    }

    // The frame a writer building fresh containers makes.
    VTX::Frame FreshFrame(const Schemas& s, const std::vector<std::vector<int>>& per_bucket,
                          const std::vector<std::string>& names) {
        VTX::Frame frame;
        for (size_t b = 0; b < names.size(); ++b) {
            VTX::Bucket& bucket = frame.CreateBucket(names[b]);
            for (int seed : per_bucket[b]) {
                VTX::PropertyContainer c;
                c.entity_type_id = seed < 0 ? kInnerType : kOuterType;
                VTX::Helpers::PreparePropertyContainer(c, seed < 0 ? s.inner : s.outer);
                if (seed < 0)
                    VTX::Set(c, *VTX::ScalarSlotOf<double>(*VTX::FindField(s.inner, "value")), double(-seed));
                else
                    FillOuter(c, s, seed);
                bucket.entities.push_back(std::move(c));
                bucket.unique_ids.push_back("id" + std::to_string(seed));
            }
        }
        return frame;
    }

    std::string Bytes(VTX::Frame& frame) {
        VTX::Serialization::ProtoFrameEncoder encoder;
        std::string out;
        encoder.EncodeFrame(frame, out);
        return out;
    }

} // namespace

TEST(FrameBuilder, PrepareForOverwriteThenEverySlotSetIsAFreshContainer) {
    const Schemas s = MakeSchemas();
    for (int seed = 0; seed < 6; ++seed) {
        VTX::PropertyContainer fresh;
        fresh.entity_type_id = kOuterType;
        VTX::Helpers::PreparePropertyContainer(fresh, s.outer);
        FillOuter(fresh, s, seed);

        // Reused after holding another frame's outer entity (the shaped path) ...
        VTX::PropertyContainer shaped;
        VTX::Helpers::PrepareForOverwrite(shaped, s.outer, kOuterType);
        FillOuter(shaped, s, seed + 7);
        VTX::Helpers::PrepareForOverwrite(shaped, s.outer, kOuterType);
        FillOuter(shaped, s, seed);

        // ... and after holding an inner entity (the full reset).
        VTX::PropertyContainer other;
        FillInner(other, s, 3.0);
        VTX::Helpers::PrepareForOverwrite(other, s.outer, kOuterType);
        FillOuter(other, s, seed);

        VTX::Frame a, b, c;
        a.CreateBucket("x").entities.push_back(fresh);
        b.CreateBucket("x").entities.push_back(shaped);
        c.CreateBucket("x").entities.push_back(other);
        a.GetBucket("x").unique_ids = b.GetBucket("x").unique_ids = c.GetBucket("x").unique_ids = {"id"};
        const std::string expected = Bytes(a);
        EXPECT_EQ(Bytes(b), expected) << "seed " << seed;
        EXPECT_EQ(Bytes(c), expected) << "seed " << seed;
    }
}

TEST(FrameBuilder, RefilledFramesAreTheFreshFrames) {
    const Schemas s = MakeSchemas();
    const std::vector<std::string> names = {"first", "second"};
    // Entities per bucket, frame after frame: counts grow and shrink, and positions change struct
    // (a negative seed is an inner entity).
    const std::vector<std::vector<std::vector<int>>> frames = {
        {{1, 2, 3}, {4}}, {{5}, {6, 7, 8, 9}},  {{-1, 10}, {}},      {{11, -2, 12, 13}, {14}},
        {{}, {15, -3}},   {{16, 17, 18}, {19}}, {{20, 21, 22}, {23}}};
    VTX::FrameBuilder builder(names);
    for (size_t f = 0; f < frames.size(); ++f) {
        builder.Begin();
        for (size_t b = 0; b < names.size(); ++b) {
            for (int seed : frames[f][b]) {
                if (seed < 0) {
                    VTX::PropertyContainer& c = builder.Add(b, "id" + std::to_string(seed), s.inner, kInnerType);
                    VTX::Set(c, *VTX::ScalarSlotOf<double>(*VTX::FindField(s.inner, "value")), double(-seed));
                } else {
                    FillOuter(builder.Add(b, "id" + std::to_string(seed), s.outer, kOuterType), s, seed);
                }
            }
        }
        VTX::Frame fresh = FreshFrame(s, frames[f], names);
        EXPECT_EQ(Bytes(builder.Finish()), Bytes(fresh)) << "frame " << f;
    }
}

TEST(FrameBuilder, BeginRebuildsAConsumedFrame) {
    const Schemas s = MakeSchemas();
    VTX::FrameBuilder builder({"only"});
    builder.Begin();
    FillOuter(builder.Add(0, "a", s.outer, kOuterType), s, 1);
    VTX::Frame taken = std::move(builder.Finish()); // a policy that consumed the frame
    builder.Begin();
    FillOuter(builder.Add(0, "a", s.outer, kOuterType), s, 1);
    EXPECT_EQ(Bytes(builder.Finish()), Bytes(taken));
    EXPECT_EQ(builder.Finish().GetBuckets().size(), 1u);
}

TEST(FrameBuilder, ResizeSubArrayKeepsNeighbours) {
    VTX::FlatArray<int> flat;
    flat.AppendSubArray({1, 2});
    flat.AppendSubArray({3});
    flat.AppendSubArray({4, 5, 6});
    auto grown = flat.ResizeSubArray(1, 3);
    ASSERT_EQ(grown.size(), 3u);
    EXPECT_EQ(grown[0], 3);
    EXPECT_EQ(grown[1], 0);
    grown[2] = 9;
    EXPECT_EQ(std::vector<int>(flat.GetSubArray(0).begin(), flat.GetSubArray(0).end()), (std::vector<int> {1, 2}));
    EXPECT_EQ(std::vector<int>(flat.GetSubArray(1).begin(), flat.GetSubArray(1).end()), (std::vector<int> {3, 0, 9}));
    EXPECT_EQ(std::vector<int>(flat.GetSubArray(2).begin(), flat.GetSubArray(2).end()), (std::vector<int> {4, 5, 6}));
    EXPECT_TRUE(flat.ResizeSubArray(0, 0).empty());
    EXPECT_EQ(std::vector<int>(flat.GetSubArray(2).begin(), flat.GetSubArray(2).end()), (std::vector<int> {4, 5, 6}));
    EXPECT_TRUE(flat.ResizeSubArray(3, 1).empty()); // out of bounds
}

TEST(FrameBuilder, SlotsResolveOnlyTheirOwnShape) {
    const Schemas s = MakeSchemas();
    const VTX::SchemaField& value = *VTX::FindField(s.inner, "value");
    EXPECT_TRUE(VTX::ScalarSlotOf<double>(value).has_value());
    EXPECT_FALSE(VTX::ScalarSlotOf<float>(value).has_value());
    EXPECT_FALSE(VTX::ArraySlotOf<double>(value).has_value());
    EXPECT_FALSE(VTX::StructSlotOf(value).has_value());
    const VTX::SchemaField& inners = *VTX::FindField(s.outer, "inners");
    EXPECT_TRUE(VTX::StructArraySlotOf(inners).has_value());
    EXPECT_FALSE(VTX::StructSlotOf(inners).has_value());
    EXPECT_EQ(VTX::FindField(s.outer, "missing"), nullptr);
}
