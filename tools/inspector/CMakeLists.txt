# ==============================================================================
# vtx_inspector -- VTX file inspection tool (GUI)
# ==============================================================================

file(GLOB_RECURSE VTX_INSPECTOR_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
file(GLOB_RECURSE VTX_INSPECTOR_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp"
)

set(VTX_INSPECTOR_RC_SOURCES "")
if(WIN32)
    set(VTX_INSPECTOR_RC_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/inspector.rc")
    add_executable(vtx_inspector WIN32
        ${VTX_INSPECTOR_SOURCES}
        ${VTX_INSPECTOR_HEADERS}
        ${VTX_INSPECTOR_RC_SOURCES}
    )
else()
    add_executable(vtx_inspector
        ${VTX_INSPECTOR_SOURCES}
        ${VTX_INSPECTOR_HEADERS}
    )
endif()

target_include_directories(vtx_inspector PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(vtx_inspector PRIVATE
    vtx_tool_shared
    vtx_reader
)

set_target_properties(vtx_inspector PROPERTIES
    FOLDER "VTX Tools"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.."
)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src"     PREFIX "Source Files" FILES ${VTX_INSPECTOR_SOURCES})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/include" PREFIX "Header Files" FILES ${VTX_INSPECTOR_HEADERS})

vtx_copy_runtime_deps(vtx_inspector)

install(TARGETS vtx_inspector RUNTIME DESTINATION bin)
