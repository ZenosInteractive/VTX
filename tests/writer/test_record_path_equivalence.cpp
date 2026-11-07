// The record path's fast forms against the forms they replace: each must produce exactly
// the same hashes, bucket layout and serialized bytes.
//
//   * Helpers::StoreContainerHashes (each container hashed once, bottom-up) and the buffered
//     XXH3 feed == Helpers::CalculateContainerHash and the plain per-field feed.
//   * SortBucketByTypeId(Bucket&&) (moves; skips reordering a grouped bucket) ==
//     SortBucketByTypeId(const Bucket&, Bucket&) (copies).
//   * ProtobufVtxPolicy::SerializeFrame + SerializeChunkFromFrames == SerializeChunk, and
//     SerializeChunk leaves its frames intact.
//   * ProtobufVtxPolicy::FromNative into a recycled frame message == into a fresh one.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "vtx_schema_generated.h" // complete fbsvtx/cppvtx types before the policy headers
#include "vtx_schema.pb.h"

#include "vtx/common/vtx_types.h"
#include "vtx/common/vtx_types_helpers.h"
#include "vtx/writer/policies/formatters/bucket_type_sort.h"
#include "vtx/writer/policies/formatters/protobuff_vtx_policy.h"

namespace {

    // A container exercising every kind of field the hash and the serializer walk, nested two deep.
    VTX::PropertyContainer RichContainer(int seed, int depth = 2) {
        VTX::PropertyContainer c;
        c.entity_type_id = seed % 3;
        c.bool_properties = {true, false, (seed & 1) != 0};
        c.int32_properties = {seed, -seed, 7};
        c.int64_properties = {int64_t {1} << 40, seed};
        c.float_properties = {1.5f * static_cast<float>(seed), -0.0f};
        c.double_properties = {3.25 * seed};
        c.string_properties = {"name_" + std::to_string(seed), "",
                               std::string(700, static_cast<char>('a' + seed % 20))};
        c.vector_properties = {{1.0, 2.0, static_cast<double>(seed)}};
        c.quat_properties = {{0.0f, 0.0f, 0.0f, 1.0f}};
        c.transform_properties.resize(1);
        c.transform_properties[0].translation = {static_cast<double>(seed), 0.0, 5.0};
        c.range_properties = {{0.0f, 10.0f, 0.5f}};
        c.int32_arrays.data = {1, 2, 3, static_cast<int32_t>(seed)};
        c.int32_arrays.offsets = {0, 2};
        c.float_arrays.data = {0.25f, 0.5f};
        c.float_arrays.offsets = {0};
        c.bool_arrays.data = {1, 0, 1};
        c.bool_arrays.offsets = {0};
        c.string_arrays.data = {"x", "yy", std::to_string(seed)};
        c.string_arrays.offsets = {0, 1};
        c.byte_array_properties.data = {0x01, 0x02, static_cast<uint8_t>(seed)};
        c.byte_array_properties.offsets = {0};
        c.vector_arrays.data = {{1.0, 1.0, 1.0}, {2.0, 2.0, 2.0}};
        c.vector_arrays.offsets = {0};
        if (depth > 0) {
            c.any_struct_properties = {RichContainer(seed + 1, depth - 1)};
            c.any_struct_arrays.data = {RichContainer(seed + 2, depth - 1), RichContainer(seed + 3, depth - 1)};
            c.any_struct_arrays.offsets = {0, 1};
            VTX::MapContainer map;
            map.keys = {"k1", "k2"};
            map.values = {RichContainer(seed + 4, depth - 1), RichContainer(seed + 5, depth - 1)};
            c.map_properties = {map};
            c.map_arrays.data = {map};
            c.map_arrays.offsets = {0};
        }
        return c;
    }

    VTX::Frame RichFrame(int seed, bool grouped) {
        VTX::Frame frame;
        frame.CreateBucket("players");
        frame.CreateBucket("props"); // both first: a new bucket can move the ones before it
        auto& players = frame.GetBucket("players");
        auto& props = frame.GetBucket("props");
        for (int i = 0; i < 6; ++i) {
            VTX::PropertyContainer e = RichContainer(seed + i);
            e.entity_type_id = grouped ? i / 2 : (5 - i) % 3;
            players.entities.push_back(std::move(e));
            players.unique_ids.push_back("p" + std::to_string(seed + i));
        }
        props.entities.push_back(RichContainer(seed + 100, 0));
        props.unique_ids.push_back("prop");
        return frame;
    }

