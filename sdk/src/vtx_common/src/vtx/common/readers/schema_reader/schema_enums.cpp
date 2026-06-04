/**
 * @file schema_enums.cpp
 * @brief Implementation of the schema enum string parsing helpers.
 * @author Zenos Interactive
 */
#include "vtx/common/readers/schema_reader/schema_enums.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <string>
#include <unordered_map>

namespace VTX {

    namespace {

        std::string ToLower(std::string_view raw) {
            std::string lowered(raw);
            std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return lowered;
        }

        bool ParsesAsWholeInteger(std::string_view text) {
            long long value = 0;
            const char* begin = text.data();
            const char* end = text.data() + text.size();
            auto [ptr, ec] = std::from_chars(begin, end, value);
            return ec == std::errc {} && ptr == end;
        }

        bool ParsesAsWholeReal(std::string_view text) {
            const std::string buffer(text);
            const char* begin = buffer.c_str();
            char* end = nullptr;
            (void)std::strtod(begin, &end);
            return end == begin + buffer.size() && end != begin;
        }

    } // namespace

    std::optional<FieldType> ParseFieldType(std::string_view raw) {
        // Keyed by the lowercased token so casing never matters.
        static const std::unordered_map<std::string, FieldType> kMap = {
            {"int8", FieldType::Int8},           {"uint8", FieldType::Int8},
            {"int32", FieldType::Int32},         {"uint32", FieldType::Int32},
            {"int", FieldType::Int32},           {"int64", FieldType::Int64},
            {"uint64", FieldType::Int64},        {"long", FieldType::Int64},
            {"float", FieldType::Float},         {"double", FieldType::Double},
            {"bool", FieldType::Bool},           {"string", FieldType::String},
            {"vector", FieldType::Vector},       {"quat", FieldType::Quat},
            {"transform", FieldType::Transform}, {"floatrange", FieldType::FloatRange},
            {"range", FieldType::FloatRange},    {"struct", FieldType::Struct},
            {"enum", FieldType::Enum},           {"none", FieldType::None},
        };

        auto it = kMap.find(ToLower(raw));
        if (it == kMap.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    std::optional<FieldContainerType> ParseContainerType(std::string_view raw) {
        const std::string lowered = ToLower(raw);
        if (lowered.empty() || lowered == "none") {
            return FieldContainerType::None;
        }
        if (lowered == "array") {
            return FieldContainerType::Array;
        }
        if (lowered == "map") {
            return FieldContainerType::Map;
        }
        return std::nullopt;
    }

    std::string_view ToString(FieldType type) {
        switch (type) {
        case FieldType::None:
            return "None";
        case FieldType::Int8:
            return "Int8";
        case FieldType::Int32:
            return "Int32";
        case FieldType::Int64:
            return "Int64";
        case FieldType::Float:
            return "Float";
        case FieldType::Double:
            return "Double";
        case FieldType::Bool:
            return "Bool";
        case FieldType::String:
            return "String";
        case FieldType::Vector:
            return "Vector";
        case FieldType::Quat:
            return "Quat";
        case FieldType::Transform:
            return "Transform";
        case FieldType::FloatRange:
            return "FloatRange";
        case FieldType::Struct:
            return "Struct";
        case FieldType::Enum:
            return "Enum";
        }
        return "Unknown";
    }

    std::string_view ToString(FieldContainerType container) {
        switch (container) {
        case FieldContainerType::None:
            return "None";
        case FieldContainerType::Array:
            return "Array";
        case FieldContainerType::Map:
            return "Map";
        }
        return "Unknown";
    }

    bool IsMapCompatibleKeyType(FieldType key) {
        switch (key) {
        case FieldType::String:
        case FieldType::Int8:
        case FieldType::Int32:
        case FieldType::Int64:
        case FieldType::Enum:
            return true;
        default:
            return false;
        }
    }

    bool IsValidDefaultValue(FieldType type, std::string_view default_value) {
        if (default_value.empty()) {
            return true; // "no explicit default" is always acceptable.
        }

        switch (type) {
        case FieldType::Int8:
        case FieldType::Int32:
        case FieldType::Int64:
        case FieldType::Enum:
            return ParsesAsWholeInteger(default_value);
        case FieldType::Float:
        case FieldType::Double:
            return ParsesAsWholeReal(default_value);
        case FieldType::Bool: {
            const std::string lowered = ToLower(default_value);
            return lowered == "true" || lowered == "false" || lowered == "1" || lowered == "0";
        }
        case FieldType::String:
            return true;
        case FieldType::None:
        case FieldType::Vector:
        case FieldType::Quat:
        case FieldType::Transform:
        case FieldType::FloatRange:
        case FieldType::Struct:
        default:
            // Composite / structural types carry no scalar string default.
            return false;
        }
    }

} // namespace VTX
