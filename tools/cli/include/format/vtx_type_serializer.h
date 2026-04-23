#pragma once

/// Public API  (what commands should call):
///   Serialize(w, Vector)
///   Serialize(w, Quat)
///   Serialize(w, Transform)
///   Serialize(w, FloatRange)
///   Serialize(w, MapContainer,        cache)
///   Serialize(w, PropertyContainer,   cache)
///   SerializeProperty(w, PropertyContainer, property_name, cache)   // single property by name

#include "core/cli_concepts.h"

#include "vtx/common/vtx_property_cache.h"

#include <string>
#include <cstdint>

namespace VtxCli {

    template <FormatWriter Fmt>
    void Serialize(Fmt& w, const VTX::PropertyContainer& pc, const VTX::PropertyAddressCache& cache);

    template <FormatWriter Fmt>
    void Serialize(Fmt& w, const VTX::Vector& v) {
        w.BeginObject().Key("x").WriteDouble(v.x).Key("y").WriteDouble(v.y).Key("z").WriteDouble(v.z).EndObject();
    }

    template <FormatWriter Fmt>
    void Serialize(Fmt& w, const VTX::Quat& q) {
        w.BeginObject()
            .Key("x")
            .WriteFloat(q.x)
            .Key("y")
            .WriteFloat(q.y)
            .Key("z")
            .WriteFloat(q.z)
            .Key("w")
            .WriteFloat(q.w)
            .EndObject();
    }

    template <FormatWriter Fmt>
    void Serialize(Fmt& w, const VTX::Transform& t) {
        w.BeginObject();
        w.Key("translation");
        Serialize(w, t.translation);
        w.Key("rotation");
        Serialize(w, t.rotation);
        w.Key("scale");
        Serialize(w, t.scale);
        w.EndObject();
    }

    template <FormatWriter Fmt>
    void Serialize(Fmt& w, const VTX::FloatRange& r) {
        w.BeginObject()
            .Key("min")
            .WriteFloat(r.min)
            .Key("max")
            .WriteFloat(r.max)
            .Key("value_normalized")
            .WriteFloat(r.value_normalized)
            .EndObject();
    }

    template <FormatWriter Fmt>
    void Serialize(Fmt& w, const VTX::MapContainer& m, const VTX::PropertyAddressCache& cache) {
        w.BeginObject();
        const size_t count = std::min(m.keys.size(), m.values.size());
        for (size_t i = 0; i < count; ++i) {
            w.Key(m.keys[i]);
            Serialize(w, m.values[i], cache);
        }
        w.EndObject();
    }


    namespace detail {

        /// Bounds-checked read. Returns true if index is within the container.
        template <typename Container>
        inline bool InBounds(const Container& c, size_t idx) {
            return idx < c.size();
        }

        template <FormatWriter Fmt>
        void WriteScalarValue(Fmt& w, const VTX::PropertyContainer& pc, const VTX::PropertyAddress& addr,
                              const VTX::PropertyAddressCache& cache) {
            const auto idx = static_cast<size_t>(addr.index);
            switch (addr.type_id) {
            case VTX::FieldType::Bool:
                // std::vector<bool> returns proxy — cast explicitly
                if (InBounds(pc.bool_properties, idx)) {
                    w.WriteBool(static_cast<bool>(pc.bool_properties[idx]));
                } else {
                    w.WriteNull();
                }
                break;

            case VTX::FieldType::Int32:
            case VTX::FieldType::Enum: // enums stored as int32
                if (InBounds(pc.int32_properties, idx)) {
                    w.WriteInt(pc.int32_properties[idx]);
                } else {
                    w.WriteNull();
                }
                break;

            case VTX::FieldType::Int64:
                if (InBounds(pc.int64_properties, idx)) {
                    w.WriteInt64(pc.int64_properties[idx]);
                } else {
                    w.WriteNull();
                }
                break;

            case VTX::FieldType::Float:
                if (InBounds(pc.float_properties, idx)) {
                    w.WriteFloat(pc.float_properties[idx]);
                } else {
                    w.WriteNull();
                }
                break;

            case VTX::FieldType::Double:
                if (InBounds(pc.double_properties, idx)) {
                    w.WriteDouble(pc.double_properties[idx]);
                } else {
                    w.WriteNull();
                }
                break;

            case VTX::FieldType::String:
                if (InBounds(pc.string_properties, idx)) {
                    w.WriteString(pc.string_properties[idx]);
                } else {
                    w.WriteNull();
                }
                break;

            case VTX::FieldType::Vector:
                if (InBounds(pc.vector_properties, idx)) {
                    Serialize(w, pc.vector_properties[idx]);
                } else {
                    w.WriteNull();
                }
                break;

            case VTX::FieldType::Quat:
                if (InBounds(pc.quat_properties, idx)) {
                    Serialize(w, pc.quat_properties[idx]);
                } else {
                    w.WriteNull();
                }
                break;

            case VTX::FieldType::Transform:
                if (InBounds(pc.transform_properties, idx)) {
                    Serialize(w, pc.transform_properties[idx]);
                } else {
                    w.WriteNull();
                }
                break;

            case VTX::FieldType::FloatRange:
                if (InBounds(pc.range_properties, idx)) {
                    Serialize(w, pc.range_properties[idx]);
                } else {
                    w.WriteNull();
                }
                break;

            case VTX::FieldType::Struct:
                if (InBounds(pc.any_struct_properties, idx)) {
                    Serialize(w, pc.any_struct_properties[idx], cache); // recursion
                } else {
                    w.WriteNull();
                }
                break;

            default:
                w.WriteNull();
                break;
            }
        }


