# ==============================================================================
# vtx_cli -- headless VTX file inspector (no GUI, stdin/stdout JSON protocol)
# ==============================================================================

file(GLOB_RECURSE VTX_CLI_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
file(GLOB_RECURSE VTX_CLI_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp"
)

# Windows resource file (version info / icon) -- only consumed by the Windows RC compiler.
set(VTX_CLI_RC_SOURCES "")
if(WIN32)
    set(VTX_CLI_RC_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/cli.rc")
endif()

add_executable(vtx_cli
    ${VTX_CLI_SOURCES}
    ${VTX_CLI_HEADERS}
    ${VTX_CLI_RC_SOURCES}
)

# Third-party headers flow in through vtx_reader / vtx_differ -> vtx_common.
# Only add the private paths that aren't already exposed publicly.
target_include_directories(vtx_cli PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${VTX_COMMON_GENERATED_DIR}"
)

target_link_libraries(vtx_cli PRIVATE
    vtx_reader
    vtx_differ
    $<$<PLATFORM_ID:Windows>:ws2_32>
)

set_target_properties(vtx_cli PROPERTIES
    FOLDER "VTX Tools"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.."
)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src"     PREFIX "Source Files" FILES ${VTX_CLI_SOURCES})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/include" PREFIX "Header Files" FILES ${VTX_CLI_HEADERS})

# Copy runtime DLLs on Windows, no-op on Linux.
vtx_copy_runtime_deps(vtx_cli)

install(TARGETS vtx_cli RUNTIME DESTINATION bin)
