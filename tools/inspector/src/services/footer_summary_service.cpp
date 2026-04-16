#include "services/footer_summary_service.h"

#include <algorithm>
#include <cstdio>
#include <string>

#include "services/time_display_service.h"
namespace {
// Formats replay duration using compact h/m/s/ms units plus total seconds.
std::string FormatDurationLabel(float duration_seconds) {
    const float clamped_seconds = std::max(0.0f, duration_seconds);
    const int64_t total_milliseconds = static_cast<int64_t>(clamped_seconds * 1000.0f + 0.5f);
    const int64_t hours = total_milliseconds / 3600000;
    const int64_t minutes = (total_milliseconds % 3600000) / 60000;
    const int64_t seconds = (total_milliseconds % 60000) / 1000;
    const int64_t milliseconds = total_milliseconds % 1000;

    char units_buffer[128];
    if (hours > 0) {
        std::snprintf(
            units_buffer,
            sizeof(units_buffer),
            "%lldh %lldm %llds %lldms",
            static_cast<long long>(hours),
            static_cast<long long>(minutes),
            static_cast<long long>(seconds),
            static_cast<long long>(milliseconds));
    } else if (minutes > 0) {
        std::snprintf(
            units_buffer,
            sizeof(units_buffer),
            "%lldm %llds %lldms",
            static_cast<long long>(minutes),
            static_cast<long long>(seconds),
            static_cast<long long>(milliseconds));
    } else {
        std::snprintf(
            units_buffer,
            sizeof(units_buffer),
            "%llds %lldms",
            static_cast<long long>(seconds),
            static_cast<long long>(milliseconds));
    }

    char final_buffer[160];
    std::snprintf(final_buffer, sizeof(final_buffer), "%s (%.1f sec)", units_buffer, clamped_seconds);
    return final_buffer;
}
} // namespace

namespace VtxServices {

FileInfoViewModel FooterSummaryService::BuildFileInfoViewModel(
    const std::string& file_path,
    float file_size_mb,
    VTX::VtxFormat format,
    const VTX::FileHeader& header,
    const VTX::FileFooter& footer) {
    FileInfoViewModel view_model;
    view_model.file_path = file_path;

    FileInfoSection header_section;
    header_section.title = "File Header";
    header_section.default_open = true;
    header_section.rows = {
        FileInfoRow{"Internal Format", ResolvePayloadFormatLabel(format), FileInfoTone::Success},
        FileInfoRow{"File Size", [&]() {
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "%.2f MB", file_size_mb);
            return std::string(buffer);
        }(), FileInfoTone::Highlight},
        FileInfoRow{"Replay Name", header.replay_name, FileInfoTone::Highlight},
        FileInfoRow{"UUID", header.replay_uuid, FileInfoTone::Disabled},
        FileInfoRow{
            "Recorded UTC",
            TimeDisplayService::FormatUnixSecondsAsRssWithRaw(header.recorded_utc_timestamp),
            FileInfoTone::Normal},
    };

    FileInfoSection version_section;
    version_section.title = "Version Info";
    version_section.default_open = true;
    version_section.rows = {
        FileInfoRow{"Format Version", "v" + std::to_string(header.version.format_major) + "." + std::to_string(header.version.format_minor)},
        FileInfoRow{"Schema Version", "v" + std::to_string(header.version.schema_version)},
    };

    FileInfoSection metadata_section;
    metadata_section.title = "Custom Metadata";
    metadata_section.render_as_raw_text = true;
    metadata_section.raw_text = header.custom_json_metadata;

    FileInfoSection footer_section;
    footer_section.title = "File Footer";
    footer_section.default_open = true;
    for (const auto& row : BuildFooterSummaryRows(footer)) {
        footer_section.rows.push_back(FileInfoRow{
            .label = row.label,
            .value = row.value,
            .tone = row.label == "Payload Checksum" ? FileInfoTone::Disabled : FileInfoTone::Normal,
        });
    }

    view_model.sections = {
        std::move(header_section),
        std::move(version_section),
        std::move(metadata_section),
        std::move(footer_section),
    };
    return view_model;
}

std::string FooterSummaryService::ResolvePayloadFormatLabel(VTX::VtxFormat format) {
    return format == VTX::VtxFormat::Protobuf ? "Protobuf" : "FlatBuffers";
}

std::string FooterSummaryService::FormatChunkSize(uint32_t chunk_size_bytes) {
    char buffer[64];
    if (chunk_size_bytes >= 1024 * 1024) {
        std::snprintf(buffer, sizeof(buffer), "%.2f MB", static_cast<float>(chunk_size_bytes) / (1024.0f * 1024.0f));
    } else if (chunk_size_bytes >= 1024) {
        std::snprintf(buffer, sizeof(buffer), "%.1f KB", static_cast<float>(chunk_size_bytes) / 1024.0f);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%u B", chunk_size_bytes);
    }
    return buffer;
}

