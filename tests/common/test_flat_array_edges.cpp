// Edge-case tests for VTX::FlatArray<T> -- complements test_flat_array.cpp.
//
// Each test targets a specific class of bug uncovered during the 2026-04-20
// SDK audit: repeated OOB PushBack, ReplaceSubArray with empty spans at
// non-zero indices, Insert-at-end equivalence with PushBack, and other
// boundary cases of the offsets[] invariant.

#include <gtest/gtest.h>
#include <string>
#include "vtx/common/vtx_types.h"

using VTX::FlatArray;

// ---------------------------------------------------------------------------
// PushBack: OOB auto-grow semantics
// ---------------------------------------------------------------------------

TEST(FlatArrayEdges, PushBackTwiceOnSameOOBIndex) {
    // First PushBack auto-grows offsets to SubIndex+1.
    // A second PushBack on the same OOB index must append to the sub-array,
    // not corrupt offsets or overread memory.
    FlatArray<int> arr;
    ASSERT_TRUE(arr.PushBack(3, 10));
    ASSERT_TRUE(arr.PushBack(3, 20));

    EXPECT_EQ(arr.SubArrayCount(), 4u);
    EXPECT_EQ(arr.TotalElementCount(), 2u);
    EXPECT_TRUE(arr.GetSubArray(0).empty());
    EXPECT_TRUE(arr.GetSubArray(1).empty());
    EXPECT_TRUE(arr.GetSubArray(2).empty());
    ASSERT_EQ(arr.GetSubArray(3).size(), 2u);
    EXPECT_EQ(arr.GetSubArray(3)[0], 10);
    EXPECT_EQ(arr.GetSubArray(3)[1], 20);
}

TEST(FlatArrayEdges, PushBackThenInsertOnAutoGrownEmpty) {
    // After auto-grow, the empty intermediate sub-arrays must stay empty even
    // if we keep pushing to the tail.
    FlatArray<int> arr;
    arr.PushBack(3, 100);
    arr.PushBack(3, 200);
    arr.PushBack(3, 300);

    EXPECT_EQ(arr.GetSubArray(0).size(), 0u);
    EXPECT_EQ(arr.GetSubArray(1).size(), 0u);
    EXPECT_EQ(arr.GetSubArray(2).size(), 0u);
    EXPECT_EQ(arr.GetSubArray(3).size(), 3u);
    EXPECT_EQ(arr.TotalElementCount(), 3u);
}

TEST(FlatArrayEdges, PushBackOnSubBeforeTailAfterAutoGrow) {
    // After auto-grow from a PushBack(3), inserting into an intermediate
    // empty sub-array (sub 1) must not smash the tail.
    FlatArray<int> arr;
    arr.PushBack(3, 999);
    ASSERT_TRUE(arr.PushBack(1, 42));

    EXPECT_EQ(arr.GetSubArray(0).size(), 0u);
    ASSERT_EQ(arr.GetSubArray(1).size(), 1u);
    EXPECT_EQ(arr.GetSubArray(1)[0], 42);
    EXPECT_EQ(arr.GetSubArray(2).size(), 0u);
    ASSERT_EQ(arr.GetSubArray(3).size(), 1u);
    EXPECT_EQ(arr.GetSubArray(3)[0], 999);
}

// ---------------------------------------------------------------------------
// InsertSubArray: boundary == AppendSubArray
// ---------------------------------------------------------------------------

TEST(FlatArrayEdges, InsertSubArrayAtSizeEqualsAppend) {
    FlatArray<int> a, b;
    a.AppendSubArray({1, 2});
    a.AppendSubArray({3});
    a.AppendSubArray({4, 5, 6});          // append via AppendSubArray

    b.AppendSubArray({1, 2});
    b.AppendSubArray({3});
    ASSERT_TRUE(b.InsertSubArray(2, {4, 5, 6})); // append via InsertSubArray at size()

    // Structural equality: same sub-arrays, same contents.
    ASSERT_EQ(a.SubArrayCount(), b.SubArrayCount());
    ASSERT_EQ(a.TotalElementCount(), b.TotalElementCount());
    for (size_t i = 0; i < a.SubArrayCount(); ++i) {
        auto sa = a.GetSubArray(i);
        auto sb = b.GetSubArray(i);
        ASSERT_EQ(sa.size(), sb.size()) << "sub " << i;
        for (size_t j = 0; j < sa.size(); ++j) {
            EXPECT_EQ(sa[j], sb[j]) << "sub " << i << " idx " << j;
        }
    }
}

