# ==============================================================================
# vtx_cli — headless VTX file inspector (no GUI, stdin/stdout JSON protocol)
# ==============================================================================

file(GLOB_RECURSE VTX_CLI_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
file(GLOB_RECURSE VTX_CLI_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp"
)

add_executable(vtx_cli
    ${VTX_CLI_SOURCES}
    ${VTX_CLI_HEADERS}
    ${CMAKE_CURRENT_SOURCE_DIR}/cli.rc
)

target_include_directories(vtx_cli PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/thirdparty/flatbuffers/include
    ${CMAKE_SOURCE_DIR}/thirdparty/protobuf/include
    ${CMAKE_SOURCE_DIR}/sdk/src/vtx_common/src/generated
)

target_link_libraries(vtx_cli PRIVATE
    vtx_reader
    vtx_differ
    $<$<PLATFORM_ID:Windows>:ws2_32>
)

target_compile_definitions(vtx_cli PRIVATE PROTOBUF_STATIC_LIBRARY)

set_target_properties(vtx_cli PROPERTIES
    FOLDER "VTX Tools"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.."
)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" PREFIX "Source Files" FILES ${VTX_CLI_SOURCES})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/include" PREFIX "Header Files" FILES ${VTX_CLI_HEADERS})

set(ZSTD_BIN_DIR "${CMAKE_SOURCE_DIR}/thirdparty/zstd/bin")

add_custom_command(TARGET vtx_cli POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<$<CONFIG:Debug>:${ZSTD_BIN_DIR}/Debug/zstd.dll>
        $<$<CONFIG:Release>:${ZSTD_BIN_DIR}/Release/zstd.dll>
        $<$<CONFIG:RelWithDebInfo>:${ZSTD_BIN_DIR}/Release/zstd.dll>
        "$<TARGET_FILE_DIR:vtx_cli>"
    COMMENT "Copying zstd.dll for vtx_cli..."
)

install(TARGETS vtx_cli RUNTIME DESTINATION bin)
