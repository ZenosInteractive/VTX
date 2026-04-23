// Tests for VTX::Bucket and VTX::Frame.

#include <gtest/gtest.h>
#include "vtx/common/vtx_types.h"

// ---------------------------------------------------------------------------
// Frame::CreateBucket / GetBucket (non-const)
// ---------------------------------------------------------------------------

TEST(Frame, CreateBucketReturnsMutableReference) {
    VTX::Frame frame;
    auto& bucket = frame.CreateBucket("entity");
    bucket.unique_ids.push_back("id_0");

    EXPECT_EQ(frame.GetBuckets().size(), 1u);
    EXPECT_EQ(frame.bucket_map.at("entity"), 0u);
    EXPECT_EQ(frame.GetBuckets()[0].unique_ids.size(), 1u);
}

TEST(Frame, CreateBucketIsIdempotent) {
    VTX::Frame frame;
    auto& b1 = frame.CreateBucket("entity");
    b1.unique_ids.push_back("id_0");

    auto& b2 = frame.CreateBucket("entity");
    b2.unique_ids.push_back("id_1");

    EXPECT_EQ(frame.GetBuckets().size(), 1u); // same bucket
    EXPECT_EQ(frame.GetBuckets()[0].unique_ids.size(), 2u);
    EXPECT_EQ(&b1, &b2); // same reference
}

TEST(Frame, MultipleBucketsKeepStableIndices) {
    VTX::Frame frame;
    frame.CreateBucket("A");
    frame.CreateBucket("B");
    frame.CreateBucket("C");

    EXPECT_EQ(frame.bucket_map.at("A"), 0u);
    EXPECT_EQ(frame.bucket_map.at("B"), 1u);
    EXPECT_EQ(frame.bucket_map.at("C"), 2u);
}

// ---------------------------------------------------------------------------
// Bucket::GetEntitiesOfType
// ---------------------------------------------------------------------------

TEST(Bucket, GetEntitiesOfTypeReturnsCorrectSpan) {
    VTX::Bucket bucket;
    // 3 Players, 2 Projectiles ordered by type
    for (int i = 0; i < 3; ++i) {
        VTX::PropertyContainer pc;
        pc.entity_type_id = 0;
        bucket.entities.push_back(std::move(pc));
        bucket.unique_ids.push_back("player_" + std::to_string(i));
    }
    for (int i = 0; i < 2; ++i) {
        VTX::PropertyContainer pc;
        pc.entity_type_id = 1;
        bucket.entities.push_back(std::move(pc));
        bucket.unique_ids.push_back("proj_" + std::to_string(i));
    }
    bucket.type_ranges = {
        {0, 3}, // type 0: indices 0..2
        {3, 2}, // type 1: indices 3..4
    };

    auto players = bucket.GetEntitiesOfType(0);
    auto projs = bucket.GetEntitiesOfType(1);

    EXPECT_EQ(players.size(), 3u);
    EXPECT_EQ(projs.size(), 2u);
    for (const auto& p : players)
        EXPECT_EQ(p.entity_type_id, 0);
    for (const auto& p : projs)
        EXPECT_EQ(p.entity_type_id, 1);
}

TEST(Bucket, GetEntitiesOfTypeReturnsEmptyForUnknownId) {
    VTX::Bucket bucket;
    bucket.type_ranges = {{0, 1}}; // only type 0 registered
    auto span = bucket.GetEntitiesOfType(5);
    EXPECT_TRUE(span.empty());
}

TEST(Bucket, GetEntitiesOfTypeWithEnumOverloadCompiles) {
    // The templated GetEntitiesOfType<TEnum> is a thin cast -- just make sure
    // it compiles + returns something plausible when the enum value is in range.
    enum class ArenaEntity : int32_t { Player = 0, Projectile = 1 };
    VTX::Bucket bucket;
    bucket.entities.emplace_back();
    bucket.type_ranges = {{0, 1}};

    auto players = bucket.GetEntitiesOfType(ArenaEntity::Player);
    EXPECT_EQ(players.size(), 1u);
}
