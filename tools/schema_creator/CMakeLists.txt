# ==============================================================================
# vtx_schema_creator - Schema authoring and evolution tool
# ==============================================================================

file(GLOB_RECURSE VTX_SCHEMA_CREATOR_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
file(GLOB_RECURSE VTX_SCHEMA_CREATOR_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp"
)

if(WIN32)
    add_executable(vtx_schema_creator WIN32
        ${VTX_SCHEMA_CREATOR_SOURCES}
        ${VTX_SCHEMA_CREATOR_HEADERS}
        ${CMAKE_CURRENT_SOURCE_DIR}/schema_creator.rc
    )
else()
    add_executable(vtx_schema_creator
        ${VTX_SCHEMA_CREATOR_SOURCES}
        ${VTX_SCHEMA_CREATOR_HEADERS}
        ${CMAKE_CURRENT_SOURCE_DIR}/schema_creator.rc
    )
endif()

target_include_directories(vtx_schema_creator PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(vtx_schema_creator PRIVATE
    vtx_tool_shared
)

target_compile_definitions(vtx_schema_creator PRIVATE PROTOBUF_STATIC_LIBRARY)

set_target_properties(vtx_schema_creator PROPERTIES
    FOLDER "VTX Tools"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.."
)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" PREFIX "Source Files" FILES ${VTX_SCHEMA_CREATOR_SOURCES})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/include" PREFIX "Header Files" FILES ${VTX_SCHEMA_CREATOR_HEADERS})

set(ZSTD_BIN_DIR "${CMAKE_SOURCE_DIR}/thirdparty/zstd/bin")

add_custom_command(TARGET vtx_schema_creator POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<$<CONFIG:Debug>:${ZSTD_BIN_DIR}/Debug/zstd.dll>
        $<$<CONFIG:Release>:${ZSTD_BIN_DIR}/Release/zstd.dll>
        $<$<CONFIG:RelWithDebInfo>:${ZSTD_BIN_DIR}/Release/zstd.dll>
        "$<TARGET_FILE_DIR:vtx_schema_creator>"
    COMMENT "Copying zstd.dll for vtx_schema_creator..."
)

install(TARGETS vtx_schema_creator RUNTIME DESTINATION bin)
