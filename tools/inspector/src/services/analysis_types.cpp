#include "services/analysis_types.h"

#include <format>
#include <fstream>
#include <sstream>

#include "services/time_display_service.h"

namespace VtxServices {

    namespace {

        constexpr int64_t kTicksPerSecond = 10'000'000;

        // Converts game time ticks to seconds string with 2 decimal places.
        std::string GameTimeToSeconds(uint64_t ticks) {
            double seconds = static_cast<double>(ticks) / static_cast<double>(kTicksPerSecond);
            return std::format("{:.2f}s", seconds);
        }

        // Escapes a CSV field: wraps in quotes if it contains commas, quotes, or newlines.
        std::string CsvEscape(const std::string& field) {
            if (field.find_first_of(",\"\n") == std::string::npos) {
                return field;
            }
            std::string escaped = "\"";
            for (char c : field) {
                if (c == '"')
                    escaped += "\"\"";
                else
                    escaped += c;
            }
            escaped += '"';
            return escaped;
        }

    } // namespace

    // -- Property value extraction --

    std::string ExtractScalarAsString(const VTX::PropertyContainer& entity, const VTX::PropertyAddress& addr) {
        if (!addr.IsValid() || addr.container_type != VTX::FieldContainerType::None) {
            return "";
        }

        auto idx = static_cast<size_t>(addr.index);

        switch (addr.type_id) {
        case VTX::FieldType::Bool:
            if (idx >= entity.bool_properties.size())
                return "";
            return entity.bool_properties[idx] ? "true" : "false";
        case VTX::FieldType::Int32:
        case VTX::FieldType::Enum:
            if (idx >= entity.int32_properties.size())
                return "";
            return std::to_string(entity.int32_properties[idx]);
        case VTX::FieldType::Int64:
            if (idx >= entity.int64_properties.size())
                return "";
            return std::to_string(entity.int64_properties[idx]);
        case VTX::FieldType::Float:
            if (idx >= entity.float_properties.size())
                return "";
            return std::format("{:.6g}", entity.float_properties[idx]);
        case VTX::FieldType::Double:
            if (idx >= entity.double_properties.size())
                return "";
            return std::format("{:.6g}", entity.double_properties[idx]);
        case VTX::FieldType::String:
            if (idx >= entity.string_properties.size())
                return "";
            return entity.string_properties[idx];
        case VTX::FieldType::Vector:
            if (idx >= entity.vector_properties.size())
                return "";
            return std::format("({:.2f},{:.2f},{:.2f})", entity.vector_properties[idx].x,
                               entity.vector_properties[idx].y, entity.vector_properties[idx].z);
        case VTX::FieldType::Quat:
            if (idx >= entity.quat_properties.size())
                return "";
            return std::format("({:.4f},{:.4f},{:.4f},{:.4f})", entity.quat_properties[idx].x,
                               entity.quat_properties[idx].y, entity.quat_properties[idx].z,
                               entity.quat_properties[idx].w);
        case VTX::FieldType::Transform:
            if (idx >= entity.transform_properties.size())
                return "";
            return std::format("T({:.2f},{:.2f},{:.2f})", entity.transform_properties[idx].translation.x,
                               entity.transform_properties[idx].translation.y,
                               entity.transform_properties[idx].translation.z);
        case VTX::FieldType::FloatRange:
            if (idx >= entity.range_properties.size())
                return "";
            return std::format("[{:.2f}-{:.2f}]", entity.range_properties[idx].min, entity.range_properties[idx].max);
        default:
            return "";
        }
    }

    // -- Formatting helpers --

    std::string FormatSingleFrameRange(const FrameRange& range) {
        return std::format("[{}-{}]", range.start_frame, range.end_frame);
    }

    std::string FormatSingleGameTimeRange(const FrameRange& range) {
        return std::format("[{}-{}]", GameTimeToSeconds(range.start_game_time), GameTimeToSeconds(range.end_game_time));
    }

    std::string FormatFrameRanges(const std::vector<FrameRange>& ranges) {
        std::string result;
        for (size_t i = 0; i < ranges.size(); ++i) {
            if (i > 0)
                result += ", ";
            result += FormatSingleFrameRange(ranges[i]);
        }
        return result;
    }

    std::string FormatGameTimeRanges(const std::vector<FrameRange>& ranges) {
        std::string result;
        for (size_t i = 0; i < ranges.size(); ++i) {
            if (i > 0)
                result += ", ";
            result += FormatSingleGameTimeRange(ranges[i]);
        }
        return result;
    }

    // -- CSV export --

    bool ExportEntityLifeTimeCsv(const std::string& path, const EntityLifeTimeResult& result) {
        std::ofstream file(path);
        if (!file.is_open())
            return false;

        file << "UniqueId,TypeName,TypeId,RangesFrame,RangesGameTime\n";

        for (const auto& entry : result.entries) {
            file << CsvEscape(entry.unique_id) << "," << CsvEscape(entry.type_name) << "," << entry.type_id << ","
                 << CsvEscape(FormatFrameRanges(entry.ranges)) << "," << CsvEscape(FormatGameTimeRanges(entry.ranges))
                 << "\n";
        }

        return file.good();
    }

    bool ExportUniquePropertiesCsv(const std::string& path, const UniquePropertiesResult& result) {
        std::ofstream file(path);
        if (!file.is_open())
            return false;

        // Header: Prop1,Prop2,...,Count,UniqueIds
        for (const auto& name : result.property_names) {
            file << CsvEscape(name) << ",";
        }
        file << "Count,UniqueIds\n";

        for (const auto& group : result.groups) {
            for (const auto& val : group.property_values) {
                file << CsvEscape(val) << ",";
            }
            file << group.count << ",";

            // Semicolon-separated UIDs
            std::string uids;
            for (size_t i = 0; i < group.contributing_uids.size(); ++i) {
                if (i > 0)
                    uids += ";";
                uids += group.contributing_uids[i];
            }
            file << CsvEscape(uids) << "\n";
        }

        return file.good();
    }

    bool ExportTrackPropertyCsv(const std::string& path, const TrackPropertyResult& result) {
        std::ofstream file(path);
        if (!file.is_open())
            return false;

        file << "UniqueId,Property,Value,RangeFrame,RangeGameTime\n";

        for (const auto& span : result.spans) {
            file << CsvEscape(span.unique_id) << "," << CsvEscape(span.property_name) << "," << CsvEscape(span.value)
                 << "," << CsvEscape(FormatSingleFrameRange(span.range)) << ","
                 << CsvEscape(FormatSingleGameTimeRange(span.range)) << "\n";
        }

        return file.good();
    }

} // namespace VtxServices
