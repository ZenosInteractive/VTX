# ==============================================================================
# VtxDependencies.cmake -- central third-party dependency resolution.
# ==============================================================================
#
# Public outputs:
#
#   VTX::deps::protobuf
#   VTX::deps::flatbuffers
#   VTX::deps::zstd
#
#   VTX_PROTOC_EXE
#   VTX_FLATC_EXE
#
#   VTX_DEPENDENCY_PROVIDER
#   VTX_NEEDS_PROTOBUF_STATIC_DEFINE
#
# Behaviour:
#
#   - Header-only deps (`nlohmann/json`, `xxHash`) always come from `thirdparty/`.
#   - On Windows, dependency source is configurable:
#       * `PACKAGE_MANAGER`: require tools + libraries from vcpkg / preinstalled packages
#       * `BUNDLED`:         require the legacy prebuilt binaries under `thirdparty/`
#       * `AUTO`:            try package-manager first, then bundled fallback
#   - On Linux/macOS, resolve through system packages / package managers.
# ==============================================================================

set(VTX_THIRDPARTY "${CMAKE_SOURCE_DIR}/thirdparty"
    CACHE PATH "Path to the legacy bundled third-party dependency tree")

set(VTX_DEPENDENCY_SOURCE "AUTO" CACHE STRING
    "How VTX resolves non-header-only dependencies on Windows: AUTO, PACKAGE_MANAGER, or BUNDLED")
set_property(CACHE VTX_DEPENDENCY_SOURCE PROPERTY STRINGS AUTO PACKAGE_MANAGER BUNDLED)

# Header-only deps are portable and intentionally kept bundled.
set(VTX_JSON_INCLUDE_DIR   "${VTX_THIRDPARTY}/jsonlohmann/include"
    CACHE PATH "nlohmann/json include directory")
set(VTX_XXHASH_INCLUDE_DIR "${VTX_THIRDPARTY}/xxhash"
    CACHE PATH "xxHash include directory")

set(VTX_DEPENDENCY_PROVIDER "uninitialized" CACHE INTERNAL "Dependency provider selected for this configure run")
set(VTX_NEEDS_PROTOBUF_STATIC_DEFINE OFF CACHE INTERNAL "Whether MSVC builds must define PROTOBUF_STATIC_LIBRARY")

# ==============================================================================
# FlatBuffers -- unified FetchContent resolution across all platforms.
# ==============================================================================
# FlatBuffers is effectively header-only at runtime.  We FetchContent the
# upstream source, build only the `flatc` compiler, and consume the headers
# through an INTERFACE target.  One pinned version everywhere eliminates the
# 2.x-vs-25.x mismatches that bit us when the bundled Windows binaries (25.x)
# leaked into Linux builds where apt ships libflatbuffers-dev 2.x.
#
# Build impact: +30-60s on first configure (per platform) while flatc builds
# from source.  Subsequent configures hit the FetchContent cache (zero cost).
# CI already caches build/_deps so this is a one-time cost per cache key.
# ==============================================================================
include(FetchContent)

FetchContent_Declare(
    flatbuffers_src
    GIT_REPOSITORY https://github.com/google/flatbuffers.git
    GIT_TAG        v24.12.23
    GIT_SHALLOW    TRUE
)

set(FLATBUFFERS_BUILD_TESTS     OFF CACHE BOOL "" FORCE)
set(FLATBUFFERS_INSTALL         OFF CACHE BOOL "" FORCE)
set(FLATBUFFERS_BUILD_FLATC     ON  CACHE BOOL "" FORCE)
set(FLATBUFFERS_BUILD_FLATLIB   OFF CACHE BOOL "" FORCE)
set(FLATBUFFERS_BUILD_SHAREDLIB OFF CACHE BOOL "" FORCE)
set(FLATBUFFERS_BUILD_FLATHASH  OFF CACHE BOOL "" FORCE)

# Force static so we don't drag flatc's CRT around.
set(_vtx_prev_shared ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF)
FetchContent_MakeAvailable(flatbuffers_src)
set(BUILD_SHARED_LIBS ${_vtx_prev_shared})

