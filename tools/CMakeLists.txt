# ==============================================================================
# VTX Tools -- orchestrator
# ==============================================================================
project(vtx_tools)
include(FetchContent)

# ------------------------------------------------------------------------------
# Tool enable / disable flags
# ------------------------------------------------------------------------------
# The GUI tools currently contain Windows-only glue (INI-based settings
# persistence, GLFW native Win32 integration).  Default them ON on Windows and
# OFF on Linux/macOS; a future port would flip the defaults.
if(WIN32)
    set(VTX_GUI_TOOLS_DEFAULT ON)
else()
    set(VTX_GUI_TOOLS_DEFAULT OFF)
endif()

option(BUILD_VTX_INSPECTOR      "Build VTX Inspector (GUI)"       ${VTX_GUI_TOOLS_DEFAULT})
option(BUILD_VTX_SCHEMA_CREATOR "Build Schema Creator (GUI)"      ${VTX_GUI_TOOLS_DEFAULT})
option(BUILD_VTX_CLI            "Build VTX CLI (headless)"        ON)

# Derived flag: do we need the GUI dep fetch + shared lib at all?
if(BUILD_VTX_INSPECTOR OR BUILD_VTX_SCHEMA_CREATOR)
    set(_VTX_NEED_GUI_DEPS ON)
else()
    set(_VTX_NEED_GUI_DEPS OFF)
endif()

# ==============================================================================
# 1. Third-party GUI dependencies -- only fetched when at least one GUI tool
#    is enabled.  On Linux this prevents GLFW from pulling in X11 as a build
#    requirement when we're only building the headless CLI.
# ==============================================================================
if(_VTX_NEED_GUI_DEPS)
    FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG        v1.90.4-docking
        GIT_SHALLOW    TRUE
        GIT_SUBMODULES ""
    )

    FetchContent_Declare(
        glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG        3.3.8
        GIT_SHALLOW    TRUE
        GIT_SUBMODULES ""
    )

    set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)

    # Always build GUI dependencies static -- vtx_tool_shared bundles them.
    set(_vtx_tools_prev_shared ${BUILD_SHARED_LIBS})
    set(BUILD_SHARED_LIBS OFF)
    FetchContent_MakeAvailable(imgui glfw)
    set(BUILD_SHARED_LIBS ${_vtx_tools_prev_shared})

    # ImGui source lists (consumed by shared library)
    set(IMGUI_SOURCES
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_demo.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    )
    set(IMGUI_BACKEND_SOURCES
        ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    )
endif()

# ==============================================================================
# 2. Shared GUI helper library -- only built when a GUI tool needs it.
# ==============================================================================
if(_VTX_NEED_GUI_DEPS)
    add_subdirectory(shared)
endif()

# ==============================================================================
# 3. Tool subtargets
# ==============================================================================
if(BUILD_VTX_INSPECTOR)
    add_subdirectory(inspector)
endif()

if(BUILD_VTX_SCHEMA_CREATOR)
    add_subdirectory(schema_creator)
endif()

if(BUILD_VTX_CLI)
    add_subdirectory(cli)
endif()
