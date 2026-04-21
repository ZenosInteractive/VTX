# ==============================================================================
# VtxDependencies.cmake -- central third-party dependency resolution.
# ==============================================================================
#
# Public outputs:
#
#   VTX::deps::protobuf      (ALIAS -> protobuf::libprotobuf from find_package)
#   VTX::deps::flatbuffers   (INTERFACE, headers-only from FetchContent)
#   VTX::deps::zstd          (INTERFACE, wraps FetchContent libzstd_static)
#
#   VTX_PROTOC_EXE           (path to protoc)
#   VTX_FLATC_EXE            (generator expression -> flatc target file)
#
# Behaviour:
#
#   - Header-only deps (`nlohmann/json`, `xxHash`) come from `thirdparty/`.
#   - FlatBuffers + zstd: FetchContent on every platform, pinned source.
#   - Protobuf: find_package.  On Linux/macOS it resolves via system packages
#     (apt libprotobuf-dev, brew protobuf, etc.).  On Windows it resolves via
#     vcpkg when the vcpkg toolchain file is in use; any other mechanism that
#     produces the `protobuf::libprotobuf` target works too.
# ==============================================================================

set(VTX_THIRDPARTY "${CMAKE_SOURCE_DIR}/thirdparty"
    CACHE PATH "Path to the header-only third-party directory")

set(VTX_JSON_INCLUDE_DIR   "${VTX_THIRDPARTY}/jsonlohmann/include"
    CACHE PATH "nlohmann/json include directory")
set(VTX_XXHASH_INCLUDE_DIR "${VTX_THIRDPARTY}/xxhash"
    CACHE PATH "xxHash include directory")

# ==============================================================================
# FlatBuffers -- FetchContent on every platform.
# ==============================================================================
include(FetchContent)

FetchContent_Declare(
    flatbuffers_src
    GIT_REPOSITORY https://github.com/google/flatbuffers.git
    GIT_TAG        v24.12.23
    GIT_SHALLOW    TRUE
)

set(FLATBUFFERS_BUILD_TESTS     OFF CACHE BOOL "" FORCE)
set(FLATBUFFERS_INSTALL         OFF CACHE BOOL "" FORCE)
set(FLATBUFFERS_BUILD_FLATC     ON  CACHE BOOL "" FORCE)
set(FLATBUFFERS_BUILD_FLATLIB   OFF CACHE BOOL "" FORCE)
set(FLATBUFFERS_BUILD_SHAREDLIB OFF CACHE BOOL "" FORCE)
set(FLATBUFFERS_BUILD_FLATHASH  OFF CACHE BOOL "" FORCE)

set(_vtx_prev_shared ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF)
FetchContent_MakeAvailable(flatbuffers_src)
set(BUILD_SHARED_LIBS ${_vtx_prev_shared})

if(TARGET flatc)
    set_target_properties(flatc PROPERTIES FOLDER "Thirdparty/flatbuffers")
endif()

add_library(VTX_deps_flatbuffers INTERFACE)
target_include_directories(VTX_deps_flatbuffers SYSTEM INTERFACE
    $<BUILD_INTERFACE:${flatbuffers_src_SOURCE_DIR}/include>
)
add_library(VTX::deps::flatbuffers ALIAS VTX_deps_flatbuffers)

set(VTX_FLATC_EXE $<TARGET_FILE:flatc>)

# ==============================================================================
# zstd -- FetchContent on every platform.  Static library linked into VTX.
# ==============================================================================
# zstd's CMakeLists.txt lives under build/cmake/ rather than the repo root,
# so we do the Populate + add_subdirectory dance manually.  Once the minimum
# CMake is bumped to 3.18 this collapses to SOURCE_SUBDIR +
# FetchContent_MakeAvailable.
FetchContent_Declare(
    zstd_src
    GIT_REPOSITORY https://github.com/facebook/zstd.git
    GIT_TAG        v1.5.6
    GIT_SHALLOW    TRUE
)