std::string FooterSummaryService::FormatFileOffset(uint64_t file_offset) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "0x%llX", static_cast<unsigned long long>(file_offset));
    return buffer;
}

std::string FooterSummaryService::FormatLocation(const VTX::Vector& location) {
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "(%.0f, %.0f, %.0f)", location.x, location.y, location.z);
    return buffer;
}

std::string FooterSummaryService::FormatGameTime(float game_time_seconds) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.2fs", game_time_seconds);
    return buffer;
}

std::vector<SummaryRow> FooterSummaryService::BuildReplayTimeSummaryRows(const VTX::ReplayTimeData& time_data) {
    return {
        {"Game Timestamps", std::to_string(time_data.game_time.size())},
        {"UTC Timestamps", std::to_string(time_data.created_utc.size())},
        {"Gaps", std::to_string(time_data.gaps.size())},
        {"Segments", std::to_string(time_data.segments.size())},
    };
}

std::vector<SummaryRow> FooterSummaryService::BuildPropertySchemaSummaryRows(const VTX::PropertySchema& prop_schema) {
    return {
        {"Int Mappings", std::to_string(prop_schema.int_mapping.size())},
        {"Float Mappings", std::to_string(prop_schema.float_mapping.size())},
        {"Bool Mappings", std::to_string(prop_schema.bool_mapping.size())},
        {"String Mappings", std::to_string(prop_schema.string_mapping.size())},
        {"Vector Mappings", std::to_string(prop_schema.vector_mapping.size())},
        {"Transform Mappings", std::to_string(prop_schema.transform_mapping.size())},
    };
}

std::vector<std::string> FooterSummaryService::BuildReplayTimeTabLabels(const VTX::ReplayTimeData& time_data) {
    return {
        "Game Timestamps (" + std::to_string(time_data.game_time.size()) + ")###GameTimeTab",
        "UTC Timestamps (" + std::to_string(time_data.created_utc.size()) + ")###UTCTab",
        "Gaps (" + std::to_string(time_data.gaps.size()) + ")###GapsTab",
        "Segments (" + std::to_string(time_data.segments.size()) + ")###SegsTab",
    };
}

std::vector<SummaryRow> FooterSummaryService::BuildFooterSummaryRows(const VTX::FileFooter& footer) {
    const std::string duration_label = FormatDurationLabel(footer.duration_seconds);

    std::vector<SummaryRow> rows = {
        {"Total Frames", std::to_string(footer.total_frames)},
        {"Duration", duration_label},
        {"Data Chunks", std::to_string(footer.chunk_index.size())},
        {"Timeline Events", std::to_string(footer.events.size())},
    };

    if (footer.total_frames > 0 && footer.duration_seconds > 0.0f) {
        char fps_buffer[32];
        const float fps = static_cast<float>(footer.total_frames) / footer.duration_seconds;
        std::snprintf(fps_buffer, sizeof(fps_buffer), "%.1f", fps);
        rows.insert(rows.begin() + 2, SummaryRow{"Avg FPS", fps_buffer});
    }

    char checksum_buffer[64];
    std::snprintf(checksum_buffer, sizeof(checksum_buffer), "0x%llX", footer.payload_checksum);
    rows.push_back({"Payload Checksum", checksum_buffer});
    return rows;
}

// Builds chunk index rows for chunk metadata columns.
std::vector<ChunkIndexRow> FooterSummaryService::BuildChunkRows(
    const std::vector<VTX::ChunkIndexEntry>& chunk_index) {
    std::vector<ChunkIndexRow> rows;
    rows.reserve(chunk_index.size());
    for (const auto& c : chunk_index) {
        ChunkIndexRow row;
        row.chunk_index = c.chunk_index;

        char frame_range[64];
        std::snprintf(frame_range, sizeof(frame_range), "%d - %d", c.start_frame, c.end_frame);
        row.frame_range = frame_range;

        row.file_offset = FormatFileOffset(c.file_offset);
        row.chunk_size = FormatChunkSize(c.chunk_size_bytes);
        row.frames_per_chunk = c.end_frame - c.start_frame + 1;
        rows.push_back(std::move(row));
    }

    return rows;
}

