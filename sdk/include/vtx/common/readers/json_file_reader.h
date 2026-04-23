/**
* @file json_file_reader.h
 * @brief Helper functions for reading json files.
 * @author Zenos Interactive
 */
#pragma once
#include <fstream>
#include <iostream>
#include <string>
#include <optional>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
namespace VTX {
    namespace JsonFileReader {

        std::optional<json> Load(const std::string& filepath) {
            std::ifstream file(filepath);
            if (!file.is_open()) {
                std::cerr << "[Error I/O] Can not find the file : " << filepath;
                return std::nullopt;
            }

            json j;
            try {
                file >> j;
            } catch (const json::parse_error& e) {
                std::cerr << "[Error Parser] JSON malformed " << filepath << "\nDetalles: " << e.what() << std::endl;
                return std::nullopt;
            }

            return j;
        }

        std::optional<std::string> LoadAsString(const std::string& filepath) {
            std::ifstream file(filepath);
            if (!file.is_open()) {
                std::cerr << "[Error I/O] File can not be found or opened: " << filepath << std::endl;
                return std::nullopt;
            }

            std::ostringstream buffer;
            buffer << file.rdbuf();

            return buffer.str();
        }
    }; // namespace JsonFileReader
} // namespace VTX