// ---------------------------------------------------------------------------
// ReplaceSubArray: empty span and non-zero indices
// ---------------------------------------------------------------------------

TEST(FlatArrayEdges, ReplaceSubArrayWithEmpty) {
    FlatArray<int> arr;
    arr.AppendSubArray({1, 2, 3});
    arr.AppendSubArray({4, 5});
    arr.AppendSubArray({6});

    ASSERT_TRUE(arr.ReplaceSubArray(0, {}));

    // Sub 0 should be empty now; sub 1 and sub 2 should be preserved and
    // their offsets shifted downward.
    EXPECT_TRUE(arr.GetSubArray(0).empty());
    ASSERT_EQ(arr.GetSubArray(1).size(), 2u);
    EXPECT_EQ(arr.GetSubArray(1)[0], 4);
    EXPECT_EQ(arr.GetSubArray(1)[1], 5);
    ASSERT_EQ(arr.GetSubArray(2).size(), 1u);
    EXPECT_EQ(arr.GetSubArray(2)[0], 6);
    EXPECT_EQ(arr.TotalElementCount(), 3u);
}

TEST(FlatArrayEdges, ReplaceSubArrayAtNonZeroIndexWithEmpty) {
    FlatArray<int> arr;
    arr.AppendSubArray({1, 2, 3});
    arr.AppendSubArray({4, 5});
    arr.AppendSubArray({6, 7});

    ASSERT_TRUE(arr.ReplaceSubArray(1, {}));

    EXPECT_EQ(arr.GetSubArray(0).size(), 3u);
    EXPECT_TRUE(arr.GetSubArray(1).empty());
    ASSERT_EQ(arr.GetSubArray(2).size(), 2u);
    EXPECT_EQ(arr.GetSubArray(2)[0], 6);
    EXPECT_EQ(arr.GetSubArray(2)[1], 7);
    EXPECT_EQ(arr.TotalElementCount(), 5u);
}

// ---------------------------------------------------------------------------
// EraseSubArray / EraseRange corner cases
// ---------------------------------------------------------------------------

TEST(FlatArrayEdges, EraseLastRemainingSubArray) {
    FlatArray<int> arr;
    arr.AppendSubArray({1, 2, 3});
    ASSERT_EQ(arr.SubArrayCount(), 1u);

    ASSERT_TRUE(arr.EraseSubArray(0));

    EXPECT_EQ(arr.SubArrayCount(), 0u);
    EXPECT_EQ(arr.TotalElementCount(), 0u);
    EXPECT_TRUE(arr.GetSubArray(0).empty());  // OOB -> empty silently
}

TEST(FlatArrayEdges, EraseRangeZeroLengthIsNoOp) {
    FlatArray<int> arr;
    arr.AppendSubArray({1, 2, 3, 4, 5});
    const size_t before = arr.TotalElementCount();

    // First == Last: zero-length range -> idempotent no-op.
    ASSERT_TRUE(arr.EraseRange(0, 2, 2));
    EXPECT_EQ(arr.TotalElementCount(), before);

    auto sub = arr.GetSubArray(0);
    ASSERT_EQ(sub.size(), 5u);
    EXPECT_EQ(sub[0], 1);  EXPECT_EQ(sub[2], 3);  EXPECT_EQ(sub[4], 5);
}

// ---------------------------------------------------------------------------
// Insert-at-end vs PushBack equivalence
// ---------------------------------------------------------------------------

TEST(FlatArrayEdges, InsertAtEndEqualsPushBack) {
    FlatArray<int> a, b;
    a.AppendSubArray({10, 20, 30});
    b.AppendSubArray({10, 20, 30});

    ASSERT_TRUE(a.Insert(0, a.GetSubArray(0).size(), 99));   // Insert at end
    ASSERT_TRUE(b.PushBack(0, 99));                           // PushBack

    auto sa = a.GetSubArray(0);
    auto sb = b.GetSubArray(0);
    ASSERT_EQ(sa.size(), sb.size());
    for (size_t i = 0; i < sa.size(); ++i) EXPECT_EQ(sa[i], sb[i]);
}

// ---------------------------------------------------------------------------
// CreateEmptySubArray interactions
// ---------------------------------------------------------------------------