        template <FormatWriter Fmt>
        void WriteArrayValue(Fmt& w, const VTX::PropertyContainer& pc, const VTX::PropertyAddress& addr,
                             const VTX::PropertyAddressCache& cache) {
            const auto idx = static_cast<size_t>(addr.index);

            switch (addr.type_id) {
            case VTX::FieldType::Bool: {
                auto span = pc.bool_arrays.GetSubArray(idx);
                w.BeginArray();
                for (auto v : span) {
                    w.WriteBool(v != 0);
                }
                w.EndArray();
                break;
            }

            case VTX::FieldType::Int32:
            case VTX::FieldType::Enum: {
                auto span = pc.int32_arrays.GetSubArray(idx);
                w.BeginArray();
                for (auto v : span) {
                    w.WriteInt(v);
                }
                w.EndArray();
                break;
            }

            case VTX::FieldType::Int64: {
                auto span = pc.int64_arrays.GetSubArray(idx);
                w.BeginArray();
                for (auto v : span) {
                    w.WriteInt64(v);
                }
                w.EndArray();
                break;
            }

            case VTX::FieldType::Float: {
                auto span = pc.float_arrays.GetSubArray(idx);
                w.BeginArray();
                for (auto v : span) {
                    w.WriteFloat(v);
                }
                w.EndArray();
                break;
            }

            case VTX::FieldType::Double: {
                auto span = pc.double_arrays.GetSubArray(idx);
                w.BeginArray();
                for (auto v : span) {
                    w.WriteDouble(v);
                }
                w.EndArray();
                break;
            }

            case VTX::FieldType::String: {
                auto span = pc.string_arrays.GetSubArray(idx);
                w.BeginArray();
                for (const auto& v : span) {
                    w.WriteString(v);
                }
                w.EndArray();
                break;
            }

            case VTX::FieldType::Vector: {
                auto span = pc.vector_arrays.GetSubArray(idx);
                w.BeginArray();
                for (const auto& v : span) {
                    Serialize(w, v);
                }
                w.EndArray();
                break;
            }

            case VTX::FieldType::Quat: {
                auto span = pc.quat_arrays.GetSubArray(idx);
                w.BeginArray();
                for (const auto& v : span) {
                    Serialize(w, v);
                }
                w.EndArray();
                break;
            }

            case VTX::FieldType::Transform: {
                auto span = pc.transform_arrays.GetSubArray(idx);
                w.BeginArray();
                for (const auto& v : span) {
                    Serialize(w, v);
                }
                w.EndArray();
                break;
            }

            case VTX::FieldType::FloatRange: {
                auto span = pc.range_arrays.GetSubArray(idx);
                w.BeginArray();
                for (const auto& v : span) {
                    Serialize(w, v);
                }
                w.EndArray();
                break;
            }

            case VTX::FieldType::Struct: {
                auto span = pc.any_struct_arrays.GetSubArray(idx);
                w.BeginArray();
                for (const auto& v : span) {
                    Serialize(w, v, cache);
                }
                w.EndArray();
                break;
            }

            default:
                w.BeginArray().EndArray(); // empty array for unknown types
                break;
                break;
            }
        }


        template <FormatWriter Fmt>
        void WriteMapValue(Fmt& w, const VTX::PropertyContainer& pc, const VTX::PropertyAddress& addr,
                           const VTX::PropertyAddressCache& cache) {
            const auto idx = static_cast<size_t>(addr.index);
            if (InBounds(pc.map_properties, idx)) {
                Serialize(w, pc.map_properties[idx], cache);
            } else {
                w.WriteNull();
            }
        }

