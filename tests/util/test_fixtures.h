#pragma once
// Shared helpers for VTX unit tests.

#include <filesystem>
#include <fstream>
#include <string>
#include <cctype>
#include <vector>

#include "vtx/common/vtx_types.h"
#include "vtx/common/vtx_types_helpers.h"

// Injected by CMake -- see tests/CMakeLists.txt.
#ifndef VTX_TEST_FIXTURES_DIR
#error "VTX_TEST_FIXTURES_DIR must be defined by the build system"
#endif
#ifndef VTX_TEST_OUTPUT_DIR
#error "VTX_TEST_OUTPUT_DIR must be defined by the build system"
#endif

namespace VtxTest {

    /// Absolute path to the read-only fixtures directory.
    inline std::string FixturePath(const std::string& name) {
        return std::string(VTX_TEST_FIXTURES_DIR) + "/" + name;
    }

    /// Absolute path for transient test artefacts.  Directory is created by the
    /// build system but we recreate on demand in case the tester nuked it.
    inline std::string OutputPath(const std::string& name) {
        std::filesystem::path p = VTX_TEST_OUTPUT_DIR;
        std::filesystem::create_directories(p);
        return (p / name).string();
    }

    /// Replaces path separators and other problematic filename characters so test
    /// names can be embedded safely into OutputPath() results.
    inline std::string SanitizePathComponent(std::string value) {
        for (char& ch : value) {
            const unsigned char uch = static_cast<unsigned char>(ch);
            if (std::isalnum(uch) || ch == '.' || ch == '_' || ch == '-') {
                continue;
            }
            ch = '_';
        }
        return value;
    }

    /// Reads an entire file into memory.  Returns empty on failure.
    inline std::vector<std::byte> ReadAllBytes(const std::string& path) {
        std::ifstream ifs(path, std::ios::binary | std::ios::ate);
        if (!ifs)
            return {};
        const auto sz = ifs.tellg();
        if (sz <= 0)
            return {};
        ifs.seekg(0);
        std::vector<std::byte> buf(static_cast<size_t>(sz));
        ifs.read(reinterpret_cast<char*>(buf.data()), sz);
        return buf;
    }

    /// Builds a minimal PropertyContainer with a predictable shape.  Useful for
    /// content_hash and diff assertions that don't need a full schema.
    inline VTX::PropertyContainer MakeSimpleEntity(int32_t type_id, float health, VTX::Vector position,
                                                   const std::string& name = "entity") {
        VTX::PropertyContainer pc;
        pc.entity_type_id = type_id;
        pc.string_properties = {name};
        pc.float_properties = {health};
        pc.vector_properties = {position};
        pc.content_hash = VTX::Helpers::CalculateContainerHash(pc);
        return pc;
    }

} // namespace VtxTest
