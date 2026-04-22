cmake_minimum_required(VERSION 3.15)
project(VTX_SDK VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Platform-specific compiler settings --------------------------------------
if(MSVC)
    # Match the dynamic CRT that protobuf and FlatBuffers ship against.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
else()
    # Build position-independent code so static libs can be linked into .so's.
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

# Enable Folders in IDEs
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# Output directories
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Help the installed binaries find bundled shared libs on Linux/macOS.
if(UNIX AND NOT APPLE)
    set(CMAKE_INSTALL_RPATH "$ORIGIN/../lib")
elseif(APPLE)
    set(CMAKE_INSTALL_RPATH "@loader_path/../lib")
endif()
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH ON)

# --- CMake module path + central dependency resolution ------------------------
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(VtxDependencies)

if(MSVC AND VTX_NEEDS_PROTOBUF_STATIC_DEFINE)
    # The legacy bundled Windows protobuf ships as static libraries. Package-
    # manager builds provide the right usage requirements through imported
    # targets, so we only force this define for the bundled fallback path.
    add_compile_definitions(PROTOBUF_STATIC_LIBRARY)
endif()

# ------------------------------------------------------------------------------
# VTX_SANITIZE -- opt into a runtime sanitizer (gcc/clang only).
#
# Values: OFF (default), address, undefined, "address,undefined", thread
#
#   address    -- ASan: heap/stack/global OOB, use-after-free, double-free, leaks
#   undefined  -- UBsan: integer overflow, null deref, misaligned access
#   thread     -- TSan: data races, deadlocks (mutually exclusive with 'address')
#
# Enabled in CI via two matrix jobs (Linux / ASan+UBsan / Debug and
# Linux / TSan / Debug).  Also usable locally -- see docs/BUILD.md.
#
# MSVC is skipped: MSVC ASan exists but uses /fsanitize=address and has no
# TSan/UBsan; if we want Windows sanitizer coverage later it's a separate
# CMake branch.
# ------------------------------------------------------------------------------
set(VTX_SANITIZE "OFF" CACHE STRING "Enable a sanitizer: OFF, address, undefined, address,undefined, thread")
set_property(CACHE VTX_SANITIZE PROPERTY STRINGS
    OFF  address undefined "address,undefined" thread)

if(NOT MSVC AND NOT VTX_SANITIZE STREQUAL "OFF")
    add_compile_options(-fsanitize=${VTX_SANITIZE} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${VTX_SANITIZE})
    message(STATUS "VTX: sanitizer enabled (${VTX_SANITIZE})")
endif()

set(VTX_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/sdk/include")

option(VTX_BUILD_WRITER "Build vtx_writer module" ON)
option(VTX_BUILD_READER "Build vtx_reader module" ON)
option(VTX_BUILD_DIFFER "Build vtx_differ module" ON)
option(BUILD_VTX_TOOL "Build tool suite (tools/)" ON)
option(BUILD_VTX_SAMPLES "Build sample programs (samples/)" ON)
option(VTX_BUILD_TESTS "Build unit test suite (tests/)" ON)

# ------------------------------------------------------------------------------
# VTX_BUILD_SHARED -- static (default) vs shared SDK libraries
# ------------------------------------------------------------------------------
#
# Off (default): vtx_common, vtx_reader, vtx_writer, vtx_differ are built as
#                static libraries.  Consumers link them directly into their
#                binary -- zero runtime discovery, zero DLL dependency surface.
#
# On:            All four are built as shared libraries (DLL / .so / .dylib).
#                Useful for language bindings, hot-reload, and multi-module
#                apps that want a single shared VTX runtime.
#
# Note: on Windows, shared builds enable CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS so
# every public function is exported automatically, including those reachable
# through bundled static protobuf / flatbuffers.  For a production-grade SDK
# you'd replace this with a VTX_API macro annotated on every public symbol --
# that's a separate ticket (see docs/BUILD.md).
# ------------------------------------------------------------------------------
option(VTX_BUILD_SHARED "Build VTX libraries as shared (DLL / .so) instead of static" OFF)

if(VTX_BUILD_SHARED)
    set(BUILD_SHARED_LIBS ON)
    set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
    message(STATUS "VTX: Building SHARED libraries")
else()
    set(BUILD_SHARED_LIBS OFF)
    message(STATUS "VTX: Building STATIC libraries")
endif()

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

# --- Tests ---
if(VTX_BUILD_TESTS AND VTX_BUILD_READER AND VTX_BUILD_WRITER AND VTX_BUILD_DIFFER)
    message(STATUS "VTX: Building tests...")
    enable_testing()
    add_subdirectory(tests)
else()
    message(STATUS "VTX: Tests disabled.")
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