        template <FormatWriter Fmt>
        void WritePropertyValue(Fmt& w, const VTX::PropertyContainer& pc, const VTX::PropertyAddress& addr,
                                const VTX::PropertyAddressCache& cache) {
            switch (addr.container_type) {
            case VTX::FieldContainerType::Array:
                WriteArrayValue(w, pc, addr, cache);
                break;
            case VTX::FieldContainerType::Map:
                WriteMapValue(w, pc, addr, cache);
                break;
            default:
                WriteScalarValue(w, pc, addr, cache);
                break;
            }
        }


        template <FormatWriter Fmt>
        void SerializeRawPropertyContainer(Fmt& w, const VTX::PropertyContainer& pc) {
            for (size_t i = 0; i < pc.bool_properties.size(); ++i) {
                w.Key("bool_" + std::to_string(i)).WriteBool(static_cast<bool>(pc.bool_properties[i]));
            }
            for (size_t i = 0; i < pc.int32_properties.size(); ++i) {
                w.Key("int32_" + std::to_string(i)).WriteInt(pc.int32_properties[i]);
            }
            for (size_t i = 0; i < pc.int64_properties.size(); ++i) {
                w.Key("int64_" + std::to_string(i)).WriteInt64(pc.int64_properties[i]);
            }
            for (size_t i = 0; i < pc.float_properties.size(); ++i) {
                w.Key("float_" + std::to_string(i)).WriteFloat(pc.float_properties[i]);
            }
            for (size_t i = 0; i < pc.double_properties.size(); ++i) {
                w.Key("double_" + std::to_string(i)).WriteDouble(pc.double_properties[i]);
            }
            for (size_t i = 0; i < pc.string_properties.size(); ++i) {
                w.Key("string_" + std::to_string(i)).WriteString(pc.string_properties[i]);
            }

            for (size_t i = 0; i < pc.vector_properties.size(); ++i) {
                w.Key("vector_" + std::to_string(i));
                Serialize(w, pc.vector_properties[i]);
            }
            for (size_t i = 0; i < pc.quat_properties.size(); ++i) {
                w.Key("quat_" + std::to_string(i));
                Serialize(w, pc.quat_properties[i]);
            }
            for (size_t i = 0; i < pc.transform_properties.size(); ++i) {
                w.Key("transform_" + std::to_string(i));
                Serialize(w, pc.transform_properties[i]);
            }
            for (size_t i = 0; i < pc.range_properties.size(); ++i) {
                w.Key("range_" + std::to_string(i));
                Serialize(w, pc.range_properties[i]);
            }
        }

    } // namespace detail


    template <FormatWriter Fmt>
    void Serialize(Fmt& w, const VTX::PropertyContainer& pc, const VTX::PropertyAddressCache& cache) {
        w.BeginObject();

        w.Key("entity_type_id").WriteInt(pc.entity_type_id);
        w.Key("content_hash").WriteUInt64(pc.content_hash);

        auto it = cache.structs.find(pc.entity_type_id);

        if (it != cache.structs.end()) {
            const VTX::StructSchemaCache& struct_cache = it->second;

            w.Key("type_name").WriteString(struct_cache.name);
            w.Key("properties");
            w.BeginObject();

            for (const auto& prop_view : struct_cache.GetPropertiesInOrder()) {
                w.Key(std::string(prop_view.name));

                if (prop_view.address && prop_view.address->IsValid()) {
                    detail::WritePropertyValue(w, pc, *prop_view.address, cache);
                } else {
                    w.WriteNull();
                }
            }

            w.EndObject(); // /properties
        } else {
            w.Key("type_name").WriteNull();
            w.Key("properties");
            w.BeginObject();
            detail::SerializeRawPropertyContainer(w, pc);
            w.EndObject(); // /properties
        }

        w.EndObject();
    }


    template <FormatWriter Fmt>
    bool SerializeProperty(Fmt& w, const VTX::PropertyContainer& pc, const std::string& property_name,
                           const VTX::PropertyAddressCache& cache) {
        auto struct_it = cache.structs.find(pc.entity_type_id);
        if (struct_it == cache.structs.end()) {
            return false;
        }

        const VTX::StructSchemaCache& struct_cache = struct_it->second;
        auto prop_it = struct_cache.properties.find(property_name);
        if (prop_it == struct_cache.properties.end()) {
            return false;
        }

        const VTX::PropertyAddress& addr = prop_it->second;
        if (!addr.IsValid()) {
            w.WriteNull();
            return true;
        }

        detail::WritePropertyValue(w, pc, addr, cache);
        return true;
    }

} // namespace VtxCli
