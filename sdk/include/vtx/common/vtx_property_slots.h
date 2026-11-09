#pragma once
// Typed slots of a PropertyContainer: where the schema put a field, and the one way to write it.
//
// A writer resolves each field's slot ONCE from its SchemaStruct (FindSlot) and then writes values through
// it (Set, Child, Assign, Resize) -- never by naming a PropertyContainer member or an index itself. How a
// container stores a field's value is this header's business, so the SDK can change that storage and every
// writer that goes through slots follows without a change of its own.
//
//   ScalarSlot<T>      one value: bool, int32_t, int64_t, float, double, std::string, Vector, Quat, Transform,
//                      FloatRange.
//   StructSlot         one nested struct: Child(container, slot) is its container, to fill in place.
//   ArraySlot<T>       an array of T (the scalar types above; bool arrays hold uint8_t).
//   StructArraySlot    an array of nested structs: Resize(container, slot, n) is its n containers, in place.
//
// A slot is valid for containers of the struct it was found in, prepared for that struct (PreparePropertyContainer,
// ResetToPrepared or PrepareForOverwrite).

#include "vtx/common/readers/schema_reader/game_schema_types.h"
#include "vtx/common/vtx_types.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace VTX {

    namespace slot_detail {
        template <typename T>
        struct Storage;

#define VTX_SLOT_STORAGE(TYPE, FIELD_TYPE, SCALARS, ARRAYS, ELEMENT)                                                   \
    template <>                                                                                                        \
    struct Storage<TYPE> {                                                                                             \
        static constexpr FieldType kType = FIELD_TYPE;                                                                 \
        using Element = ELEMENT;                                                                                       \
        static auto& Scalars(PropertyContainer& c) { return c.SCALARS; }                                               \
        static auto& Arrays(PropertyContainer& c) { return c.ARRAYS; }                                                 \
    };

        VTX_SLOT_STORAGE(bool, FieldType::Bool, bool_properties, bool_arrays, uint8_t)
        VTX_SLOT_STORAGE(int32_t, FieldType::Int32, int32_properties, int32_arrays, int32_t)
        VTX_SLOT_STORAGE(int64_t, FieldType::Int64, int64_properties, int64_arrays, int64_t)
        VTX_SLOT_STORAGE(float, FieldType::Float, float_properties, float_arrays, float)
        VTX_SLOT_STORAGE(double, FieldType::Double, double_properties, double_arrays, double)
        VTX_SLOT_STORAGE(std::string, FieldType::String, string_properties, string_arrays, std::string)
        VTX_SLOT_STORAGE(Vector, FieldType::Vector, vector_properties, vector_arrays, Vector)
        VTX_SLOT_STORAGE(Quat, FieldType::Quat, quat_properties, quat_arrays, Quat)
        VTX_SLOT_STORAGE(Transform, FieldType::Transform, transform_properties, transform_arrays, Transform)
        VTX_SLOT_STORAGE(FloatRange, FieldType::FloatRange, range_properties, range_arrays, FloatRange)
#undef VTX_SLOT_STORAGE

        inline const SchemaField* Field(const SchemaStruct& schema, std::string_view name) {
            for (const SchemaField& field : schema.fields) {
                if (field.name == name)
                    return &field;
            }
            return nullptr;
        }
    } // namespace slot_detail

    template <typename T>
    struct ScalarSlot {
        int32_t index = -1;
    };
    struct StructSlot {
        int32_t index = -1;
    };
    template <typename T>
    struct ArraySlot {
        int32_t index = -1;
    };
    struct StructArraySlot {
        int32_t index = -1;
    };

    // ---- Resolving: the slot of a field, or none when the field is not of that shape ----------------------------

    template <typename T>
    std::optional<ScalarSlot<T>> ScalarSlotOf(const SchemaField& field) {
        if (field.type_id != slot_detail::Storage<T>::kType || field.container_type != FieldContainerType::None ||
            field.index < 0)
            return std::nullopt;
        return ScalarSlot<T> {field.index};
    }
    inline std::optional<StructSlot> StructSlotOf(const SchemaField& field) {
        if (field.type_id != FieldType::Struct || field.container_type != FieldContainerType::None || field.index < 0)
            return std::nullopt;
        return StructSlot {field.index};
    }
    template <typename T>
    std::optional<ArraySlot<T>> ArraySlotOf(const SchemaField& field) {
        if (field.type_id != slot_detail::Storage<T>::kType || field.container_type != FieldContainerType::Array ||
            field.index < 0)
            return std::nullopt;
        return ArraySlot<T> {field.index};
    }
    inline std::optional<StructArraySlot> StructArraySlotOf(const SchemaField& field) {
        if (field.type_id != FieldType::Struct || field.container_type != FieldContainerType::Array || field.index < 0)
            return std::nullopt;
        return StructArraySlot {field.index};
    }

    /// The field named @p name of @p schema, or null.
    inline const SchemaField* FindField(const SchemaStruct& schema, std::string_view name) {
        return slot_detail::Field(schema, name);
    }

    // ---- Writing ------------------------------------------------------------------------------------------------

    template <typename T, typename V>
    void Set(PropertyContainer& container, ScalarSlot<T> slot, V&& value) {
        auto& values = slot_detail::Storage<T>::Scalars(container);
        values[static_cast<size_t>(slot.index)] = std::forward<V>(value);
    }

    inline PropertyContainer& Child(PropertyContainer& container, StructSlot slot) {
        return container.any_struct_properties[static_cast<size_t>(slot.index)];
    }

    template <typename T>
    void Assign(PropertyContainer& container, ArraySlot<T> slot,
                std::span<const typename slot_detail::Storage<T>::Element> values) {
        slot_detail::Storage<T>::Arrays(container).ReplaceSubArray(static_cast<size_t>(slot.index), values);
    }

    /// The array slot's elements, made @p count long in place, for the caller to fill (ArraySlot of a scalar).
    template <typename T>
    std::span<typename slot_detail::Storage<T>::Element> Resize(PropertyContainer& container, ArraySlot<T> slot,
                                                                size_t count) {
        return slot_detail::Storage<T>::Arrays(container).ResizeSubArray(static_cast<size_t>(slot.index), count);
    }

    /// The struct array's @p count element containers, in place: each is then prepared for its struct and filled.
    inline std::span<PropertyContainer> Resize(PropertyContainer& container, StructArraySlot slot, size_t count) {
        return container.any_struct_arrays.ResizeSubArray(static_cast<size_t>(slot.index), count);
    }

} // namespace VTX