TEST(FlatArrayEdges, DoubleCreateEmptyThenPushBackOnLast) {
    FlatArray<int> arr;
    arr.CreateEmptySubArray();  // sub 0
    arr.CreateEmptySubArray();  // sub 1
    ASSERT_TRUE(arr.PushBack(1, 42));

    EXPECT_EQ(arr.SubArrayCount(), 2u);
    EXPECT_TRUE(arr.GetSubArray(0).empty());
    ASSERT_EQ(arr.GetSubArray(1).size(), 1u);
    EXPECT_EQ(arr.GetSubArray(1)[0], 42);
}

// ---------------------------------------------------------------------------
// Non-trivial element types (strings, move detection through rebase)
// ---------------------------------------------------------------------------

TEST(FlatArrayEdges, ReplaceSubArrayWithLargerSpanContainingMoves) {
    // Use std::string to exercise the non-trivial copy/move paths in
    // data.insert / data.erase during ReplaceSubArray.
    FlatArray<std::string> arr;
    arr.AppendSubArray({std::string("alpha"), std::string("beta")});
    arr.AppendSubArray({std::string("gamma")});

    const std::vector<std::string> replacement = {
        std::string(128, 'x'),              // large string (no SSO)
        std::string("short"),
        std::string(""),                    // empty
        std::string(64, 'y')
    };
    ASSERT_TRUE(arr.ReplaceSubArray(0, std::span<const std::string>(replacement)));

    ASSERT_EQ(arr.GetSubArray(0).size(), 4u);
    EXPECT_EQ(arr.GetSubArray(0)[0].size(), 128u);
    EXPECT_EQ(arr.GetSubArray(0)[1], "short");
    EXPECT_TRUE(arr.GetSubArray(0)[2].empty());
    EXPECT_EQ(arr.GetSubArray(0)[3].size(), 64u);
    ASSERT_EQ(arr.GetSubArray(1).size(), 1u);
    EXPECT_EQ(arr.GetSubArray(1)[0], "gamma");
}

TEST(FlatArrayEdges, OpsOnStringFlatArrayAcrossSsoBoundary) {
    // SSO (small string optimization) typically crosses around 15-22 chars.
    // Mix short and long strings in the same sub-array, then erase/insert.
    FlatArray<std::string> arr;
    arr.AppendSubArray({
        std::string("a"),
        std::string(""),
        std::string("short-ish"),
        std::string(100, 'A'),
        std::string(1, 'Z')
    });

    // Delete the middle (long) element.
    ASSERT_TRUE(arr.Erase(0, 3));
    auto sub = arr.GetSubArray(0);
    ASSERT_EQ(sub.size(), 4u);
    EXPECT_EQ(sub[0], "a");
    EXPECT_TRUE(sub[1].empty());
    EXPECT_EQ(sub[2], "short-ish");
    EXPECT_EQ(sub[3].size(), 1u);

    // Insert a new long string at the front.
    ASSERT_TRUE(arr.Insert(0, 0, std::string(200, 'B')));
    sub = arr.GetSubArray(0);
    ASSERT_EQ(sub.size(), 5u);
    EXPECT_EQ(sub[0].size(), 200u);
    EXPECT_EQ(sub[1], "a");
}

// ---------------------------------------------------------------------------
// FlatBoolArray typedef pinning
// ---------------------------------------------------------------------------

TEST(FlatArrayEdges, FlatBoolArrayIsUint8Based_Regression) {
    // The typedef uses uint8_t to sidestep std::vector<bool>'s bitset quirks
    // (no .data(), no proxy iterators that break span semantics).  If someone
    // "corrects" this to FlatArray<bool>, the code would silently compile but
    // break std::span<const bool> consumers.  Pin it.
    static_assert(std::is_same_v<VTX::FlatBoolArray, VTX::FlatArray<uint8_t>>,
                  "FlatBoolArray must remain FlatArray<uint8_t> for SoA safety");

    VTX::FlatBoolArray arr;
    arr.AppendSubArray({uint8_t{1}, uint8_t{0}, uint8_t{1}});
    ASSERT_EQ(arr.GetSubArray(0).size(), 3u);
    EXPECT_EQ(arr.GetSubArray(0)[0], 1);
    EXPECT_EQ(arr.GetSubArray(0)[1], 0);
    EXPECT_EQ(arr.GetSubArray(0)[2], 1);
}
