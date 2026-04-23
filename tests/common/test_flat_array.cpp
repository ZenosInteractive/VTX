// Tests for VTX::FlatArray<T> -- the SoA sub-array container.
//
// FlatArray<T> is the workhorse of the writer/reader property system, so we
// cover every public mutation and the OOB / edge behaviour individually.

#include <gtest/gtest.h>
#include "vtx/common/vtx_types.h"

using VTX::FlatArray;

// ---------------------------------------------------------------------------
// Construction / empty state
// ---------------------------------------------------------------------------

TEST(FlatArray, DefaultConstructsEmpty) {
    FlatArray<int> arr;
    EXPECT_EQ(arr.SubArrayCount(), 0u);
    EXPECT_EQ(arr.TotalElementCount(), 0u);
    EXPECT_TRUE(arr.GetSubArray(0).empty()); // OOB read is silent-empty
}

TEST(FlatArray, ClearResetsEverything) {
    FlatArray<int> arr;
    arr.AppendSubArray({1, 2, 3});
    arr.AppendSubArray({4, 5});
    ASSERT_EQ(arr.SubArrayCount(), 2u);
    arr.Clear();
    EXPECT_EQ(arr.SubArrayCount(), 0u);
    EXPECT_EQ(arr.TotalElementCount(), 0u);
}

// ---------------------------------------------------------------------------
// AppendSubArray
// ---------------------------------------------------------------------------

TEST(FlatArray, AppendSubArrayStoresItemsContiguously) {
    FlatArray<int> arr;
    arr.AppendSubArray({10, 20, 30});
    arr.AppendSubArray({40, 50});

    ASSERT_EQ(arr.SubArrayCount(), 2u);
    EXPECT_EQ(arr.TotalElementCount(), 5u);

    auto sub0 = arr.GetSubArray(0);
    auto sub1 = arr.GetSubArray(1);
    ASSERT_EQ(sub0.size(), 3u);
    ASSERT_EQ(sub1.size(), 2u);
    EXPECT_EQ(sub0[0], 10);
    EXPECT_EQ(sub0[1], 20);
    EXPECT_EQ(sub0[2], 30);
    EXPECT_EQ(sub1[0], 40);
    EXPECT_EQ(sub1[1], 50);
}

TEST(FlatArray, AppendEmptySubArrayStillCountsAsOne) {
    FlatArray<int> arr;
    arr.AppendSubArray({});
    arr.AppendSubArray({7});
    EXPECT_EQ(arr.SubArrayCount(), 2u);
    EXPECT_TRUE(arr.GetSubArray(0).empty());
    EXPECT_EQ(arr.GetSubArray(1).size(), 1u);
}

TEST(FlatArray, CreateEmptySubArrayAppendsAMarker) {
    FlatArray<int> arr;
    arr.AppendSubArray({1, 2});
    arr.CreateEmptySubArray();
    arr.AppendSubArray({9});
    ASSERT_EQ(arr.SubArrayCount(), 3u);
    EXPECT_EQ(arr.GetSubArray(0).size(), 2u);
    EXPECT_TRUE(arr.GetSubArray(1).empty());
    EXPECT_EQ(arr.GetSubArray(2).size(), 1u);
}

// ---------------------------------------------------------------------------
// InsertSubArray / EraseSubArray
// ---------------------------------------------------------------------------

TEST(FlatArray, InsertSubArrayAtFrontShiftsExisting) {
    FlatArray<int> arr;
    arr.AppendSubArray({10, 20});
    arr.AppendSubArray({30});
    ASSERT_TRUE(arr.InsertSubArray(0, {1, 2, 3}));

    ASSERT_EQ(arr.SubArrayCount(), 3u);
    EXPECT_EQ(arr.GetSubArray(0).size(), 3u);
    EXPECT_EQ(arr.GetSubArray(1).size(), 2u);
    EXPECT_EQ(arr.GetSubArray(2).size(), 1u);
    EXPECT_EQ(arr.GetSubArray(0)[0], 1);
    EXPECT_EQ(arr.GetSubArray(1)[0], 10);
    EXPECT_EQ(arr.GetSubArray(2)[0], 30);
}

TEST(FlatArray, InsertAtSizeEqualsAppend) {
    FlatArray<int> arr;
    arr.AppendSubArray({1});
    ASSERT_TRUE(arr.InsertSubArray(1, {2, 3}));
    EXPECT_EQ(arr.SubArrayCount(), 2u);
    EXPECT_EQ(arr.GetSubArray(1).size(), 2u);
}

TEST(FlatArray, EraseSubArrayRemovesElementsAndShifts) {
    FlatArray<int> arr;
    arr.AppendSubArray({1, 2});
    arr.AppendSubArray({3, 4, 5});
    arr.AppendSubArray({6});
    ASSERT_TRUE(arr.EraseSubArray(1));

    ASSERT_EQ(arr.SubArrayCount(), 2u);
    EXPECT_EQ(arr.GetSubArray(0).size(), 2u);
    EXPECT_EQ(arr.GetSubArray(1).size(), 1u);
    EXPECT_EQ(arr.GetSubArray(1)[0], 6);
    EXPECT_EQ(arr.TotalElementCount(), 3u); // 2 + 1
}

TEST(FlatArray, EraseSubArrayOutOfBoundsReturnsFalse) {
    FlatArray<int> arr;
    arr.AppendSubArray({1, 2});
    EXPECT_FALSE(arr.EraseSubArray(5));
    EXPECT_EQ(arr.SubArrayCount(), 1u);
}

