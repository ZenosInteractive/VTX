# ==============================================================================
# VtxVersionResource.cmake -- Windows version information for VTX executables.
# ==============================================================================
#
#   vtx_add_version_resource(<target> DESCRIPTION <text>)
#
# Generates a VERSIONINFO resource for <target> from cmake/vtx_version.rc.in
# and adds it to the target's sources: company, product, file description,
# copyright, original file name, file + product version.  This is what
# Explorer shows under Properties > Details and Task Manager shows as the
# process name, and antivirus heuristics treat executables without it as more
# suspicious.
#
# The version comes from project(VTX_SDK VERSION ...) in the root
# CMakeLists.txt -- the value release.yml checks the tag against -- so it can
# never drift from the release.  VTX_SDK_VERSION is used rather than
# PROJECT_VERSION because nested project() calls (tools/) reset the latter.
#
# No-op off Windows.  The icon .rc files next to each tool stay separate; the
# linker merges both resources into the executable.
# ==============================================================================

function(vtx_add_version_resource target)
    if(NOT WIN32)
        return()
    endif()

    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "DESCRIPTION" "")
    if(NOT ARG_DESCRIPTION)
        message(FATAL_ERROR "vtx_add_version_resource(${target}): DESCRIPTION is required")
    endif()

    set(VTX_RC_COMPANY     "Zenos Interactive Limited")
    set(VTX_RC_PRODUCT     "VTX SDK")
    set(VTX_RC_COPYRIGHT   "Copyright (C) 2026 Zenos Interactive Limited")
    set(VTX_RC_DESCRIPTION "${ARG_DESCRIPTION}")
    set(VTX_RC_TARGET      "${target}")
    set(VTX_RC_VERSION     "${VTX_SDK_VERSION}")

    # FILEVERSION / PRODUCTVERSION take four comma-separated numbers.
    set(_parts "")
    foreach(_component MAJOR MINOR PATCH TWEAK)
        if("${VTX_SDK_VERSION_${_component}}" STREQUAL "")
            list(APPEND _parts 0)
        else()
            list(APPEND _parts ${VTX_SDK_VERSION_${_component}})
        endif()
    endforeach()
    string(REPLACE ";" "," VTX_RC_VERSION_COMMAS "${_parts}")

    set(_rc "${CMAKE_CURRENT_BINARY_DIR}/${target}_version.rc")
    configure_file("${VTX_SDK_SOURCE_DIR}/cmake/vtx_version.rc.in" "${_rc}" @ONLY)
    target_sources(${target} PRIVATE "${_rc}")
    source_group("Resource Files" FILES "${_rc}")
endfunction()
