#pragma once
#include <string>
#include <unordered_map>
#include <cstdint>

namespace VTX {

    class EntityTypeRegistry {
    public:
        static void RegisterName(int32_t type_id, const std::string& name) {
            GetInternalMap()[type_id] = name;
        }

        static std::string GetName(int32_t type_id) {
            auto& map = GetInternalMap();
            auto it = map.find(type_id);
            if (it != map.end()) {
                return it->second;
            }
            return "Type_" + std::to_string(type_id);
        }

        static void Clear() {
            GetInternalMap().clear();
        }

    private:
        static std::unordered_map<int32_t, std::string>& GetInternalMap() {
            static std::unordered_map<int32_t, std::string> instance;
            return instance;
        }
    };
} // namespace VTX