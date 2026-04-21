# ==============================================================================
# VTX SDK -- Unit tests
# ==============================================================================
#
# Uses GoogleTest fetched at configure time.  Runs on ctest.
#
#   cmake --build build --config Release --target vtx_tests
#   ctest --test-dir build --output-on-failure -C Release
#
# Or run the executable directly from samples/ CWD:
#   build/bin/Release/vtx_tests.exe                          all tests
#   build/bin/Release/vtx_tests.exe --gtest_filter=FlatArray*  subset
#   build/bin/Release/vtx_tests.exe --gtest_list_tests          list only
# ==============================================================================

include(FetchContent)

# Pin a recent stable GoogleTest release.
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
    GIT_SHALLOW    TRUE
)

# Match MSVC runtime with the rest of the project (/MD, /MDd).
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST          OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK            OFF CACHE BOOL "" FORCE)

# Force GoogleTest to build as a static library regardless of VTX_BUILD_SHARED.
# Gtest is internal test infrastructure -- no need to ship it as a DLL/.so.
set(_vtx_prev_shared ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF)
FetchContent_MakeAvailable(googletest)
set(BUILD_SHARED_LIBS ${_vtx_prev_shared})

# Silence warnings inside GoogleTest headers on MSVC
if(MSVC)
    target_compile_options(gtest      PRIVATE /W0)
    target_compile_options(gtest_main PRIVATE /W0)
endif()

include(GoogleTest)

# ------------------------------------------------------------------------------
# Test target
# ------------------------------------------------------------------------------
add_executable(vtx_tests
    util/test_main.cpp
    util/test_fixtures.h

    common/test_flat_array.cpp
    common/test_flat_array_edges.cpp
    common/test_property_container.cpp
    common/test_bucket_frame.cpp
    common/test_vtx_game_times.cpp
    common/test_vtx_game_times_state.cpp
    common/test_schema_registry.cpp
    common/test_schema_registry_errors.cpp
    common/test_property_address_cache.cpp
    common/test_content_hash_edges.cpp
    common/test_time_utils.cpp
    common/test_frame_accessor.cpp
    common/test_vtx_game_times_extended.cpp

    writer/test_writer_basic.cpp
    writer/test_writer_edges.cpp
    writer/test_roundtrip.cpp
    writer/test_writer_advanced.cpp

    reader/test_reader_context.cpp
    reader/test_corrupt_files.cpp
    reader/test_open_replay_edges.cpp
    reader/test_reader_api.cpp

    differ/test_diff_basic.cpp
    differ/test_diff_edges.cpp
    differ/test_diff_parametrized.cpp
    differ/test_diff_utils.cpp
)

target_link_libraries(vtx_tests PRIVATE
    GTest::gtest
    vtx_common
    vtx_writer
    vtx_reader
    vtx_differ
)

target_include_directories(vtx_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${VTX_COMMON_GENERATED_DIR}"
)

target_compile_definitions(vtx_tests PRIVATE
    VTX_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    VTX_TEST_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/test_output"
)

set_target_properties(vtx_tests PROPERTIES
    FOLDER "Tests"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
)

# Copy runtime DLLs on Windows; no-op on Linux.
vtx_copy_runtime_deps(vtx_tests)

# Ensure the test_output directory exists at build time.
add_custom_command(TARGET vtx_tests PRE_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/test_output"
)

# IDE grouping
source_group("util"    REGULAR_EXPRESSION "util/.*")
source_group("common"  REGULAR_EXPRESSION "common/.*")
source_group("writer"  REGULAR_EXPRESSION "writer/.*")
source_group("reader"  REGULAR_EXPRESSION "reader/.*")
source_group("differ"  REGULAR_EXPRESSION "differ/.*")

# Register every TEST(suite, name) with ctest
gtest_discover_tests(vtx_tests
    PROPERTIES
        LABELS "vtx"
        TIMEOUT 60
)

if(TARGET vtx_sample_diff)
    add_test(
        NAME vtx_sample_diff_smoke
        COMMAND $<TARGET_FILE:vtx_sample_diff>
                "${CMAKE_SOURCE_DIR}/samples/content/reader/arena/arena_from_fbs_ds.vtx"
                --fail-on-empty
    )
    set_tests_properties(vtx_sample_diff_smoke PROPERTIES
        LABELS "vtx;samples"
        TIMEOUT 60
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/samples"
    )
endif()
