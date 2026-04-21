project(vtx_common)

# ==============================================================================
# vtx_common
# ==============================================================================
# All third-party dependency resolution happens in cmake/VtxDependencies.cmake,
# which exposes:
#
#   VTX::deps::protobuf      (static on Win, shared on Linux)
#   VTX::deps::flatbuffers
#   VTX::deps::zstd
#   VTX::deps::json          (header-only)
#   VTX::deps::xxhash        (header-only)
#   VTX_PROTOC_EXE           path to protoc
#   VTX_FLATC_EXE            path to flatc
#   vtx_copy_runtime_deps()  helper -- currently a no-op stub (nothing to
#                            copy at runtime; zstd is statically linked).
# ==============================================================================

# ==============================================================================
# 1. Schema code generation (Protobuf + FlatBuffers)
# ==============================================================================
set(SCHEMA_DIR    "${CMAKE_SOURCE_DIR}/sdk/src/schemas")

# Generated protobuf/flatbuffers code lives under the build tree, not the
# source tree.  Reasons:
#   1. The flatc / protoc versions differ between platforms (bundled 25.x on
#      Windows, apt 2.x on Ubuntu 22.04).  Having the generated code inside
#      the source tree caused cross-platform crashes when the files got
#      shared between machines (e.g. rsync / shared mount).
#   2. Build artefacts shouldn't pollute the source tree.
#
# Expose the path as a cache variable so sibling CMakeLists (samples, tests,
# tools, vtx_reader/writer/differ) can reference it.
set(VTX_COMMON_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated"
    CACHE INTERNAL "Directory containing generated protobuf/flatbuffers C++ code")
set(GENERATED_DIR "${VTX_COMMON_GENERATED_DIR}")
file(MAKE_DIRECTORY ${GENERATED_DIR})

set(PROTO_FILENAME "vtx_schema")
set(PROTO_FILE     "${SCHEMA_DIR}/${PROTO_FILENAME}.proto")
set(PROTO_SRC      "${GENERATED_DIR}/${PROTO_FILENAME}.pb.cc")
set(PROTO_HDR      "${GENERATED_DIR}/${PROTO_FILENAME}.pb.h")

add_custom_command(
    OUTPUT  ${PROTO_SRC} ${PROTO_HDR}
    COMMAND "${VTX_PROTOC_EXE}"
    ARGS    --cpp_out "${GENERATED_DIR}" -I . "${PROTO_FILENAME}.proto"
    WORKING_DIRECTORY "${SCHEMA_DIR}"
    DEPENDS ${PROTO_FILE}
    COMMENT "Generating Protobuf C++..."
    VERBATIM
)

set(FBS_FILENAME "vtx_schema")
set(FBS_FILE     "${SCHEMA_DIR}/${FBS_FILENAME}.fbs")
set(FBS_HDR      "${GENERATED_DIR}/${FBS_FILENAME}_generated.h")

add_custom_command(
    OUTPUT  ${FBS_HDR} "${GENERATED_DIR}/vtx_schema_bfbs_generated.h"
    COMMAND "${VTX_FLATC_EXE}"
    ARGS    --cpp --gen-object-api -b --schema --bfbs-gen-embed -o "${GENERATED_DIR}" "${FBS_FILENAME}.fbs"
    WORKING_DIRECTORY "${SCHEMA_DIR}"
    DEPENDS ${FBS_FILE}
    COMMENT "Generating FlatBuffers C++..."
    VERBATIM
)

