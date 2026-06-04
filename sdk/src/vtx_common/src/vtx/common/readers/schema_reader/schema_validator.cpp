/**
 * @file schema_validator.cpp
 * @brief JSON extraction + concrete validation rules for SchemaValidator.
 * @author Zenos Interactive
 */
#include "vtx/common/readers/schema_reader/schema_validator.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <nlohmann/json.hpp>

#include "vtx/common/readers/schema_reader/schema_enums.h"

using json = nlohmann::json;

namespace VTX {

    namespace {

        // -----------------------------------------------------------------
        //  JSON -> RawSchema extraction
        // -----------------------------------------------------------------

        RawSchemaField ExtractField(const json& field_json, int field_index) {
            RawSchemaField field;
            field.field_index = field_index;
            field.name = field_json.value("name", "");
            field.struct_type = field_json.value("structType", "");
            field.type_id = field_json.value("typeId", "");
            field.key_id = field_json.value("keyId", "");
            field.container_type = field_json.value("containerType", "");

            if (field_json.contains("meta") && field_json["meta"].is_object()) {
                const auto& meta = field_json["meta"];
                field.default_value = meta.value("defaultValue", "");
                if (meta.contains("fixedArrayDim") && meta["fixedArrayDim"].is_number_integer()) {
                    field.has_fixed_array_dim = true;
                    field.fixed_array_dim = meta["fixedArrayDim"].get<int64_t>();
                }
            }
            return field;
        }

        RawSchema ExtractRawSchema(const json& root, SchemaValidationResult& out) {
            RawSchema schema;
            const auto& mapping = root["property_mapping"];

            int struct_index = 0;
            for (const auto& struct_json : mapping) {
                if (!struct_json.is_object()) {
                    out.AddError("SchemaStructure", "", "",
                                 "property_mapping entry #" + std::to_string(struct_index) + " is not an object.");
                    ++struct_index;
                    continue;
                }

                RawSchemaStruct entry;
                entry.struct_index = struct_index;
                entry.name = struct_json.value("struct", "");
                if (struct_json.contains("id") && struct_json["id"].is_number_integer()) {
                    entry.declared_id = struct_json["id"].get<int64_t>();
                }

                if (struct_json.contains("values") && struct_json["values"].is_array()) {
                    int field_index = 0;
                    for (const auto& field_json : struct_json["values"]) {
                        if (!field_json.is_object()) {
                            out.AddError("SchemaStructure", entry.name, "",
                                         "values entry #" + std::to_string(field_index) + " is not an object.");
                            ++field_index;
                            continue;
                        }
                        entry.fields.push_back(ExtractField(field_json, field_index));
                        ++field_index;
                    }
                }

                schema.structs.push_back(std::move(entry));
                ++struct_index;
            }
            return schema;
        }

        // -----------------------------------------------------------------
        //  Rules (one per schema requirement)
        // -----------------------------------------------------------------

        /// #1 unknown typeId rejected, #4 every FieldType supported.
        class FieldTypeRule final : public ISchemaValidationRule {
        public:
            std::string Name() const override { return "FieldType"; }
            void Validate(const RawSchema& schema, SchemaValidationResult& out) const override {
                for (const auto& s : schema.structs) {
                    for (const auto& f : s.fields) {
                        const auto type = ParseFieldType(f.type_id);
                        if (!type.has_value()) {
                            out.AddError(Name(), s.name, f.name, "unknown/unsupported typeId '" + f.type_id + "'.");
                        } else if (*type == FieldType::None) {
                            out.AddError(Name(), s.name, f.name, "field type cannot be 'None'.");
                        }
                    }
                }
            }
        };

        /// #2 unknown containerType rejected, #3 canonical casing accepted.
        class ContainerTypeRule final : public ISchemaValidationRule {
        public:
            std::string Name() const override { return "ContainerType"; }
            void Validate(const RawSchema& schema, SchemaValidationResult& out) const override {
                for (const auto& s : schema.structs) {
                    for (const auto& f : s.fields) {
                        if (!ParseContainerType(f.container_type).has_value()) {
                            out.AddError(Name(), s.name, f.name,
                                         "unknown containerType '" + f.container_type +
                                             "' (expected None, Array or Map).");
                        }
                    }
                }
            }
        };

