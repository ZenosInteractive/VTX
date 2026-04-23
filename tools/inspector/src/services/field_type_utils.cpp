#include "services/field_type_utils.h"

namespace VtxServices {

    const char* FieldTypeToString(VTX::FieldType type_id) {
        switch (type_id) {
        case VTX::FieldType::None:
            return "None";
        case VTX::FieldType::Int8:
            return "Int8";
        case VTX::FieldType::Int32:
            return "Int32";
        case VTX::FieldType::Int64:
            return "Int64";
        case VTX::FieldType::Float:
            return "Float";
        case VTX::FieldType::Double:
            return "Double";
        case VTX::FieldType::Bool:
            return "Bool";
        case VTX::FieldType::String:
            return "String";
        case VTX::FieldType::Vector:
            return "Vector";
        case VTX::FieldType::Quat:
            return "Quat";
        case VTX::FieldType::Transform:
            return "Transform";
        case VTX::FieldType::FloatRange:
            return "FloatRange";
        case VTX::FieldType::Struct:
            return "Struct";
        case VTX::FieldType::Enum:
            return "Enum";
        default:
            return "Unknown";
        }
    }

} // namespace VtxServices
