# ==============================================================================
# vtx_tool_shared -- GUI infrastructure reused by all tools
# ==============================================================================

file(GLOB_RECURSE VTX_TOOL_SHARED_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
file(GLOB_RECURSE VTX_TOOL_SHARED_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp"
)

# vtx_tool_shared is an internal implementation detail of the GUI tools --
# never a user-facing artefact.  Pin to STATIC regardless of VTX_BUILD_SHARED so
# tools get a single self-contained executable.
add_library(vtx_tool_shared STATIC
    ${VTX_TOOL_SHARED_SOURCES}
    ${VTX_TOOL_SHARED_HEADERS}
    ${IMGUI_SOURCES}
    ${IMGUI_BACKEND_SOURCES}
)

target_include_directories(vtx_tool_shared PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${glfw_SOURCE_DIR}/include
    ${VTX_COMMON_GENERATED_DIR}
)

target_link_libraries(vtx_tool_shared PUBLIC
    vtx_common
    glfw
    $<$<PLATFORM_ID:Windows>:opengl32>
    $<$<PLATFORM_ID:Linux>:GL>
    $<$<PLATFORM_ID:Linux>:dl>
    $<$<PLATFORM_ID:Linux>:pthread>
    $<$<PLATFORM_ID:Darwin>:"-framework OpenGL">
)

target_compile_definitions(vtx_tool_shared PUBLIC
    IMGUI_IMPL_OPENGL_LOADER_GLAD
    $<$<PLATFORM_ID:Windows>:NOMINMAX>
    $<$<PLATFORM_ID:Windows>:WIN32_LEAN_AND_MEAN>
)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src"     PREFIX "Source Files" FILES ${VTX_TOOL_SHARED_SOURCES})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/include" PREFIX "Header Files" FILES ${VTX_TOOL_SHARED_HEADERS})
set_target_properties(vtx_tool_shared PROPERTIES FOLDER "VTX Tools")
