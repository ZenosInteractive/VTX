/**
 * @file schema_dynamic_loader.h
 * @brief Universal VTX loader.
 * @details System of Adapters to read from JSON, Protobuf o FlatBuffers
 * and populate SoA PropertyContainer.
 * Adapters are used mostly for formats like JSON where 
 */

#pragma once

#include <iostream>
#include <vector>
#include <string>

#include "vtx/common/vtx_types.h"
#include "vtx/common/readers/schema_reader/schema_registry.h"
#include "schema_sanitizer.h"
#include "vtx/common/vtx_types_helpers.h"

namespace VTX {
    
    class SchemaDynamicLoader {
    private:
        const SchemaRegistry* registry_;
        const SchemaSanitizerRegistry* sanitizer_ = nullptr;
        bool debug_mode_;

    public:
        explicit SchemaDynamicLoader(const SchemaRegistry& registry, 
                                    const SchemaSanitizerRegistry* sanitizer = nullptr, 
                                    bool debug_mode = false) 
            : registry_(&registry), sanitizer_(sanitizer), debug_mode_(debug_mode) {}


        template <typename AdapterType>
        void Load(const AdapterType& adapter, const std::string& structName, PropertyContainer& outContainer) {
            if (debug_mode_) std::cout << "[Loader] Loading struct: '" << structName << "'" << std::endl;
            
            if(!registry_)
            {
                if (debug_mode_) std::cerr << "[Loader][Error] Schema is null: " << std::endl;
                return;
            }
            
            const SchemaStruct* schema = registry_->GetStruct(structName);
            if (!schema) {
                if (debug_mode_) std::cerr << "[Loader][Error] Schema not found: " << structName << std::endl;
                return;
            }

            Helpers::PreparePropertyContainer(outContainer, *schema);

            for (const auto& field : schema->fields) {
                std::string key = field.json_name.empty() ? field.name : field.json_name;

                if (!adapter.HasKey(key)) {
                    continue;
                }

                auto child_adapter = adapter.GetChild(key);

                if (field.container_type == FieldContainerType::Array) {
                    LoadArray(child_adapter, field, outContainer);
                } 
                else if (field.type_id == FieldType::Struct) {
                    Load(child_adapter, field.struct_type, outContainer.any_struct_properties[field.index]);
                } 
                else {
                    LoadSingleField(child_adapter, field, outContainer);
                }
            }

            if (sanitizer_) {
                SanitizeContext ctx{structName,schema,true};// is_loading = true, we are in loading phase
                sanitizer_->TrySanitize(outContainer, ctx);
            }
            
            outContainer.content_hash = Helpers::CalculateContainerHash(outContainer);
        }

    private:

        template <typename AdapterType>
        void LoadSingleField(const AdapterType& node, const SchemaField& field, PropertyContainer& out) {
            const int32_t idx = field.index;
            try {
                switch (field.type_id) {
                    case FieldType::Int32:  out.int32_properties[idx] = node.template GetValue<int32_t>(); break;
                    case FieldType::Int64:  out.int64_properties[idx] = node.template GetValue<int64_t>(); break;
                    case FieldType::Float:  out.float_properties[idx] = node.template GetValue<float>(); break;
                    case FieldType::Double: out.double_properties[idx] = node.template GetValue<double>(); break;
                    case FieldType::Bool:   out.bool_properties[idx] = node.template GetValue<bool>(); break;
                    case FieldType::String: out.string_properties[idx] = node.template GetValue<std::string>(); break;

                    case FieldType::Vector: {
                        out.vector_properties[idx] = {
                            node.GetChild("x").template GetValue<double>(),
                            node.GetChild("y").template GetValue<double>(),
                            node.GetChild("z").template GetValue<double>()
                        };
                        break;
                    }
                    case FieldType::Quat: {
                        out.quat_properties[idx] = {
                            node.GetChild("x").template GetValue<float>(),
                            node.GetChild("y").template GetValue<float>(),
                            node.GetChild("z").template GetValue<float>(),
                            node.GetChild("w").template GetValue<float>()
                        };
                        break;
                    }
                    case FieldType::FloatRange: {
                        out.range_properties[idx] = {
                            node.GetChild("min").template GetValue<float>(),
                            node.GetChild("max").template GetValue<float>(),
                            node.GetChild("value").template GetValue<float>()
                        };
                        break;
                    }
                    case FieldType::Transform: {
                        Transform t;
                        auto loc = node.GetChild("translation");
                        t.translation = { loc.GetChild("x").template GetValue<double>(), loc.GetChild("y").template GetValue<double>(), loc.GetChild("z").template GetValue<double>() };
                        
                        auto rot = node.GetChild("rotation");
                        t.rotation = { rot.GetChild("x").template GetValue<float>(), rot.GetChild("y").template GetValue<float>(), rot.GetChild("z").template GetValue<float>(), rot.GetChild("w").template GetValue<float>() };
                        
                        auto scl = node.GetChild("scale");
                        t.scale = { scl.GetChild("x").template GetValue<double>(), scl.GetChild("y").template GetValue<double>(), scl.GetChild("z").template GetValue<double>() };
                        
                        out.transform_properties[idx] = t;
                        break;
                    }
                    default: break;
                }
            } catch (const std::exception& e) {
                if (debug_mode_) std::cerr << "[Loader] Error loading field '" << field.name << "': " << e.what() << std::endl;
            }
        }


        template <typename AdapterType>
        void LoadArray(const AdapterType& arrayNode, const SchemaField& field, PropertyContainer& out) {
            if (!arrayNode.IsArray()) return;
            
            const size_t count = arrayNode.Size();
            const int32_t idx = field.index;

            switch (field.type_id) {
                case FieldType::Int32:
                    for (size_t i = 0; i < count; ++i) 
                        out.int32_arrays.PushBack(idx, arrayNode.GetElement(i).template GetValue<int32_t>());
                    break;
                case FieldType::Float:
                    for (size_t i = 0; i < count; ++i) 
                        out.float_arrays.PushBack(idx, arrayNode.GetElement(i).template GetValue<float>());
                    break;
                case FieldType::String:
                    for (size_t i = 0; i < count; ++i) 
                        out.string_arrays.PushBack(idx, arrayNode.GetElement(i).template GetValue<std::string>());
                    break;
                case FieldType::Vector:
                    for (size_t i = 0; i < count; ++i) {
                        auto elem = arrayNode.GetElement(i);
                        Vector v{ elem.GetChild("x").template GetValue<double>(), 
                                  elem.GetChild("y").template GetValue<double>(), 
                                  elem.GetChild("z").template GetValue<double>() };
                        out.vector_arrays.PushBack(idx, v);
                    }
                    break;
                case FieldType::Struct:
                    for (size_t i = 0; i < count; ++i) {
                        PropertyContainer childContainer;
                        Load(arrayNode.GetElement(i), field.struct_type, childContainer);
                        out.any_struct_arrays.PushBack(idx, childContainer);
                    }
                    break;
                default: break;
            }
        }
    };
}