// ---------------------------------------------------------------------------
// ReplaceSubArray
// ---------------------------------------------------------------------------

TEST(FlatArray, ReplaceSubArrayWithSmallerShrinks) {
    FlatArray<int> arr;
    arr.AppendSubArray({1, 2, 3, 4});
    arr.AppendSubArray({5, 6});
    ASSERT_TRUE(arr.ReplaceSubArray(0, {9}));

    ASSERT_EQ(arr.SubArrayCount(), 2u);
    EXPECT_EQ(arr.GetSubArray(0).size(), 1u);
    EXPECT_EQ(arr.GetSubArray(0)[0], 9);
    EXPECT_EQ(arr.GetSubArray(1)[0], 5);
}

TEST(FlatArray, ReplaceSubArrayWithLargerGrows) {
    FlatArray<int> arr;
    arr.AppendSubArray({1});
    arr.AppendSubArray({5, 6});
    ASSERT_TRUE(arr.ReplaceSubArray(0, {10, 11, 12}));

    EXPECT_EQ(arr.GetSubArray(0).size(), 3u);
    EXPECT_EQ(arr.GetSubArray(1).size(), 2u);
    EXPECT_EQ(arr.GetSubArray(1)[0], 5);
    EXPECT_EQ(arr.GetSubArray(1)[1], 6);
}

// ---------------------------------------------------------------------------
// Element-level ops: PushBack / Insert / Replace / Erase / EraseRange
// ---------------------------------------------------------------------------

TEST(FlatArray, PushBackAppendsToSubArray) {
    FlatArray<int> arr;
    arr.AppendSubArray({1, 2});
    ASSERT_TRUE(arr.PushBack(0, 3));
    EXPECT_EQ(arr.GetSubArray(0).size(), 3u);
    EXPECT_EQ(arr.GetSubArray(0)[2], 3);
}

TEST(FlatArray, PushBackOnOutOfBoundsSubIndexAutoGrowsOffsets) {
    // Asymmetric behaviour: PushBack on OOB creates empty sub-arrays up to
    // SubIndex.  Documented quirk -- test pins it.
    FlatArray<int> arr;
    ASSERT_TRUE(arr.PushBack(3, 42));
    EXPECT_GE(arr.SubArrayCount(), 4u);
    EXPECT_EQ(arr.GetSubArray(3).size(), 1u);
    EXPECT_EQ(arr.GetSubArray(3)[0], 42);
}

TEST(FlatArray, InsertInsertsAtExactPosition) {
    FlatArray<int> arr;
    arr.AppendSubArray({10, 20, 30});
    ASSERT_TRUE(arr.Insert(0, 1, 99));
    auto sub = arr.GetSubArray(0);
    ASSERT_EQ(sub.size(), 4u);
    EXPECT_EQ(sub[0], 10);
    EXPECT_EQ(sub[1], 99);
    EXPECT_EQ(sub[2], 20);
    EXPECT_EQ(sub[3], 30);
}

TEST(FlatArray, ReplaceAtPositionUpdatesInPlace) {
    FlatArray<int> arr;
    arr.AppendSubArray({10, 20, 30});
    ASSERT_TRUE(arr.Replace(0, 1, 999));
    EXPECT_EQ(arr.GetSubArray(0)[1], 999);
    EXPECT_EQ(arr.GetSubArray(0).size(), 3u);
}

TEST(FlatArray, EraseRangeRemovesContiguousRun) {
    FlatArray<int> arr;
    arr.AppendSubArray({1, 2, 3, 4, 5});
    ASSERT_TRUE(arr.EraseRange(0, 1, 4)); // remove indexes [1,4)  = 2,3,4
    auto sub = arr.GetSubArray(0);
    ASSERT_EQ(sub.size(), 2u);
    EXPECT_EQ(sub[0], 1);
    EXPECT_EQ(sub[1], 5);
}

TEST(FlatArray, InsertReplaceEraseReturnFalseOnBadSubIndex) {
    FlatArray<int> arr;
    arr.AppendSubArray({1});
    EXPECT_FALSE(arr.Insert(5, 0, 99));
    EXPECT_FALSE(arr.Replace(5, 0, 99));
    EXPECT_FALSE(arr.Erase(5, 0));
}

// ---------------------------------------------------------------------------
// Mutable access / multi-type safety
// ---------------------------------------------------------------------------

TEST(FlatArray, GetMutableSubArrayAllowsInPlaceEdit) {
    FlatArray<int> arr;
    arr.AppendSubArray({1, 2, 3});
    auto sub = arr.GetMutableSubArray(0);
    ASSERT_EQ(sub.size(), 3u);
    sub[0] = 100;
    sub[2] = 300;
    auto readback = arr.GetSubArray(0);
    EXPECT_EQ(readback[0], 100);
    EXPECT_EQ(readback[1], 2);
    EXPECT_EQ(readback[2], 300);
}

TEST(FlatArray, WorksWithNonTrivialTypes) {
    FlatArray<std::string> arr;
    arr.AppendSubArray({std::string("alpha"), std::string("bravo")});
    arr.PushBack(0, std::string("charlie"));
    auto sub = arr.GetSubArray(0);
    ASSERT_EQ(sub.size(), 3u);
    EXPECT_EQ(sub[2], "charlie");
}