# Hide flatc + its internal targets from IDE folder views.
if(TARGET flatc)
    set_target_properties(flatc PROPERTIES FOLDER "Thirdparty/flatbuffers")
endif()

# Public INTERFACE target -- headers only, no library link.
add_library(VTX_deps_flatbuffers INTERFACE)
target_include_directories(VTX_deps_flatbuffers SYSTEM INTERFACE
    $<BUILD_INTERFACE:${flatbuffers_src_SOURCE_DIR}/include>
)
add_library(VTX::deps::flatbuffers ALIAS VTX_deps_flatbuffers)

# flatc compiler: use the FetchContent-built target.  Generator expression
# resolves to the full path at build time; add_custom_command handles it
# natively and tracks the dependency on the flatc target.
set(VTX_FLATC_EXE $<TARGET_FILE:flatc>)

# ==============================================================================
# zstd -- unified FetchContent resolution across all platforms.
# ==============================================================================
# Same pattern as FlatBuffers.  Build zstd's static library from source, link
# it into the VTX modules, and ship nothing at runtime.  No DLLs to copy, no
# bundled binaries in the repo, no `libzstd-dev` apt requirement.
#
# zstd's CMakeLists.txt lives under build/cmake/ rather than the repo root,
# so we do the Populate + add_subdirectory dance manually (works on CMake
# 3.15+ without needing SOURCE_SUBDIR which was added in 3.18).
# ==============================================================================
FetchContent_Declare(
    zstd_src
    GIT_REPOSITORY https://github.com/facebook/zstd.git
    GIT_TAG        v1.5.6
    GIT_SHALLOW    TRUE
)

set(ZSTD_BUILD_PROGRAMS      OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS         OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_CONTRIB       OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC        ON  CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED        OFF CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT      OFF CACHE BOOL "" FORCE)
set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "" FORCE)

set(_vtx_prev_shared_zstd ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF)
# CMake 3.30+ deprecates manual FetchContent_Populate; the replacement --
# FetchContent_MakeAvailable with SOURCE_SUBDIR -- requires CMake 3.18.
# Since we still declare a minimum of 3.15 (CMakeLists.txt), we keep the
# manual dance behind CMP0169=OLD to suppress the deprecation warning.
# When the minimum is bumped, this block collapses to SOURCE_SUBDIR +
# FetchContent_MakeAvailable.
if(POLICY CMP0169)
    cmake_policy(PUSH)
    cmake_policy(SET CMP0169 OLD)
endif()
FetchContent_GetProperties(zstd_src)
if(NOT zstd_src_POPULATED)
    FetchContent_Populate(zstd_src)
    add_subdirectory(
        ${zstd_src_SOURCE_DIR}/build/cmake
        ${zstd_src_BINARY_DIR}
        EXCLUDE_FROM_ALL
    )
endif()
if(POLICY CMP0169)
    cmake_policy(POP)
endif()
set(BUILD_SHARED_LIBS ${_vtx_prev_shared_zstd})

# Hide zstd's internal targets from IDE folder views.
foreach(_t libzstd_static zstd_static clean-all uninstall)
    if(TARGET ${_t})
        set_target_properties(${_t} PROPERTIES FOLDER "Thirdparty/zstd")
    endif()
endforeach()

# Public target -- wrap the static library so consumers see a stable name
# regardless of whether upstream zstd tweaks its target names across releases.
add_library(VTX_deps_zstd INTERFACE)
target_link_libraries(VTX_deps_zstd INTERFACE libzstd_static)
target_include_directories(VTX_deps_zstd SYSTEM INTERFACE
    $<BUILD_INTERFACE:${zstd_src_SOURCE_DIR}/lib>
)
add_library(VTX::deps::zstd ALIAS VTX_deps_zstd)

# Export libzstd_static so find_package(VTX) consumers get VTX::libzstd_static
# automatically (referenced via $<INSTALL_INTERFACE:...> in vtx_common).
install(TARGETS libzstd_static
        EXPORT VTXTargets
        ARCHIVE DESTINATION lib)

