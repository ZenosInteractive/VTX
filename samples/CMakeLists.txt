# ==============================================================================
# VTX SDK -- Samples
# ==============================================================================
#
# Five targets, in complexity order:
#
#   vtx_sample_read            basic_read.cpp        reads a .vtx, prints summary
#   vtx_sample_write           basic_write.cpp       writes synthetic frames
#   vtx_sample_diff            basic_diff.cpp        structural diff of two frames
#
#   vtx_sample_generate        generate_replay.cpp   arena simulator -> 3 data
#                                                    source files (JSON + Proto
#                                                    + FBS) under content/writer/arena/
#
#   vtx_sample_advance_write   advance_write.cpp     reads those 3 data sources
#                                                    through SDK integration
#                                                    primitives and writes .vtx
#                                                    replays via IFrameDataSource.
#
# Third-party deps are resolved through VTX::deps::* targets established by
# cmake/VtxDependencies.cmake -- portable across Windows and Linux.
# ==============================================================================

set(SAMPLE_CONTENT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/content")

# ------------------------------------------------------------------------------
# Small helper that applies the options every sample target needs.
# ------------------------------------------------------------------------------
function(vtx_configure_sample target)
    set_target_properties(${target} PROPERTIES
        FOLDER "Samples"
        VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    vtx_copy_runtime_deps(${target})
endfunction()

# ------------------------------------------------------------------------------
# Basic samples (single-file, no codegen required)
# ------------------------------------------------------------------------------

# --- basic_read ---
add_executable(vtx_sample_read basic_read.cpp)
target_link_libraries(vtx_sample_read PRIVATE vtx_reader)
vtx_configure_sample(vtx_sample_read)

# --- basic_write ---
add_executable(vtx_sample_write basic_write.cpp)
target_link_libraries(vtx_sample_write PRIVATE vtx_writer)
target_include_directories(vtx_sample_write PRIVATE
    "${VTX_COMMON_GENERATED_DIR}"
)
vtx_configure_sample(vtx_sample_write)

# --- basic_diff ---
add_executable(vtx_sample_diff basic_diff.cpp)
target_link_libraries(vtx_sample_diff PRIVATE vtx_reader vtx_differ)
vtx_configure_sample(vtx_sample_diff)

# --- ready_api (chunk-0 "ready" signalling: poll / block / callback) ---
add_executable(vtx_sample_ready_api ready_api.cpp)
target_link_libraries(vtx_sample_ready_api PRIVATE vtx_reader)
vtx_configure_sample(vtx_sample_ready_api)

# ==============================================================================
# Arena-specific codegen (arena_data.proto + arena_data.fbs)
#
# Both schemas describe the same game-side data model from two different
# serialization worlds.  They use distinct namespaces (arena_pb / arena_fb) so
# the generated C++ can coexist in the same translation unit.
# ==============================================================================

set(ARENA_SCHEMA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/schemas")
set(ARENA_GEN_DIR    "${CMAKE_CURRENT_BINARY_DIR}/arena_generated")
file(MAKE_DIRECTORY  ${ARENA_GEN_DIR})

# Protobuf codegen
set(ARENA_PROTO_FILE "${ARENA_SCHEMA_DIR}/arena_data.proto")
set(ARENA_PROTO_SRC  "${ARENA_GEN_DIR}/arena_data.pb.cc")
set(ARENA_PROTO_HDR  "${ARENA_GEN_DIR}/arena_data.pb.h")

add_custom_command(
    OUTPUT  ${ARENA_PROTO_SRC} ${ARENA_PROTO_HDR}
    COMMAND "${VTX_PROTOC_EXE}"
    ARGS    --cpp_out "${ARENA_GEN_DIR}" -I . "arena_data.proto"
    WORKING_DIRECTORY "${ARENA_SCHEMA_DIR}"
    DEPENDS ${ARENA_PROTO_FILE}
    COMMENT "Generating Arena Protobuf C++..."
    VERBATIM
)

# FlatBuffers codegen (--gen-object-api gives native C++ types like ArenaReplayT)
set(ARENA_FBS_FILE "${ARENA_SCHEMA_DIR}/arena_data.fbs")
set(ARENA_FBS_HDR  "${ARENA_GEN_DIR}/arena_data_generated.h")

add_custom_command(
    OUTPUT  ${ARENA_FBS_HDR}
    COMMAND "${VTX_FLATC_EXE}"
    ARGS    --cpp --gen-object-api -o "${ARENA_GEN_DIR}" "arena_data.fbs"
    WORKING_DIRECTORY "${ARENA_SCHEMA_DIR}"
    DEPENDS ${ARENA_FBS_FILE}
    COMMENT "Generating Arena FlatBuffers C++..."
    VERBATIM
)

# ------------------------------------------------------------------------------
# Advanced samples (arena pipeline, depend on arena codegen)
# ------------------------------------------------------------------------------

# --- generate_replay (arena simulator -> 3 data-source files) ---
add_executable(vtx_sample_generate
    generate_replay.cpp
    ${ARENA_PROTO_SRC}
    ${ARENA_PROTO_HDR}
    ${ARENA_FBS_HDR}
)
target_link_libraries(vtx_sample_generate PRIVATE vtx_writer)
target_include_directories(vtx_sample_generate PRIVATE
    "${VTX_COMMON_GENERATED_DIR}"
    "${ARENA_GEN_DIR}"
)
vtx_configure_sample(vtx_sample_generate)

# --- advance_write (3 arena data sources -> 3 .vtx replays) ---
add_executable(vtx_sample_advance_write
    advance_write.cpp
    ${ARENA_PROTO_SRC}
    ${ARENA_PROTO_HDR}
    ${ARENA_FBS_HDR}
    arena_generated.h
)
target_link_libraries(vtx_sample_advance_write PRIVATE vtx_writer)
target_include_directories(vtx_sample_advance_write PRIVATE
    "${VTX_COMMON_GENERATED_DIR}"
    "${ARENA_GEN_DIR}"
)
vtx_configure_sample(vtx_sample_advance_write)

# Show arena schemas in VS Solution Explorer
source_group("Arena Schemas" FILES
    "${ARENA_SCHEMA_DIR}/arena_data.proto"
    "${ARENA_SCHEMA_DIR}/arena_data.fbs"
)
target_sources(vtx_sample_generate PRIVATE
    "${ARENA_SCHEMA_DIR}/arena_data.proto"
    "${ARENA_SCHEMA_DIR}/arena_data.fbs"
)
target_sources(vtx_sample_advance_write PRIVATE
    "${ARENA_SCHEMA_DIR}/arena_data.proto"
    "${ARENA_SCHEMA_DIR}/arena_data.fbs"
)