    void ExpectSameBucket(const VTX::Bucket& a, const VTX::Bucket& b) {
        EXPECT_EQ(a.unique_ids, b.unique_ids);
        ASSERT_EQ(a.entities.size(), b.entities.size());
        for (size_t i = 0; i < a.entities.size(); ++i) {
            EXPECT_EQ(a.entities[i].entity_type_id, b.entities[i].entity_type_id) << "entity " << i;
            EXPECT_EQ(VTX::Helpers::CalculateContainerHash(a.entities[i]),
                      VTX::Helpers::CalculateContainerHash(b.entities[i]))
                << "entity " << i;
        }
        ASSERT_EQ(a.type_ranges.size(), b.type_ranges.size());
        for (size_t t = 0; t < a.type_ranges.size(); ++t) {
            EXPECT_EQ(a.type_ranges[t].start_index, b.type_ranges[t].start_index) << "type " << t;
            EXPECT_EQ(a.type_ranges[t].count, b.type_ranges[t].count) << "type " << t;
        }
    }

    void ExpectMoveSortMatchesCopySort(const VTX::Bucket& src) {
        VTX::Bucket copied;
        VTX::Serialization::SortBucketByTypeId(src, copied);
        VTX::Bucket moved;
        VTX::Serialization::SortBucketByTypeId(VTX::Bucket(src), moved);
        ExpectSameBucket(copied, moved);
    }

    std::string Serialized(const cppvtx::Frame& frame) {
        std::string out;
        frame.SerializeToString(&out);
        return out;
    }

} // namespace

TEST(RecordPathEquivalence, StoredHashesEqualCalculatedHashes) {
    for (int seed = 0; seed < 5; ++seed) {
        VTX::PropertyContainer c = RichContainer(seed);
        const uint64_t calculated = VTX::Helpers::CalculateContainerHash(c);
        EXPECT_EQ(VTX::Helpers::StoreContainerHashes(c), calculated);
        EXPECT_EQ(c.content_hash, calculated);
        EXPECT_EQ(c.any_struct_properties[0].content_hash,
                  VTX::Helpers::CalculateContainerHash(c.any_struct_properties[0]));
        EXPECT_EQ(c.any_struct_arrays.data[1].content_hash,
                  VTX::Helpers::CalculateContainerHash(c.any_struct_arrays.data[1]));
        EXPECT_EQ(c.map_properties[0].values[1].content_hash,
                  VTX::Helpers::CalculateContainerHash(c.map_properties[0].values[1]));
        EXPECT_EQ(c.map_arrays.data[0].values[0].content_hash,
                  VTX::Helpers::CalculateContainerHash(c.map_arrays.data[0].values[0]));
    }
}

TEST(RecordPathEquivalence, BufferedHashFeedsTheSameBytes) {
    // A container with only the fields at the head of the hash order: type id, bools (one byte each),
    // int32s, then strings -- including one longer than the hasher's buffer.
    VTX::PropertyContainer c;
    c.entity_type_id = 42;
    c.bool_properties = {true, false, true};
    c.int32_properties = {1, -2, 3};
    c.string_properties = {"short", std::string(1500, 'z'), "", "tail"};

    std::string bytes;
    bytes.append(reinterpret_cast<const char*>(&c.entity_type_id), sizeof(c.entity_type_id));
    for (bool b : c.bool_properties)
        bytes.push_back(b ? 1 : 0);
    bytes.append(reinterpret_cast<const char*>(c.int32_properties.data()), c.int32_properties.size() * sizeof(int32_t));
    for (const auto& s : c.string_properties)
        bytes.append(s);

    EXPECT_EQ(VTX::Helpers::CalculateContainerHash(c), XXH3_64bits(bytes.data(), bytes.size()));
}