std::vector<TimelineEventRow> FooterSummaryService::BuildTimelineEventRows(const std::vector<VTX::TimelineEvent>& events) {
    std::vector<TimelineEventRow> rows;
    rows.reserve(events.size());
    for (const auto& ev : events) {
        rows.push_back(TimelineEventRow{
            .game_time = FormatGameTime(ev.game_time),
            .event_type = ev.event_type,
            .label = ev.label,
            .location = FormatLocation(ev.location),
            .entity_id = ev.entity_unique_id,
        });
    }
    return rows;
}

// Builds replay-time tab values with synchronized tick/format display modes.
ReplayTimeViewModel FooterSummaryService::BuildReplayTimeViewModel(
    const VTX::ReplayTimeData& time_data,
    TimeDisplayFormat time_display_format) {
    ReplayTimeViewModel view_model;
    view_model.summary_rows = BuildReplayTimeSummaryRows(time_data);
    const auto labels = BuildReplayTimeTabLabels(time_data);

    auto append_tick_tab = [
        &view_model,
        time_display_format](
        const std::string& label,
        const std::string& value_column_label,
        const auto& values,
        const auto& formatter) {
        ReplayTimeTabViewModel tab;
        tab.label = label;
        tab.value_column_label = value_column_label;
        tab.values.reserve(values.size());
        for (const auto& value : values) {
            tab.values.push_back(formatter(static_cast<uint64_t>(value), time_display_format));
        }
        view_model.tabs.push_back(std::move(tab));
    };

    auto append_raw_tab = [&view_model](
        const std::string& label,
        const std::string& value_column_label,
        const auto& values) {
        ReplayTimeTabViewModel tab;
        tab.label = label;
        tab.value_column_label = value_column_label;
        tab.values.reserve(values.size());
        for (const auto& value : values) {
            tab.values.push_back(std::to_string(value));
        }
        view_model.tabs.push_back(std::move(tab));
    };

    append_tick_tab(labels[0], "Game Timestamps", time_data.game_time, TimeDisplayService::FormatGameTimeTicks);
    append_tick_tab(labels[1], "UTC Timestamps", time_data.created_utc, TimeDisplayService::FormatUtcTicks);
    append_raw_tab(labels[2], "Frame Index", time_data.gaps);
    append_raw_tab(labels[3], "Frame Index", time_data.segments);
    return view_model;
}

// Builds chunk index table presentation from chunk metadata.
FooterTableViewModel FooterSummaryService::BuildChunkTableViewModel(
    const std::vector<VTX::ChunkIndexEntry>& chunk_index) {
    FooterTableViewModel view_model;
    const auto rows = BuildChunkRows(chunk_index);
    view_model.empty_message = "No chunks.";
    view_model.count_label = std::to_string(rows.size()) + " chunks";
    view_model.columns = {
        FooterTableColumn{"Chunk", 50.0f, false, false, false},
        FooterTableColumn{"Frames", 100.0f, false, false, false},
        FooterTableColumn{"Offset", 100.0f, false, true, false},
        FooterTableColumn{"Size", 90.0f, false, false, false},
        FooterTableColumn{"Frames/Chunk", 90.0f, false, false, false},
    };
    view_model.rows.reserve(rows.size());
    for (const auto& row : rows) {
        FooterTableRow vm_row;
        vm_row.cells = {
            FooterTableCell{std::to_string(row.chunk_index)},
            FooterTableCell{row.frame_range},
            FooterTableCell{row.file_offset, true, false},
            FooterTableCell{row.chunk_size},
            FooterTableCell{std::to_string(row.frames_per_chunk)},
        };
        view_model.rows.push_back(std::move(vm_row));
    }
    return view_model;
}

FooterTableViewModel FooterSummaryService::BuildTimelineEventsTableViewModel(const std::vector<VTX::TimelineEvent>& events) {
    FooterTableViewModel view_model;
    const auto rows = BuildTimelineEventRows(events);
    view_model.empty_message = "No events.";
    view_model.count_label = std::to_string(rows.size()) + " events";
    view_model.columns = {
        FooterTableColumn{"Time", 70.0f, false, false, false},
        FooterTableColumn{"Type", 100.0f, false, false, true},
        FooterTableColumn{"Label", 0.0f, true, false, false},
        FooterTableColumn{"Location", 180.0f, false, true, false},
        FooterTableColumn{"Entity ID", 120.0f, false, true, false},
    };
    view_model.rows.reserve(rows.size());
    for (const auto& row : rows) {
        FooterTableRow vm_row;
        vm_row.cells = {
            FooterTableCell{row.game_time},
            FooterTableCell{row.event_type, false, true},
            FooterTableCell{row.label},
            FooterTableCell{row.location, true, false},
            FooterTableCell{row.entity_id, true, false},
        };
        view_model.rows.push_back(std::move(vm_row));
    }
    return view_model;
}

} // namespace VtxServices
