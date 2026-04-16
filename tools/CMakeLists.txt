# ==============================================================================
# VTX Tools — orchestrator
# ==============================================================================
project(vtx_tools)
include(FetchContent)

# ==============================================================================
# 1. Third-party GUI dependencies (shared by all GUI tools)
# ==============================================================================
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

set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(imgui glfw)

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

# ==============================================================================
# 2. Shared library + tool subtargets
# ==============================================================================
add_subdirectory(shared)

option(BUILD_VTX_INSPECTOR "Build VTX Inspector" ON)
option(BUILD_VTX_SCHEMA_CREATOR "Build Schema Creator" ON)
option(BUILD_VTX_CLI       "Build VTX CLI (headless inspector)" ON)

if(BUILD_VTX_INSPECTOR)
    add_subdirectory(inspector)
endif()

if(BUILD_VTX_SCHEMA_CREATOR)
    add_subdirectory(schema_creator)
endif()

if(BUILD_VTX_CLI)
    add_subdirectory(cli)
endif()