        /// #5 reject duplicate (or empty) field names within a struct.
        class DuplicateFieldNameRule final : public ISchemaValidationRule {
        public:
            std::string Name() const override { return "DuplicateFieldName"; }
            void Validate(const RawSchema& schema, SchemaValidationResult& out) const override {
                for (const auto& s : schema.structs) {
                    std::unordered_set<std::string> seen;
                    for (const auto& f : s.fields) {
                        if (f.name.empty()) {
                            out.AddError(Name(), s.name, "",
                                         "field #" + std::to_string(f.field_index) + " has an empty name.");
                            continue;
                        }
                        if (!seen.insert(f.name).second) {
                            out.AddError(Name(), s.name, f.name, "duplicate field name within the struct.");
                        }
                    }
                }
            }
        };

        /// #6 reject duplicate (or empty) struct names and duplicate struct ids.
        class DuplicateStructRule final : public ISchemaValidationRule {
        public:
            std::string Name() const override { return "DuplicateStruct"; }
            void Validate(const RawSchema& schema, SchemaValidationResult& out) const override {
                std::unordered_set<std::string> names;
                std::unordered_set<int64_t> ids;
                for (const auto& s : schema.structs) {
                    if (s.name.empty()) {
                        out.AddError(Name(), "", "",
                                     "struct #" + std::to_string(s.struct_index) + " has an empty name.");
                    } else if (!names.insert(s.name).second) {
                        out.AddError(Name(), s.name, "", "duplicate struct name.");
                    }
                    if (s.declared_id.has_value() && !ids.insert(*s.declared_id).second) {
                        out.AddError(Name(), s.name, "", "duplicate struct id " + std::to_string(*s.declared_id) + ".");
                    }
                }
            }
        };

        /// #7 reject Struct fields whose structType does not resolve.
        class StructTypeResolutionRule final : public ISchemaValidationRule {
        public:
            std::string Name() const override { return "StructTypeResolution"; }
            void Validate(const RawSchema& schema, SchemaValidationResult& out) const override {
                std::unordered_set<std::string> known;
                for (const auto& s : schema.structs) {
                    if (!s.name.empty()) {
                        known.insert(s.name);
                    }
                }

                for (const auto& s : schema.structs) {
                    for (const auto& f : s.fields) {
                        const auto type = ParseFieldType(f.type_id);
                        const bool is_struct = type.has_value() && *type == FieldType::Struct;

                        if (is_struct) {
                            if (f.struct_type.empty()) {
                                out.AddError(Name(), s.name, f.name, "Struct field is missing 'structType'.");
                            } else if (known.find(f.struct_type) == known.end()) {
                                out.AddError(Name(), s.name, f.name,
                                             "structType '" + f.struct_type + "' does not resolve to a known struct.");
                            }
                        } else if (!f.struct_type.empty()) {
                            out.AddWarning(Name(), s.name, f.name,
                                           "structType '" + f.struct_type + "' is ignored on a non-Struct field.");
                        }
                    }
                }
            }
        };

        /// #8 reject Map fields whose keyId is missing, unsupported or incompatible.
        class MapKeyRule final : public ISchemaValidationRule {
        public:
            std::string Name() const override { return "MapKey"; }
            void Validate(const RawSchema& schema, SchemaValidationResult& out) const override {
                for (const auto& s : schema.structs) {
                    for (const auto& f : s.fields) {
                        const auto container = ParseContainerType(f.container_type);
                        if (!container.has_value() || *container != FieldContainerType::Map) {
                            continue; // not a (valid) map field.
                        }

                        const auto key = ParseFieldType(f.key_id);
                        if (f.key_id.empty() || !key.has_value() || *key == FieldType::None) {
                            out.AddError(Name(), s.name, f.name, "Map field is missing a valid keyId.");
                        } else if (!IsMapCompatibleKeyType(*key)) {
                            out.AddError(Name(), s.name, f.name,
                                         "Map keyId '" + f.key_id + "' is not a serializable map key type.");
                        }
                    }
                }
            }
        };

