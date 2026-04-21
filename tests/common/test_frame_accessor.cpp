// Tests for FrameAccessor and EntityView.

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

#include "vtx/reader/core/vtx_frame_accessor.h"

namespace {

VTX::PropertyContainer MakeCompanion(int32_t level)
{
    VTX::PropertyContainer pc;
    pc.int32_properties = {level};
    return pc;
}

VTX::PropertyAddressCache BuildCache()
{
    VTX::PropertyAddressCache cache;

    cache.name_to_id["Player"] = 7;
    auto& player = cache.structs[7];
    player.name = "Player";
    player.properties["Name"] = {
        .index = 0,
        .type_id = VTX::FieldType::String,
        .container_type = VTX::FieldContainerType::None
    };
    player.properties["Score"] = {
        .index = 0,
        .type_id = VTX::FieldType::Int32,
        .container_type = VTX::FieldContainerType::None
    };
    player.properties["Health"] = {
        .index = 0,
        .type_id = VTX::FieldType::Float,
        .container_type = VTX::FieldContainerType::None
    };
    player.properties["IsAlive"] = {
        .index = 0,
        .type_id = VTX::FieldType::Bool,
        .container_type = VTX::FieldContainerType::None
    };
    player.properties["Inventory"] = {
        .index = 0,
        .type_id = VTX::FieldType::Int32,
        .container_type = VTX::FieldContainerType::Array
    };
    player.properties["Companion"] = {
        .index = 0,
        .type_id = VTX::FieldType::Struct,
        .container_type = VTX::FieldContainerType::None,
        .child_type_name = "Companion"
    };
    player.properties["History"] = {
        .index = 0,
        .type_id = VTX::FieldType::Struct,
        .container_type = VTX::FieldContainerType::Array,
        .child_type_name = "Companion"
    };
    player.property_order = {
        "Name", "Score", "Health", "IsAlive", "Inventory", "Companion", "History"
    };

    cache.name_to_id["Companion"] = 8;
    auto& companion = cache.structs[8];
    companion.name = "Companion";
    companion.properties["Level"] = {
        .index = 0,
        .type_id = VTX::FieldType::Int32,
        .container_type = VTX::FieldContainerType::None
    };
    companion.property_order = {"Level"};

    return cache;
}

VTX::PropertyContainer BuildEntity()
{
    VTX::PropertyContainer pc;
    pc.string_properties = {"Alpha"};
    pc.int32_properties = {7};
    pc.float_properties = {75.0f};
    pc.bool_properties = {true};
    pc.int32_arrays.AppendSubArray({10, 20, 30});
    pc.any_struct_properties = {MakeCompanion(42)};
    pc.any_struct_arrays.AppendSubArray({MakeCompanion(1), MakeCompanion(2)});
    return pc;
}

} // namespace

TEST(FrameAccessor, ResolvesKeysAndReportsAvailableMetadata)
{
    VTX::FrameAccessor accessor;
    accessor.InitializeFromCache(BuildCache());

    EXPECT_TRUE(accessor.Get<std::string>("Player", "Name").IsValid());
    EXPECT_TRUE(accessor.Get<int32_t>("Player", "Score").IsValid());
    EXPECT_TRUE(accessor.GetArray<int32_t>("Player", "Inventory").IsValid());
    EXPECT_TRUE(accessor.GetViewKey("Player", "Companion").IsValid());
    EXPECT_TRUE(accessor.GetViewArrayKey("Player", "History").IsValid());

    EXPECT_FALSE(accessor.Get<float>("Player", "Name").IsValid());
    EXPECT_FALSE(accessor.Get<std::string>("Ghost", "Name").IsValid());
    EXPECT_FALSE(accessor.GetArray<float>("Player", "Inventory").IsValid());

    EXPECT_TRUE(accessor.HasProperty("Player", "Health"));
    EXPECT_FALSE(accessor.HasProperty("Player", "Missing"));

    const auto names = accessor.GetAvailableStructNames();
    EXPECT_NE(std::find(names.begin(), names.end(), "Player"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "Companion"), names.end());

    const auto props = accessor.GetPropertiesForStruct("Player");
    EXPECT_EQ(props.size(), 7u);
    EXPECT_TRUE(accessor.GetPropertiesForStruct("Ghost").empty());
}

TEST(FrameAccessor, EntityViewReadsScalarArrayAndNestedProperties)
{
    VTX::FrameAccessor accessor;
    accessor.InitializeFromCache(BuildCache());

    const VTX::PropertyContainer entity = BuildEntity();
    const VTX::EntityView view(entity);

    const auto name_key = accessor.Get<std::string>("Player", "Name");
    const auto score_key = accessor.Get<int32_t>("Player", "Score");
    const auto health_key = accessor.Get<float>("Player", "Health");
    const auto alive_key = accessor.Get<bool>("Player", "IsAlive");
    const auto inventory_key = accessor.GetArray<int32_t>("Player", "Inventory");
    const auto companion_key = accessor.GetViewKey("Player", "Companion");
    const auto history_key = accessor.GetViewArrayKey("Player", "History");
    const auto level_key = accessor.Get<int32_t>("Companion", "Level");

    EXPECT_EQ(view.Get(name_key), "Alpha");
    EXPECT_EQ(view.Get(score_key), 7);
    EXPECT_FLOAT_EQ(view.Get(health_key), 75.0f);
    EXPECT_TRUE(view.Get(alive_key));

    const auto inventory = view.GetArray(inventory_key);
    ASSERT_EQ(inventory.size(), 3u);
    EXPECT_EQ(inventory[0], 10);
    EXPECT_EQ(inventory[2], 30);

    const auto companion = view.GetView(companion_key);
    EXPECT_EQ(companion.Get(level_key), 42);

    const auto history = view.GetViewArray(history_key);
    ASSERT_EQ(history.size(), 2u);
    EXPECT_EQ(VTX::EntityView(history[0]).Get(level_key), 1);
    EXPECT_EQ(VTX::EntityView(history[1]).Get(level_key), 2);
}

TEST(FrameAccessor, InvalidKeysReturnDefaultsOrEmptyViews)
{
    VTX::FrameAccessor accessor;
    accessor.InitializeFromCache(BuildCache());

    const VTX::PropertyContainer entity = BuildEntity();
    const VTX::EntityView view(entity);
    const auto bad_string_key = accessor.Get<std::string>("Ghost", "Name");
    const auto bad_array_key = accessor.GetArray<int32_t>("Ghost", "Inventory");
    const auto bad_view_key = accessor.GetViewKey("Ghost", "Companion");
    const auto level_key = accessor.Get<int32_t>("Companion", "Level");

    EXPECT_FALSE(bad_string_key.IsValid());
    EXPECT_TRUE(view.Get(bad_string_key).empty());
    EXPECT_TRUE(view.GetArray(bad_array_key).empty());
    EXPECT_EQ(view.GetView(bad_view_key).Get(level_key), 0);
}
