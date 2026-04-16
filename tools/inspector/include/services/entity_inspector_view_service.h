#pragma once

#include <string>
#include <vector>

#include "vtx/common/vtx_property_cache.h"
#include "vtx/common/vtx_types.h"
#include "vtx/reader/core/vtx_reader_facade.h"

namespace VTX {
class IVtxReaderFacade;
struct Frame;
}

namespace VtxServices {

struct EntityListItem {
    std::string entity_id;
    int entity_type_id = -1;
    std::string entity_type_name;
    bool is_selected = false;
};

struct EntityBucketView {
    std::string id;
    std::string label;
    int bucket_index = -1;
    std::vector<EntityListItem> entities;
};

struct FrameResolveContext {
    bool is_scrubbing = false;
    int current_frame = 0;
    int last_drawn_frame_index = -1;
};

struct FrameResolveResult {
    const VTX::Frame* frame_to_draw = nullptr;
    bool is_loading = false;
    bool showing_stale_frame = false;
    int stale_frame_index = -1;
    std::string status_message;
    int next_last_drawn_frame_index = -1;
};

struct EntityPropertyNode {
    std::string id;
    std::string label;
    std::string schema_label;
    std::string value;
    std::string raw_value;
    std::string struct_name;
    std::string schema_focus_struct_name;
    std::string schema_focus_property_name;
    int schema_index = -1;
    VTX::FieldType schema_type = VTX::FieldType::None;
    VTX::FieldContainerType schema_container_type = VTX::FieldContainerType::None;
    bool is_property = false;
    bool open_by_default = false;
    bool schema_focus_on_secondary_click = false;
    std::vector<EntityPropertyNode> children;
};

struct EntityPropertiesViewModel {
    std::vector<EntityPropertyNode> roots;
};

struct EntityPropertyCommand {
    bool copy_to_clipboard = false;
    std::string clipboard_text;
    bool request_schema_focus = false;
    std::string struct_name;
    std::string property_name;
};

enum class EntityStatusTone {
    Normal,
    Warning,
    Error,
};

struct EntityInspectorState {
    int last_drawn_frame_index = -1;
    int selected_bucket_index = -1;
    std::string selected_entity_id;
};

struct EntityInspectorHeaderViewModel {
    std::string entity_label;
    std::string type_label;
    std::string type_name;
    std::string hash_label;
    bool show_hash = false;
    std::vector<std::string> missing_field_names;
};

struct EntityInspectorViewModel {
    bool has_replay = false;
    bool is_loading = false;
    bool disable_panels = false;
    bool has_frame = false;
    std::string status_message;
    std::string stale_frame_message;
    EntityStatusTone status_tone = EntityStatusTone::Normal;
    std::vector<EntityBucketView> buckets;
    EntityPropertiesViewModel properties;
    EntityInspectorHeaderViewModel header;
    std::string empty_properties_message;
};

struct EntityInspectorEffect {
    bool copy_to_clipboard = false;
    std::string clipboard_text;
    bool request_schema_focus = false;
    bool focus_schema_window = false;
    std::string schema_struct_name;
    std::string schema_property_name;
};

struct EntityInspectorScreenResult {
    EntityInspectorState state;
    EntityInspectorViewModel view_model;
};

class EntityInspectorViewService {
public:
    static FrameResolveResult ResolveFrameToDraw(VTX::IVtxReaderFacade& reader, const FrameResolveContext& context);
    static std::vector<EntityBucketView> BuildEntityBuckets(
        const VTX::Frame& frame,
        const EntityInspectorState& state,
        const VTX::PropertyAddressCache* global_cache);
    static EntityPropertiesViewModel BuildPropertyTree(
        const VTX::PropertyContainer& pc,
        const VTX::PropertyAddressCache* global_cache);
    static EntityInspectorScreenResult BuildScreen(
        VTX::IVtxReaderFacade* reader,
        bool has_loaded_replay,
        bool is_scrubbing_timeline,
        int current_frame,
        const EntityInspectorState& state);
    static EntityInspectorState SelectEntity(
        const EntityInspectorState& state,
        int bucket_index,
        const std::string& entity_id);
    static EntityInspectorEffect BuildPropertyActivateEffect(
        const EntityPropertyNode& node,
        const VTX::PropertyAddressCache* global_cache);
    static EntityInspectorEffect BuildPropertyContextEffect(
        const EntityPropertyNode& node,
        const VTX::PropertyAddressCache* global_cache);
    static const VTX::PropertyContainer* FindSelectedEntity(
        const VTX::Frame& frame,
        const EntityInspectorState& state);
    static EntityInspectorViewModel BuildFrameViewModel(
        const VTX::Frame& frame,
        const EntityInspectorState& state,
        const VTX::PropertyAddressCache* global_cache);

    static const VTX::StructSchemaCache* ResolveStructCache(const VTX::PropertyAddressCache* global_cache, int32_t entity_type_id);
    static const VTX::StructSchemaCache* ResolveStructCache(
        const VTX::PropertyAddressCache* global_cache,
        const std::string& struct_name);
    static std::string ResolveSchemaName(
        const VTX::StructSchemaCache* struct_cache,
        VTX::FieldType expected_type,
        VTX::FieldContainerType container_type,
        size_t index);
    static std::string ResolveSchemaName(
        const VTX::PropertyAddressCache* global_cache,
        const EntityPropertyNode& node);
    static std::string ResolveStructName(const VTX::StructSchemaCache* struct_cache);
    static std::string FormatVector(const VTX::Vector& v);
    static std::string FormatQuat(const VTX::Quat& q);
    static std::string FormatTransform(const VTX::Transform& t);
    static std::string FormatRange(const VTX::FloatRange& r);
    static std::string FormatFloat(float value);
    static std::string FormatDouble(double value);
    static std::string FormatRawVector(const VTX::Vector& v);
    static std::string FormatRawQuat(const VTX::Quat& q);
    static std::string FormatRawTransform(const VTX::Transform& t);
    static std::string FormatRawRange(const VTX::FloatRange& r);
    static std::string FormatRawFloat(float value);
    static std::string FormatRawDouble(double value);

private:
    static EntityPropertyCommand BuildActivateCommand(
        const EntityPropertyNode& node,
        const VTX::PropertyAddressCache* global_cache);
    static EntityPropertyCommand BuildSchemaFocusCommand(
        const EntityPropertyNode& node,
        const VTX::PropertyAddressCache* global_cache);
};

} // namespace VtxServices
