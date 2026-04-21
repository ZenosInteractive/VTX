# ==============================================================================
# vtx_schema_creator -- Schema authoring and evolution tool (GUI)
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
    )
endif()

target_include_directories(vtx_schema_creator PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(vtx_schema_creator PRIVATE
    vtx_tool_shared
)

set_target_properties(vtx_schema_creator PROPERTIES
    FOLDER "VTX Tools"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.."
)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src"     PREFIX "Source Files" FILES ${VTX_SCHEMA_CREATOR_SOURCES})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/include" PREFIX "Header Files" FILES ${VTX_SCHEMA_CREATOR_HEADERS})

vtx_copy_runtime_deps(vtx_schema_creator)

install(TARGETS vtx_schema_creator RUNTIME DESTINATION bin)