function(_vtx_use_bundled_windows_deps)
    if(NOT DEFINED VTX_PROTOC_EXE OR VTX_PROTOC_EXE STREQUAL "")
        set(VTX_PROTOC_EXE "${VTX_THIRDPARTY}/protobuf/bin/protoc.exe"
            CACHE FILEPATH "Path to the protoc binary" FORCE)
    endif()
    if(NOT EXISTS "${VTX_PROTOC_EXE}")
        message(FATAL_ERROR
            "Bundled dependency mode requested, but protoc was not found at "
            "${VTX_PROTOC_EXE}. Either install dependencies via vcpkg and use "
            "-DVTX_DEPENDENCY_SOURCE=PACKAGE_MANAGER, or restore the legacy "
            "bundle under thirdparty/protobuf.")
    endif()

    # flatc + flatbuffers headers are supplied by FetchContent at the top of
    # this file; nothing to resolve here.  We intentionally ignore
    # thirdparty/flatbuffers/* on all platforms to keep a single source of
    # truth for flatc + headers.

    set(_vtx_non_debug_map
        MAP_IMPORTED_CONFIG_RELEASE         Release
        MAP_IMPORTED_CONFIG_RELWITHDEBINFO  Release
        MAP_IMPORTED_CONFIG_MINSIZEREL      Release
        MAP_IMPORTED_CONFIG_DEBUG           Debug
    )

    add_library(VTX_deps_protobuf STATIC IMPORTED)
    set_target_properties(VTX_deps_protobuf PROPERTIES
        IMPORTED_CONFIGURATIONS       "Release;Debug"
        IMPORTED_LOCATION_RELEASE     "${VTX_THIRDPARTY}/protobuf/lib/libprotobuf.lib"
        IMPORTED_LOCATION_DEBUG       "${VTX_THIRDPARTY}/protobuf/lib/libprotobufd.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${VTX_THIRDPARTY}/protobuf/include"
        ${_vtx_non_debug_map}
    )
    add_library(VTX::deps::protobuf ALIAS VTX_deps_protobuf)

    # zstd is supplied unconditionally by the FetchContent block at the top
    # of this file -- no bundled binaries, no DLL copy, nothing to do here.

    set(VTX_DEPENDENCY_PROVIDER "bundled-thirdparty" CACHE INTERNAL "" FORCE)
    set(VTX_NEEDS_PROTOBUF_STATIC_DEFINE ON CACHE INTERNAL "" FORCE)
endfunction()

function(_vtx_try_package_manager_windows out_success out_missing)
    set(_missing "")

    find_program(_vtx_protoc_exe NAMES protoc DOC "Path to the protoc binary")
    if(NOT _vtx_protoc_exe)
        list(APPEND _missing "protoc")
    endif()

    find_package(Protobuf CONFIG QUIET)
    if(NOT TARGET protobuf::libprotobuf)
        list(APPEND _missing "protobuf::libprotobuf")
    endif()

    # zstd is supplied by the FetchContent block at the top of this file --
    # we don't probe for a system/vcpkg copy.  One pinned version everywhere.

    if(_missing)
        string(JOIN ", " _missing_joined ${_missing})
        set(${out_success} FALSE PARENT_SCOPE)
        set(${out_missing} "${_missing_joined}" PARENT_SCOPE)
        return()
    endif()

    add_library(VTX::deps::protobuf ALIAS protobuf::libprotobuf)

    set(VTX_PROTOC_EXE "${_vtx_protoc_exe}" CACHE FILEPATH "Path to the protoc binary" FORCE)
    # VTX_FLATC_EXE + VTX::deps::flatbuffers + VTX::deps::zstd come from the
    # FetchContent blocks at the top of this file -- no per-platform probing.
    set(VTX_DEPENDENCY_PROVIDER "package-manager" CACHE INTERNAL "" FORCE)
    set(VTX_NEEDS_PROTOBUF_STATIC_DEFINE OFF CACHE INTERNAL "" FORCE)

    set(${out_success} TRUE PARENT_SCOPE)
    set(${out_missing} "" PARENT_SCOPE)
endfunction()

