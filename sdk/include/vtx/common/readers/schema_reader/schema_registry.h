/**
 * @file schema_registry.h
 * 
 * @brief Central registry for runtime schema definitions.
 * 
 * @details Manages the loading and lookup of structural definitions (SchemaStruct).
 * It is essential for mapping string keys (from JSON/UI) to the efficient integer indices
 * used by `PropertyContainer`.
 * 
 * @author Zenos Interactive
 */
#pragma once
#include <string>
#include <unordered_map>
#include <google/protobuf/stubs/port.h>

#include "game_schema_types.h"
#include "vtx/common/vtx_property_cache.h"
#include "vtx/common/vtx_types.h"

namespace VTX {
    
    
    /**
     * @class SchemaRegistry
     * @brief Singleton-like registry that holds all loaded structure definitions.
     */
    class SchemaRegistry {
    public:
        enum class ELoadMethod: uint32_t
        {
            LoadToBuffer = 0,
            LoadToSchema,
            Both
        };
        /**
         * @brief Default constructor. Initializes the registry as valid.
         */
        SchemaRegistry();

        /**
         * @brief Loads the schema definitions from a JSON file.
         * @param json_path Filesystem path to the schema JSON.
         * @return true If loading and parsing were successful.
         * @return false If the file could not be opened or parsed.
         */
        bool LoadFromJson(const std::string& json_path,ELoadMethod = ELoadMethod::Both);

        /**
         * @brief
         * @param
         * @return
         * @return
         */
        bool LoadFromRawString(const std::string& raw_json);
        
        /**
         * @brief Retrieves the numeric index for a specific field within a struct.
         * @param structName The name of the structure (e.g., "PlayerState").
         * @param fieldName The name of the property (e.g., "Health").
         * @return int32_t The index for the PropertyContainer arrays, or -1 if not found.
         */
        int32_t GetIndex(const std::string& structName, const std::string& fieldName) const;

        /**
         * @brief Retrieves a pointer to a full structure definition.
         * @param name The name of the structure.
         * @return const SchemaStruct* Pointer to the schema, or nullptr if not found.
         */
        const SchemaStruct* GetStruct(const std::string& name) const;

        /**
         * @brief Checks if the registry is currently in a valid state.
         * @return true If initialized and loaded correctly.
         */
        bool GetIsValid() const;
        const std::unordered_map<std::string, VTX::SchemaStruct>& GetDefinitions() const;

        /**
         * @brief
         * @return 
         */
        const VTX::SchemaField* GetField(const std::string& struct_name, const std::string& field_name) const;
        
        /**
         * @brief Retrieves the unique integer ID for a structure (matches the generated Enum).
         * @param name The name of the structure.
         * @return int32_t The TypeId, or -1 if the struct is not registered.
         */
        int32_t GetStructTypeId(const std::string& name) const;
        
        /**/
        std::string GetContentAsString() const{return json_content_;}
        
        const VTX::PropertyAddressCache& GetPropertyCache() const
        {
            return property_cache_;
        }
    private:
        std::string json_content_;
        std::unordered_map<std::string, SchemaStruct> structs_; ///< Storage map: Struct Name -> Definition.
        std::unordered_map<std::string, int32_t> struct_type_ids_;
        VTX::PropertyAddressCache property_cache_;
        int32_t current_type_id_=0;
        bool b_is_valid_;///< Internal validity flag.
    };
            
} // namespace VTX