        /// #9 validate meta.defaultValue against the declared field type.
        class DefaultValueRule final : public ISchemaValidationRule {
        public:
            std::string Name() const override { return "DefaultValue"; }
            void Validate(const RawSchema& schema, SchemaValidationResult& out) const override {
                for (const auto& s : schema.structs) {
                    for (const auto& f : s.fields) {
                        const auto type = ParseFieldType(f.type_id);
                        if (!type.has_value() || *type == FieldType::None) {
                            continue; // FieldTypeRule already reports this.
                        }
                        if (!IsValidDefaultValue(*type, f.default_value)) {
                            out.AddError(Name(), s.name, f.name,
                                         "defaultValue '" + f.default_value + "' is not valid for type " +
                                             std::string(ToString(*type)) + ".");
                        }
                    }
                }
            }
        };

        /// #10 validate meta.fixedArrayDim (schema-time portion).
        class FixedArrayDimRule final : public ISchemaValidationRule {
        public:
            std::string Name() const override { return "FixedArrayDim"; }
            void Validate(const RawSchema& schema, SchemaValidationResult& out) const override {
                for (const auto& s : schema.structs) {
                    for (const auto& f : s.fields) {
                        if (!f.has_fixed_array_dim) {
                            continue;
                        }
                        if (f.fixed_array_dim < 0) {
                            out.AddError(Name(), s.name, f.name,
                                         "fixedArrayDim cannot be negative (" + std::to_string(f.fixed_array_dim) +
                                             ").");
                            continue;
                        }
                        const auto container = ParseContainerType(f.container_type);
                        if (container.has_value() && *container == FieldContainerType::None && f.fixed_array_dim > 1) {
                            out.AddError(Name(), s.name, f.name,
                                         "scalar field declares fixedArrayDim " + std::to_string(f.fixed_array_dim) +
                                             " > 1 but is not an Array.");
                        }
                    }
                }
            }
        };

    } // namespace

    // ---------------------------------------------------------------------
    //  SchemaValidator
    // ---------------------------------------------------------------------

    std::vector<std::unique_ptr<ISchemaValidationRule>> SchemaValidator::DefaultRules() {
        std::vector<std::unique_ptr<ISchemaValidationRule>> rules;
        rules.push_back(std::make_unique<FieldTypeRule>());
        rules.push_back(std::make_unique<ContainerTypeRule>());
        rules.push_back(std::make_unique<DuplicateFieldNameRule>());
        rules.push_back(std::make_unique<DuplicateStructRule>());
        rules.push_back(std::make_unique<StructTypeResolutionRule>());
        rules.push_back(std::make_unique<MapKeyRule>());
        rules.push_back(std::make_unique<DefaultValueRule>());
        rules.push_back(std::make_unique<FixedArrayDimRule>());
        return rules;
    }

    SchemaValidator::SchemaValidator()
        : rules_(DefaultRules()) {}

    SchemaValidator::SchemaValidator(std::vector<std::unique_ptr<ISchemaValidationRule>> rules)
        : rules_(std::move(rules)) {}

    void SchemaValidator::AddRule(std::unique_ptr<ISchemaValidationRule> rule) {
        if (rule) {
            rules_.push_back(std::move(rule));
        }
    }

    SchemaValidationResult SchemaValidator::Validate(const RawSchema& schema) const {
        SchemaValidationResult result;
        for (const auto& rule : rules_) {
            rule->Validate(schema, result);
        }
        return result;
    }

    SchemaValidationResult SchemaValidator::Validate(const std::string& raw_json) const {
        SchemaValidationResult result;

        json root;
        try {
            root = json::parse(raw_json);
        } catch (const json::parse_error& e) {
            result.AddError("Json", "", "", std::string("JSON parse error: ") + e.what());
            return result;
        }

        if (!root.contains("property_mapping") || !root["property_mapping"].is_array()) {
            result.AddError("SchemaStructure", "", "", "schema has no 'property_mapping' array.");
            return result;
        }

        const RawSchema schema = ExtractRawSchema(root, result);
        result.Absorb(Validate(schema));
        return result;
    }

} // namespace VTX