# ==============================================================================
# 2. Source files
# ==============================================================================
file(GLOB_RECURSE EXISTING_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/sdk/include/vtx/common/*.h"
    "${CMAKE_SOURCE_DIR}/sdk/include/vtx/common/*.hpp"
)

file(GLOB_RECURSE EXISTING_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cc"
)
# The generated sources belong to the vtx_generated_code OBJECT library below.
# Drop them from this module's direct sources so we don't double-compile them
# (and get duplicate symbols at link time).
list(FILTER EXISTING_SOURCES EXCLUDE REGEX "/src/generated/")

# ==============================================================================
# 3. Target (vtx_common)
# ==============================================================================
#
# The generated protobuf / FlatBuffers sources live in an OBJECT library.
# Motivation: on Windows with VTX_BUILD_SHARED=ON, DLLs hide symbols by default
# and CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS cannot export the data globals that
# protobuf emits (the `_default_instance_` variables).  Letting each SDK DLL
# compile its own copy of the generated objects makes each DLL a self-contained
# linking unit.  For static builds it stays as a single copy inside vtx_common.
# ==============================================================================
add_library(vtx_generated_code OBJECT
    ${PROTO_SRC} ${PROTO_HDR}
    ${FBS_HDR}
)
target_include_directories(vtx_generated_code PUBLIC
    $<BUILD_INTERFACE:${GENERATED_DIR}>
)
target_link_libraries(vtx_generated_code PUBLIC
    $<BUILD_INTERFACE:VTX::deps::protobuf>
    $<BUILD_INTERFACE:VTX::deps::flatbuffers>
)
set_target_properties(vtx_generated_code PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    FOLDER "SDK/Internal"
)
source_group("Generated" FILES ${PROTO_SRC} ${PROTO_HDR} ${FBS_HDR})

# Library type (STATIC/SHARED) is picked by BUILD_SHARED_LIBS -- see root
# CMakeLists.txt and the VTX_BUILD_SHARED option.
add_library(vtx_common
    ${EXISTING_HEADERS}
    ${EXISTING_SOURCES}
)
# BUILD_INTERFACE keeps the OBJECT library out of the install(EXPORT) tree;
# downstream consumers never need to link it directly.
target_link_libraries(vtx_common PRIVATE $<BUILD_INTERFACE:vtx_generated_code>)

file(GLOB_RECURSE IDE_SCHEMA_FILES
    "${SCHEMA_DIR}/*.fbs"
    "${SCHEMA_DIR}/*.proto"
)
source_group("Schemas" FILES ${IDE_SCHEMA_FILES})
target_sources(vtx_common PRIVATE ${IDE_SCHEMA_FILES})

target_link_libraries(vtx_common
    PUBLIC
        # BUILD_INTERFACE keeps the imported thirdparty targets out of the
        # install(EXPORT) tree -- consumers of the installed SDK supply their
        # own protobuf / flatbuffers / zstd (documented in docs/BUILD.md).
        $<BUILD_INTERFACE:VTX::deps::protobuf>
        $<BUILD_INTERFACE:VTX::deps::flatbuffers>
        $<BUILD_INTERFACE:VTX::deps::zstd>
)

# ==============================================================================
# 4. Include directories
# ==============================================================================
target_include_directories(vtx_common
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/sdk/include>
        $<BUILD_INTERFACE:${VTX_JSON_INCLUDE_DIR}>
        $<BUILD_INTERFACE:${VTX_XXHASH_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        "${GENERATED_DIR}"
)

# ==============================================================================
# 5. Runtime deps + IDE filters
# ==============================================================================
source_group(TREE "${CMAKE_SOURCE_DIR}/sdk/include/vtx/common" PREFIX "Header Files" FILES ${EXISTING_HEADERS})
source_group(TREE "${SCHEMA_DIR}" PREFIX "Schemas" FILES ${IDE_SCHEMA_FILES})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" PREFIX "Source Files" FILES ${EXISTING_SOURCES})

# No-op stub: zstd is statically linked from FetchContent, nothing to copy.
# Kept at all historical call sites so reintroducing a runtime dep is a
# one-line change in VtxDependencies.cmake instead of a sweep here.
vtx_copy_runtime_deps(vtx_common)

install(TARGETS vtx_common EXPORT VTXTargets
    ARCHIVE DESTINATION lib    # static .lib / .a
    LIBRARY DESTINATION lib    # shared .so / .dylib
    RUNTIME DESTINATION bin    # shared .dll on Windows
)
