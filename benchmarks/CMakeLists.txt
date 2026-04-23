# ==============================================================================
# VTX SDK -- Performance benchmarks (google/benchmark).
# ==============================================================================
#
# Enabled via `-DVTX_BUILD_BENCHMARKS=ON` (default OFF).  Builds a single
# executable `vtx_benchmarks` that runs every BENCHMARK() registered across
# the .cpp files listed below.
#
#   cmake -S . -B build -DVTX_BUILD_BENCHMARKS=ON -DCMAKE_BUILD_TYPE=Release
#   cmake --build build --target vtx_benchmarks
#   ./build/benchmarks/vtx_benchmarks
#
# Output is standard google/benchmark: ns/iter, items/sec, bytes/sec, plus
# auto-iterated warmup and statistical smoothing.  For JSON output (CI
# artifacts):
#
#   ./vtx_benchmarks --benchmark_out=results.json --benchmark_out_format=json
# ==============================================================================

include(FetchContent)

# Pin a recent stable google/benchmark release.
FetchContent_Declare(
    benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG        v1.9.0
    GIT_SHALLOW    TRUE
)

# Turn off google/benchmark's own tests and install rules -- we only want
# the library target.
set(BENCHMARK_ENABLE_TESTING     OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL     OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)

# Force static so we don't drag google/benchmark's runtime around.
set(_vtx_prev_shared_bench ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF)
FetchContent_MakeAvailable(benchmark)
set(BUILD_SHARED_LIBS ${_vtx_prev_shared_bench})

if(TARGET benchmark)
    set_target_properties(benchmark PROPERTIES FOLDER "Thirdparty/benchmark")
endif()
if(TARGET benchmark_main)
    set_target_properties(benchmark_main PROPERTIES FOLDER "Thirdparty/benchmark")
endif()

# ==============================================================================
# Benchmark executable
# ==============================================================================
# Each BENCHMARK() in these files is auto-registered by google/benchmark at
# static init.  `benchmark::benchmark_main` provides the main() that calls
# Initialize + RunSpecifiedBenchmarks + Shutdown.
add_executable(vtx_benchmarks
    bench_reader.cpp
    bench_writer.cpp
    bench_differ.cpp
    bench_property_cache.cpp
    bench_accessor.cpp
    bench_accessor_key_resolution.cpp
    bench_cs.cpp
    bench_rl.cpp
    bench_scenarios.cpp
    bench_schema.cpp
)

target_link_libraries(vtx_benchmarks PRIVATE
    benchmark::benchmark
    benchmark::benchmark_main
    vtx_common
    vtx_reader
    vtx_writer
    vtx_differ
)

target_include_directories(vtx_benchmarks PRIVATE
    # vtx_common keeps protobuf / flatbuffers headers PRIVATE; benchmarks
    # that touch PropertyContainer internals need the same thirdparty paths.
    "${CMAKE_SOURCE_DIR}/thirdparty/protobuf/include"
)

target_compile_definitions(vtx_benchmarks PRIVATE
    VTX_BENCH_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)

set_target_properties(vtx_benchmarks PROPERTIES
    FOLDER "Benchmarks"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
)

# ==============================================================================
# Fixture generation
# ==============================================================================
# Benchmark scenarios need realistic-size .vtx files.  We generate them at
# build time via vtx_sample_write instead of committing binary blobs.  One
# fixture is enough for the initial scenarios; more sizes can be added later.
#
# The fixture depends on vtx_sample_write, which must be built before this
# add_custom_command fires.
# ==============================================================================
set(VTX_BENCH_FIXTURES_DIR_ABS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
set(VTX_BENCH_ARENA_SCHEMA     "${CMAKE_SOURCE_DIR}/samples/content/writer/arena/arena_schema.json")
set(VTX_BENCH_FIXTURE_10K      "${VTX_BENCH_FIXTURES_DIR_ABS}/synth_10k.vtx")

# Ensure the fixtures directory exists before custom commands run.
file(MAKE_DIRECTORY "${VTX_BENCH_FIXTURES_DIR_ABS}")

add_custom_command(
    OUTPUT  "${VTX_BENCH_FIXTURE_10K}"
    COMMAND $<TARGET_FILE:vtx_sample_write>
            "${VTX_BENCH_ARENA_SCHEMA}"
            "${VTX_BENCH_FIXTURE_10K}"
            10000
    DEPENDS vtx_sample_write "${VTX_BENCH_ARENA_SCHEMA}"
    WORKING_DIRECTORY "${VTX_BENCH_FIXTURES_DIR_ABS}"
    COMMENT "Generating 10k-frame benchmark fixture (synth_10k.vtx)"
    VERBATIM
)

add_custom_target(vtx_bench_fixtures ALL
    DEPENDS "${VTX_BENCH_FIXTURE_10K}"
)
set_target_properties(vtx_bench_fixtures PROPERTIES FOLDER "Benchmarks")

# Make the benchmark binary depend on the fixture being generated.
add_dependencies(vtx_benchmarks vtx_bench_fixtures)

source_group("Benchmarks" REGULAR_EXPRESSION "bench_.*\\.cpp")
