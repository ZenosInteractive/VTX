# ==============================================================================
# VTX SDK — Samples
# ==============================================================================

set(SAMPLE_CONTENT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/content")

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

# Copy zstd.dll next to sample executables
set(ZSTD_BIN_DIR "${CMAKE_SOURCE_DIR}/thirdparty/zstd/bin")

foreach(target vtx_sample_read vtx_sample_write vtx_sample_diff)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<$<CONFIG:Debug>:${ZSTD_BIN_DIR}/Debug/zstd.dll>
            $<$<CONFIG:Release>:${ZSTD_BIN_DIR}/Release/zstd.dll>
            $<$<CONFIG:RelWithDebInfo>:${ZSTD_BIN_DIR}/Release/zstd.dll>
            "$<TARGET_FILE_DIR:${target}>"
        COMMENT "Copying zstd.dll for ${target}..."
    )
endforeach()