TEST(RecordPathEquivalence, MoveSortMatchesCopySort) {
    // Already grouped by type: moved over whole.
    ExpectMoveSortMatchesCopySort(RichFrame(1, /*grouped*/ true).GetBucket("players"));
    // Out of type order: reordered.
    ExpectMoveSortMatchesCopySort(RichFrame(2, /*grouped*/ false).GetBucket("players"));

    // Grouped with a type id gap (type 1 absent): the empty range starts where type 2 starts.
    VTX::Bucket gap;
    for (int t : {0, 0, 2, 3, 3}) {
        VTX::PropertyContainer e;
        e.entity_type_id = t;
        e.int32_properties = {t};
        gap.entities.push_back(e);
        gap.unique_ids.push_back("id" + std::to_string(gap.unique_ids.size()));
    }
    ExpectMoveSortMatchesCopySort(gap);

    // More ids than entities, and fewer.
    VTX::Bucket more_ids = gap;
    more_ids.unique_ids.push_back("extra");
    ExpectMoveSortMatchesCopySort(more_ids);
    VTX::Bucket fewer_ids = gap;
    fewer_ids.unique_ids.resize(2);
    ExpectMoveSortMatchesCopySort(fewer_ids);

    // An entity with no type: dropped by both.
    VTX::Bucket untyped = gap;
    untyped.entities[1].entity_type_id = -1;
    ExpectMoveSortMatchesCopySort(untyped);

    // Empty, and all untyped: copied over as they are.
    ExpectMoveSortMatchesCopySort(VTX::Bucket {});
    VTX::Bucket none;
    none.entities.resize(2);
    none.unique_ids = {"a", "b"};
    ExpectMoveSortMatchesCopySort(none);
}

TEST(RecordPathEquivalence, ChunkFromSerializedFramesMatchesSerializeChunk) {
    std::vector<std::unique_ptr<cppvtx::Frame>> frames;
    for (int i = 0; i < 4; ++i)
        frames.push_back(VTX::ProtobufVtxPolicy::FromNative(RichFrame(i * 10, (i & 1) == 0)));

    std::vector<std::string> serialized;
    for (const auto& frame : frames) {
        std::string bytes = "stale contents a reused buffer may hold";
        const size_t size = VTX::ProtobufVtxPolicy::SerializeFrame(*frame, bytes);
        EXPECT_EQ(size, VTX::ProtobufVtxPolicy::GetFrameSize(*frame));
        EXPECT_EQ(bytes, Serialized(*frame));
        serialized.push_back(std::move(bytes));
    }

    for (int32_t chunk_index : {0, 1, 7, 300}) {
        for (bool compressed : {false, true}) {
            const std::string before = Serialized(*frames[2]);
            const std::string reference = VTX::ProtobufVtxPolicy::SerializeChunk(frames, chunk_index, compressed);
            EXPECT_EQ(VTX::ProtobufVtxPolicy::SerializeChunkFromFrames(serialized, chunk_index, compressed), reference)
                << "chunk " << chunk_index << (compressed ? " compressed" : "");
            // SerializeChunk borrowed the frames and gave them back unchanged.
            EXPECT_EQ(Serialized(*frames[2]), before);
        }
    }
    EXPECT_TRUE(VTX::ProtobufVtxPolicy::SerializeChunkFromFrames({}, 3, true).empty());
}

TEST(RecordPathEquivalence, RecycledFrameMessageMatchesFreshOne) {
    // A message that held a larger frame, refilled with a smaller, differently shaped one.
    std::unique_ptr<cppvtx::Frame> recycled = VTX::ProtobufVtxPolicy::FromNative(RichFrame(1, true));
    recycled = VTX::ProtobufVtxPolicy::FromNative(RichFrame(50, false), std::move(recycled));

    VTX::Frame small;
    auto& bucket = small.CreateBucket("players");
    VTX::PropertyContainer e;
    e.entity_type_id = 0;
    e.float_properties = {1.0f};
    bucket.entities.push_back(e);
    bucket.unique_ids.push_back("only");
    VTX::Frame small_copy = small;

    recycled = VTX::ProtobufVtxPolicy::FromNative(std::move(small), std::move(recycled));
    const std::unique_ptr<cppvtx::Frame> fresh = VTX::ProtobufVtxPolicy::FromNative(std::move(small_copy));
    EXPECT_EQ(Serialized(*recycled), Serialized(*fresh));
}