if(WIN32)
    string(TOUPPER "${VTX_DEPENDENCY_SOURCE}" _vtx_dependency_source)
    if(NOT _vtx_dependency_source MATCHES "^(AUTO|PACKAGE_MANAGER|BUNDLED)$")
        message(FATAL_ERROR
            "Invalid VTX_DEPENDENCY_SOURCE='${VTX_DEPENDENCY_SOURCE}'. "
            "Expected AUTO, PACKAGE_MANAGER, or BUNDLED.")
    endif()

    if(_vtx_dependency_source STREQUAL "PACKAGE_MANAGER")
        _vtx_try_package_manager_windows(_vtx_found_pkg_mgr _vtx_missing)
        if(NOT _vtx_found_pkg_mgr)
            message(FATAL_ERROR
                "VTX_DEPENDENCY_SOURCE=PACKAGE_MANAGER was requested, but the "
                "following dependencies were not found: ${_vtx_missing}. "
                "Use the provided vcpkg.json manifest, install the packages "
                "manually, or switch to -DVTX_DEPENDENCY_SOURCE=BUNDLED.")
        endif()
    elseif(_vtx_dependency_source STREQUAL "BUNDLED")
        _vtx_use_bundled_windows_deps()
    else()
        _vtx_try_package_manager_windows(_vtx_found_pkg_mgr _vtx_missing)
        if(NOT _vtx_found_pkg_mgr)
            message(STATUS
                "VTX deps: package-manager resolution incomplete on Windows "
                "(${_vtx_missing}). Falling back to legacy thirdparty/ bundle.")
            _vtx_use_bundled_windows_deps()
        endif()
    endif()
else()
    find_program(VTX_PROTOC_EXE protoc
        DOC "Path to the protoc binary"
        REQUIRED)

    # flatc + flatbuffers headers come from the FetchContent block at the
    # top of this file.  No system-flatbuffers probing here.

    # Try config mode first (preferred -- provides protobuf::libprotobuf
    # target), then fall back to the FindProtobuf.cmake module.  The first
    # call must NOT be REQUIRED -- otherwise a distro without protobuf's
    # cmake config files (Ubuntu's libprotobuf-dev doesn't ship them on
    # 22.04 with apt's packaging) aborts the build before the fallback
    # even runs.
    find_package(Protobuf CONFIG QUIET)
    if(NOT Protobuf_FOUND)
        find_package(Protobuf REQUIRED MODULE)
    endif()
    if(TARGET protobuf::libprotobuf)
        add_library(VTX::deps::protobuf ALIAS protobuf::libprotobuf)
    else()
        add_library(VTX_deps_protobuf INTERFACE)
        target_include_directories(VTX_deps_protobuf SYSTEM INTERFACE ${Protobuf_INCLUDE_DIRS})
        target_link_libraries(VTX_deps_protobuf INTERFACE ${Protobuf_LIBRARIES})
        add_library(VTX::deps::protobuf ALIAS VTX_deps_protobuf)
    endif()

    # zstd is supplied by the FetchContent block at the top of this file --
    # no apt / pkg-config / find_library probing, no libzstd-dev requirement.

    set(VTX_DEPENDENCY_PROVIDER "system-package-manager" CACHE INTERNAL "" FORCE)
    set(VTX_NEEDS_PROTOBUF_STATIC_DEFINE OFF CACHE INTERNAL "" FORCE)
endif()

message(STATUS "VTX deps provider: ${VTX_DEPENDENCY_PROVIDER}")
message(STATUS "VTX deps: protoc=${VTX_PROTOC_EXE}")
message(STATUS "VTX deps: flatc=<built from FetchContent source ${flatbuffers_src_SOURCE_DIR}>")

function(vtx_copy_runtime_deps target)
    # Kept as a no-op stub so existing call sites keep working.  zstd is now
    # linked statically from the FetchContent build, flatc has no runtime
    # needs, and protobuf is either statically bundled or shipped through
    # vcpkg's own deployment pipeline.  If we ever add a dependency that
    # genuinely needs a runtime DLL copy, hang it off this hook.
    if(NOT TARGET ${target})
        message(WARNING "vtx_copy_runtime_deps: target '${target}' does not exist")
    endif()
endfunction()