set(ZSTD_BUILD_PROGRAMS      OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS         OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_CONTRIB       OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC        ON  CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED        OFF CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT      OFF CACHE BOOL "" FORCE)
set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "" FORCE)

set(_vtx_prev_shared_zstd ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF)
if(POLICY CMP0169)
    cmake_policy(PUSH)
    cmake_policy(SET CMP0169 OLD)
endif()
FetchContent_GetProperties(zstd_src)
if(NOT zstd_src_POPULATED)
    FetchContent_Populate(zstd_src)
    add_subdirectory(
        ${zstd_src_SOURCE_DIR}/build/cmake
        ${zstd_src_BINARY_DIR}
        EXCLUDE_FROM_ALL
    )
endif()
if(POLICY CMP0169)
    cmake_policy(POP)
endif()
set(BUILD_SHARED_LIBS ${_vtx_prev_shared_zstd})

foreach(_t libzstd_static zstd_static clean-all uninstall)
    if(TARGET ${_t})
        set_target_properties(${_t} PROPERTIES FOLDER "Thirdparty/zstd")
    endif()
endforeach()

add_library(VTX_deps_zstd INTERFACE)
target_link_libraries(VTX_deps_zstd INTERFACE libzstd_static)
target_include_directories(VTX_deps_zstd SYSTEM INTERFACE
    $<BUILD_INTERFACE:${zstd_src_SOURCE_DIR}/lib>
)
add_library(VTX::deps::zstd ALIAS VTX_deps_zstd)

# Install libzstd_static into VTX's EXPORT set so find_package(VTX) consumers
# get the zstd static library automatically (VTX::libzstd_static).
# EXCLUDE_FROM_ALL was set via add_subdirectory, so we re-enable install here.
install(TARGETS libzstd_static
        EXPORT VTXTargets
        ARCHIVE DESTINATION lib)

# ==============================================================================
# Protobuf -- find_package on every platform.
# ==============================================================================
# Try CONFIG mode first (preferred -- provides protobuf::libprotobuf target
# with proper usage requirements), then fall back to the FindProtobuf.cmake
# module.  The first call must NOT be REQUIRED -- distros without protobuf's
# cmake config files (Ubuntu 22.04 ships libprotobuf-dev without
# ProtobufConfig.cmake) would abort the build before the module-mode fallback
# runs.
find_package(Protobuf CONFIG QUIET)
if(NOT Protobuf_FOUND)
    find_package(Protobuf REQUIRED MODULE)
endif()

if(TARGET protobuf::libprotobuf)
    add_library(VTX::deps::protobuf ALIAS protobuf::libprotobuf)
else()
    add_library(VTX_deps_protobuf INTERFACE)
    target_include_directories(VTX_deps_protobuf SYSTEM INTERFACE ${Protobuf_INCLUDE_DIRS})
    target_link_libraries(VTX_deps_protobuf INTERFACE ${Protobuf_LIBRARIES})
    add_library(VTX::deps::protobuf ALIAS VTX_deps_protobuf)
endif()

find_program(VTX_PROTOC_EXE protoc
    DOC "Path to the protoc binary"
    REQUIRED)

message(STATUS "VTX deps: protoc=${VTX_PROTOC_EXE}")
message(STATUS "VTX deps: flatc=<built from FetchContent source ${flatbuffers_src_SOURCE_DIR}>")
message(STATUS "VTX deps: zstd=<built from FetchContent source ${zstd_src_SOURCE_DIR}>")

# ==============================================================================
# Runtime-dep copy helper -- no-op stub kept at historical call sites so
# reintroducing a runtime DLL dep is a one-line change here.
# ==============================================================================
function(vtx_copy_runtime_deps target)
    if(NOT TARGET ${target})
        message(WARNING "vtx_copy_runtime_deps: target '${target}' does not exist")
    endif()
endfunction()
