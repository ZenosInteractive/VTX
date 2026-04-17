cmake_minimum_required(VERSION 3.15)
project(VTX_SDK VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

add_compile_definitions(PROTOBUF_STATIC_LIBRARY)

# Enable Folders in IDEs
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# Output directories
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

set(VTX_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/sdk/include")

option(VTX_BUILD_WRITER "Build vtx_writer module" ON)
option(VTX_BUILD_READER "Build vtx_reader module" ON)
option(VTX_BUILD_DIFFER "Build vtx_differ module" ON)
option(BUILD_VTX_TOOL "Build tool suite (tools/)" ON)
option(BUILD_VTX_SAMPLES "Build sample programs (samples/)" ON)
option(VTX_BUILD_TESTS "Build unit test suite (tests/)" ON)

# --- SDK Modules ---
add_subdirectory(sdk/src)

if(VTX_BUILD_WRITER)
    add_compile_definitions(VTX_HAS_WRITER)
endif()
if(VTX_BUILD_READER)
    add_compile_definitions(VTX_HAS_READER)
endif()
if(VTX_BUILD_DIFFER)
    add_compile_definitions(VTX_HAS_DIFFER)
endif()

# --- Tools ---
if(BUILD_VTX_TOOL AND VTX_BUILD_READER AND VTX_BUILD_WRITER)
    message(STATUS "VTX: Building tools...")
    add_subdirectory(tools)
else()
    message(STATUS "VTX: Tools disabled.")
endif()

# --- Samples ---
if(BUILD_VTX_SAMPLES AND VTX_BUILD_READER AND VTX_BUILD_WRITER)
    message(STATUS "VTX: Building samples...")
    add_subdirectory(samples)
else()
    message(STATUS "VTX: Samples disabled.")
endif()

# --- Install ---
install(DIRECTORY "${CMAKE_SOURCE_DIR}/sdk/include/"
        DESTINATION include
        FILES_MATCHING
        PATTERN "*.h"
        PATTERN "*.hpp"
        PATTERN "*.proto"
        PATTERN "*.fbs"
        PATTERN "*.cc" EXCLUDE
        PATTERN "*.cpp" EXCLUDE
)

install(DIRECTORY "${CMAKE_SOURCE_DIR}/scripts/"
        DESTINATION scripts
        USE_SOURCE_PERMISSIONS
        FILES_MATCHING
        PATTERN "*.py"
)

# --- CMake Package Config (for find_package) ---
include(CMakePackageConfigHelpers)

install(EXPORT VTXTargets
    FILE VTXTargets.cmake
    NAMESPACE VTX::
    DESTINATION lib/cmake/VTX
)

configure_package_config_file(
    "${CMAKE_SOURCE_DIR}/cmake/VTXConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/VTXConfig.cmake"
    INSTALL_DESTINATION lib/cmake/VTX
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/VTXConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/VTXConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/VTXConfigVersion.cmake"
    DESTINATION lib/cmake/VTX
)

# IDE helpers
file(GLOB_RECURSE IDE_SCRIPT_FILES "${CMAKE_SOURCE_DIR}/scripts/*")
set(ROOT_CONFIG_FILES "${CMAKE_CURRENT_LIST_FILE}")
add_custom_target(vtx_utility_scripts SOURCES ${IDE_SCRIPT_FILES})
source_group("Scripts" FILES ${IDE_SCRIPT_FILES})
source_group("CMake" FILES ${ROOT_CONFIG_FILES})
set_target_properties(vtx_utility_scripts PROPERTIES FOLDER "SDK Tools")
