#pragma once
//
// FieldType display utilities — maps SDK enum values to strings/colors.
// SDK-candidate: FieldTypeToString should migrate to vtx_common once stable.
// FieldTypeColor stays GUI-side (depends on ImVec4).
//

#include "vtx/common/readers/schema_reader/game_schema_types.h"

namespace VtxServices {

    /// Convert a VTX::FieldType enum to a human-readable string.
    /// Pure logic, no GUI dependency — first candidate for SDK migration.
    const char* FieldTypeToString(VTX::FieldType type_id);

} // namespace VtxServices
