# ==============================================================================
# VTX SDK -- Samples
# ==============================================================================
#
# Five targets, in complexity order:
#
#   vtx_sample_read            basic_read.cpp        reads a .vtx, prints summary
#   vtx_sample_write           basic_write.cpp       writes synthetic frames
#   vtx_sample_diff            basic_diff.cpp        hash-based per-frame delta
#
#   vtx_sample_generate        generate_replay.cpp   arena simulator -> 3 data
#                                                    source files (JSON + Proto
#                                                    + FBS) under content/writer/arena/
#
#   vtx_sample_advance_write   advance_write.cpp     reads those 3 data sources
#                                                    through SDK integration
#                                                    primitives (JsonMapping /
#                                                    ProtoBinding / FlatBufferBinding)
#                                                    and writes .vtx replays via
#                                                    VTX::IFrameDataSource.
#
# The last two targets depend on generated arena schema code (below).
# ==============================================================================

set(SAMPLE_CONTENT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/content")

# ------------------------------------------------------------------------------
# Basic samples (single-file, no codegen required)
# ------------------------------------------------------------------------------

# --- basic_read ---
add_executable(vtx_sample_read basic_read.cpp)
target_link_libraries(vtx_sample_read PRIVATE vtx_reader)
target_compile_definitions(vtx_sample_read PRIVATE PROTOBUF_STATIC_LIBRARY)
set_target_properties(vtx_sample_read PROPERTIES
    FOLDER "Samples"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
)

# --- basic_write ---
add_executable(vtx_sample_write basic_write.cpp)
target_link_libraries(vtx_sample_write PRIVATE vtx_writer)
target_include_directories(vtx_sample_write PRIVATE
    "${CMAKE_SOURCE_DIR}/thirdparty/protobuf/include"
    "${CMAKE_SOURCE_DIR}/thirdparty/flatbuffers/include"
    "${CMAKE_SOURCE_DIR}/sdk/src/vtx_common/src/generated"
)
target_compile_definitions(vtx_sample_write PRIVATE PROTOBUF_STATIC_LIBRARY)
set_target_properties(vtx_sample_write PROPERTIES
    FOLDER "Samples"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
)

# --- basic_diff ---
add_executable(vtx_sample_diff basic_diff.cpp)
target_link_libraries(vtx_sample_diff PRIVATE vtx_reader vtx_differ)
target_compile_definitions(vtx_sample_diff PRIVATE PROTOBUF_STATIC_LIBRARY)
set_target_properties(vtx_sample_diff PROPERTIES
    FOLDER "Samples"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
)

# ==============================================================================
# Arena-specific codegen (arena_data.proto + arena_data.fbs)
#
# Both schemas describe the same game-side data model, but from two different
# serialization worlds.  They use distinct namespaces (arena_pb / arena_fb) so
# the generated C++ can coexist in the same translation unit.
# ==============================================================================
set(PROTO_ROOT  "${CMAKE_SOURCE_DIR}/thirdparty/protobuf")
set(PROTOC_EXE  "${PROTO_ROOT}/bin/protoc.exe")
set(FLAT_ROOT   "${CMAKE_SOURCE_DIR}/thirdparty/flatbuffers")
set(FLATC_EXE   "${FLAT_ROOT}/bin/flatc.exe")

set(ARENA_SCHEMA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/schemas")
set(ARENA_GEN_DIR    "${CMAKE_CURRENT_BINARY_DIR}/arena_generated")
file(MAKE_DIRECTORY  ${ARENA_GEN_DIR})

# Protobuf codegen
set(ARENA_PROTO_FILE "${ARENA_SCHEMA_DIR}/arena_data.proto")
set(ARENA_PROTO_SRC  "${ARENA_GEN_DIR}/arena_data.pb.cc")
set(ARENA_PROTO_HDR  "${ARENA_GEN_DIR}/arena_data.pb.h")

add_custom_command(
    OUTPUT  ${ARENA_PROTO_SRC} ${ARENA_PROTO_HDR}
    COMMAND "${PROTOC_EXE}"
    ARGS    --cpp_out "${ARENA_GEN_DIR}" -I . "arena_data.proto"
    WORKING_DIRECTORY "${ARENA_SCHEMA_DIR}"
    DEPENDS ${ARENA_PROTO_FILE}
    COMMENT "Generating Arena Protobuf C++..."
)

# FlatBuffers codegen (--gen-object-api gives native C++ types like ArenaReplayT)
set(ARENA_FBS_FILE "${ARENA_SCHEMA_DIR}/arena_data.fbs")
set(ARENA_FBS_HDR  "${ARENA_GEN_DIR}/arena_data_generated.h")

add_custom_command(
    OUTPUT  ${ARENA_FBS_HDR}
    COMMAND "${FLATC_EXE}"
    ARGS    --cpp --gen-object-api -o "${ARENA_GEN_DIR}" "arena_data.fbs"
    WORKING_DIRECTORY "${ARENA_SCHEMA_DIR}"
    DEPENDS ${ARENA_FBS_FILE}
    COMMENT "Generating Arena FlatBuffers C++..."
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
    "${CMAKE_SOURCE_DIR}/thirdparty/protobuf/include"
    "${CMAKE_SOURCE_DIR}/thirdparty/flatbuffers/include"
    "${CMAKE_SOURCE_DIR}/sdk/src/vtx_common/src/generated"
    "${ARENA_GEN_DIR}"
)
target_compile_definitions(vtx_sample_generate PRIVATE PROTOBUF_STATIC_LIBRARY)
set_target_properties(vtx_sample_generate PROPERTIES
    FOLDER "Samples"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
)

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
    "${CMAKE_SOURCE_DIR}/thirdparty/protobuf/include"
    "${CMAKE_SOURCE_DIR}/thirdparty/flatbuffers/include"
    "${CMAKE_SOURCE_DIR}/sdk/src/vtx_common/src/generated"
    "${ARENA_GEN_DIR}"
)
target_compile_definitions(vtx_sample_advance_write PRIVATE PROTOBUF_STATIC_LIBRARY)
set_target_properties(vtx_sample_advance_write PROPERTIES
    FOLDER "Samples"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
)

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

# ==============================================================================
# Copy zstd.dll next to sample executables
# ==============================================================================
set(ZSTD_BIN_DIR "${CMAKE_SOURCE_DIR}/thirdparty/zstd/bin")

foreach(target vtx_sample_read vtx_sample_write vtx_sample_diff vtx_sample_generate vtx_sample_advance_write)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<$<CONFIG:Debug>:${ZSTD_BIN_DIR}/Debug/zstd.dll>
            $<$<CONFIG:Release>:${ZSTD_BIN_DIR}/Release/zstd.dll>
            $<$<CONFIG:RelWithDebInfo>:${ZSTD_BIN_DIR}/Release/zstd.dll>
            "$<TARGET_FILE_DIR:${target}>"
        COMMENT "Copying zstd.dll for ${target}..."
    )
endforeach()
