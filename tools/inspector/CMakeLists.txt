# ==============================================================================
# vtx_inspector — VTX file inspection tool
# ==============================================================================

file(GLOB_RECURSE VTX_INSPECTOR_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
file(GLOB_RECURSE VTX_INSPECTOR_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp"
)

if(WIN32)
    add_executable(vtx_inspector WIN32
        ${VTX_INSPECTOR_SOURCES}
        ${VTX_INSPECTOR_HEADERS}
        ${CMAKE_CURRENT_SOURCE_DIR}/inspector.rc
    )
else()
    add_executable(vtx_inspector
        ${VTX_INSPECTOR_SOURCES}
        ${VTX_INSPECTOR_HEADERS}
        ${CMAKE_CURRENT_SOURCE_DIR}/inspector.rc
    )
endif()

target_include_directories(vtx_inspector PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(vtx_inspector PRIVATE
    vtx_tool_shared
    vtx_reader
)

target_compile_definitions(vtx_inspector PRIVATE PROTOBUF_STATIC_LIBRARY)

set_target_properties(vtx_inspector PROPERTIES
    FOLDER "VTX Tools"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.."
)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" PREFIX "Source Files" FILES ${VTX_INSPECTOR_SOURCES})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/include" PREFIX "Header Files" FILES ${VTX_INSPECTOR_HEADERS})

set(ZSTD_BIN_DIR "${CMAKE_SOURCE_DIR}/thirdparty/zstd/bin")

add_custom_command(TARGET vtx_inspector POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<$<CONFIG:Debug>:${ZSTD_BIN_DIR}/Debug/zstd.dll>
        $<$<CONFIG:Release>:${ZSTD_BIN_DIR}/Release/zstd.dll>
        $<$<CONFIG:RelWithDebInfo>:${ZSTD_BIN_DIR}/Release/zstd.dll>
        "$<TARGET_FILE_DIR:vtx_inspector>"
    COMMENT "Copying zstd.dll for vtx_inspector..."
)

install(TARGETS vtx_inspector RUNTIME DESTINATION bin)
