#pragma once

#include <string>
#include <vector>

#include "gui/gui_types.h"
#include "vtx/common/vtx_types.h"
#include "vtx/reader/core/vtx_reader_facade.h"

namespace VtxServices {

    enum class FileInfoTone {
        Normal,
        Highlight,
        Success,
        Disabled,
    };

    struct FileInfoRow {
        std::string label;
        std::string value;
        FileInfoTone tone = FileInfoTone::Normal;
    };

    struct FileInfoSection {
        std::string title;
        bool default_open = false;
        bool render_as_raw_text = false;
        std::string raw_text;
        std::vector<FileInfoRow> rows;
    };

    struct FileInfoViewModel {
        std::string file_path;
        std::vector<FileInfoSection> sections;
    };

    struct SummaryRow {
        std::string label;
        std::string value;
    };

    struct ChunkIndexRow {
        int chunk_index = 0;
        std::string frame_range;
        std::string file_offset;
        std::string chunk_size;
        int frames_per_chunk = 0;
    };

    struct TimelineEventRow {
        std::string game_time;
        std::string event_type;
        std::string label;
        std::string location;
        std::string entity_id;
    };

    struct FooterTableColumn {
        std::string label;
        float width = 0.0f;
        bool stretch = false;
        bool disabled = false;
        bool highlighted = false;
    };

    struct FooterTableCell {
        std::string text;
        bool disabled = false;
        bool highlighted = false;
    };

    struct FooterTableRow {
        std::vector<FooterTableCell> cells;
    };

    struct FooterTableViewModel {
        std::string empty_message;
        std::string count_label;
        std::vector<FooterTableColumn> columns;
        std::vector<FooterTableRow> rows;
    };

    struct ReplayTimeTabViewModel {
        std::string label;
        std::string value_column_label = "Value";
        std::vector<std::string> values;
    };

    struct ReplayTimeViewModel {
        std::vector<SummaryRow> summary_rows;
        std::vector<ReplayTimeTabViewModel> tabs;
    };

    class FooterSummaryService {
    public:
        static FileInfoViewModel BuildFileInfoViewModel(const std::string& file_path, float file_size_mb,
                                                        VTX::VtxFormat format, const VTX::FileHeader& header,
                                                        const VTX::FileFooter& footer);
        static std::string ResolvePayloadFormatLabel(VTX::VtxFormat format);
        static std::string FormatChunkSize(uint32_t chunk_size_bytes);
        static std::string FormatFileOffset(uint64_t file_offset);
        static std::string FormatLocation(const VTX::Vector& location);
        static std::string FormatGameTime(float game_time_seconds);

        static std::vector<SummaryRow> BuildReplayTimeSummaryRows(const VTX::ReplayTimeData& time_data);
        static std::vector<SummaryRow> BuildPropertySchemaSummaryRows(const VTX::PropertySchema& prop_schema);
        static std::vector<std::string> BuildReplayTimeTabLabels(const VTX::ReplayTimeData& time_data);
        static std::vector<SummaryRow> BuildFooterSummaryRows(const VTX::FileFooter& footer);
        static std::vector<ChunkIndexRow> BuildChunkRows(const std::vector<VTX::ChunkIndexEntry>& chunk_index);
        static std::vector<TimelineEventRow> BuildTimelineEventRows(const std::vector<VTX::TimelineEvent>& events);
        static ReplayTimeViewModel BuildReplayTimeViewModel(const VTX::ReplayTimeData& time_data,
                                                            TimeDisplayFormat time_display_format);
        static FooterTableViewModel BuildChunkTableViewModel(const std::vector<VTX::ChunkIndexEntry>& chunk_index);
        static FooterTableViewModel BuildTimelineEventsTableViewModel(const std::vector<VTX::TimelineEvent>& events);
    };

} // namespace VtxServices
