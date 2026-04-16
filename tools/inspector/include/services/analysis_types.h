#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "vtx/common/vtx_types.h"
#include "vtx/common/vtx_property_cache.h"

namespace VtxServices {

// -- Shared types --

struct FrameRange {
    int32_t start_frame = 0;
    int32_t end_frame = 0;
    uint64_t start_game_time = 0;
    uint64_t end_game_time = 0;
};

// -- EntityLifeTime --

struct EntityLifeTimeEntry {
    std::string unique_id;
    std::string type_name;
    int32_t type_id = -1;
    std::vector<FrameRange> ranges;
};

struct EntityLifeTimeResult {
    std::vector<EntityLifeTimeEntry> entries;
    bool is_complete = false;
    std::string error_message;
};

// -- UniqueProperties --

struct UniquePropertiesGroup {
    std::vector<std::string> property_values;
    std::vector<std::string> contributing_uids;
    int32_t count = 0;
};

struct UniquePropertiesResult {
    std::vector<std::string> property_names;
    std::vector<UniquePropertiesGroup> groups;
    bool is_complete = false;
    std::string error_message;
};

// -- TrackProperty --

struct PropertyValueSpan {
    std::string unique_id;
    std::string property_name;
    std::string value;
    FrameRange range;
};

struct TrackPropertyResult {
    std::vector<std::string> property_names;
    std::vector<PropertyValueSpan> spans;
    bool is_complete = false;
    std::string error_message;
};

// -- Utility: extract scalar property value as string --

std::string ExtractScalarAsString(
    const VTX::PropertyContainer& entity,
    const VTX::PropertyAddress& addr);

// -- CSV export --

bool ExportEntityLifeTimeCsv(const std::string& path, const EntityLifeTimeResult& result);
bool ExportUniquePropertiesCsv(const std::string& path, const UniquePropertiesResult& result);
bool ExportTrackPropertyCsv(const std::string& path, const TrackPropertyResult& result);

// -- Formatting helpers --

std::string FormatFrameRanges(const std::vector<FrameRange>& ranges);
std::string FormatGameTimeRanges(const std::vector<FrameRange>& ranges);
std::string FormatSingleFrameRange(const FrameRange& range);
std::string FormatSingleGameTimeRange(const FrameRange& range);

} // namespace VtxServices
