# VTX SDK — Test Suite

GoogleTest-based unit suite. Covers the main public surface of `vtx_common`, `vtx_writer`, `vtx_reader`, and `vtx_differ`, plus targeted regression tests for bugs called out in the CHANGELOG.

## Quick reference

```bat
REM Configure with tests enabled (default ON).
cmake -S . -B build -A x64

REM Build the test exe.
cmake --build build --config Release --target vtx_tests

REM Run every test via ctest.
ctest --test-dir build --output-on-failure -C Release

REM Or invoke the executable directly (zstd is linked statically -- no DLL dep).
build\bin\Release\vtx_tests.exe
build\bin\Release\vtx_tests.exe --gtest_filter=VTXGameTimes.*
build\bin\Release\vtx_tests.exe --gtest_list_tests
```

`VTX_BUILD_TESTS` is `ON` by default. Flip it `OFF` for a no-deps SDK-only build (skips fetching GoogleTest via `FetchContent`):

```bat
cmake -S . -B build -A x64 -DVTX_BUILD_TESTS=OFF
```

## Layout

```
tests/
  CMakeLists.txt              FetchContent GoogleTest + target + gtest_discover_tests
  README.md                   (this file)

  util/
    test_main.cpp             shared main -- installs a silent logger sink
    test_fixtures.h           FixturePath / OutputPath / MakeSimpleEntity helpers

  common/
    test_flat_array.cpp               FlatArray<T> -- append, insert, erase,
                                       replace, PushBack auto-grow, OOB behaviour
    test_property_container.cpp       content_hash determinism + sensitivity
    test_bucket_frame.cpp             CreateBucket idempotency, GetEntitiesOfType
    test_frame_accessor.cpp           FrameAccessor + EntityView scalar, array,
                                       nested-struct and invalid-key behaviour
    test_time_utils.cpp               PreparePropertyContainer, hash coverage
                                       for flat-array offsets, UTC/ticks helpers
    test_vtx_game_times.cpp           REGRESSION TESTS for the VTXGameTimes bugs
                                       fixed on 2026-04-17 (historical UTC, game_time=0
                                       on first frame, destructor seeding start_utc_)
    test_vtx_game_times_extended.cpp  Setup/fake-time branches, gap detection,
                                       segment detection, snapshot rollback,
                                       chunk-copy helpers
    test_schema_registry.cpp          LoadFromJson / LoadFromRawString / GetField
    test_property_address_cache.cpp   cache population, property order, lookup keys

  writer/
    test_writer_basic.cpp             FBS + Proto factories, record + stop smoke tests
    test_roundtrip.cpp                WRITE -> READ -> ASSERT.  Both backends.
                                       Frame-index-dependent payloads verified at
                                       frames [0, N/2, N-1].  Historical-UTC run too.
    test_writer_advanced.cpp          chunking boundaries, type sorting,
                                       empty replay, UUID defaulting, write failures

  reader/
    test_reader_context.cpp           OpenReplayFile happy + failure paths,
                                       destruction-order regression, metadata readback.
    test_reader_api.cpp               accessor API, frame range/context,
                                       OOB reads, cache-window eviction

  differ/
    test_diff_basic.cpp               Factory null-for-Unknown, identical = 0 ops,
                                       Replace on float/vector change,
                                       Add/Remove on entity churn, float epsilon.
    test_diff_parametrized.cpp        Real DiffRawFrames coverage for both
                                       FlatBuffers and Protobuf backends
    test_diff_utils.cpp               Path helpers, field-name helpers,
                                       epsilon comparisons, array classification

  fixtures/
    test_schema.json           arena schema (copy of samples/content/.../arena_schema.json)

  (build dir)
    test_output/               transient .vtx files produced during tests
```

## Which tests pin today's bug fixes?

These tests would have caught the 7 bugs fixed on 2026-04-17:

| Bug (from CHANGELOG) | Test that pins it |
|---|---|
| `AddTimeRegistry` rejected historical UTC on first frame | `VTXGameTimes.AcceptsHistoricalUtcOnFirstFrame_Regression` |
| `VTXGameTimes()` seeded `start_utc_` with `now()` | `VTXGameTimes.DefaultConstructedStartUtcIsZero_Regression` |
| `OnlyIncreasing` filter rejected `game_time=0` on frame 0 | `VTXGameTimes.OnlyIncreasingAcceptsGameTimeZeroOnFirstFrame_Regression` |
| `OnlyDecreasing` filter rejected `game_time=0` on frame 0 | `VTXGameTimes.OnlyDecreasingAcceptsGameTimeZeroOnFirstFrame_Regression` |
| printf specifiers in `std::format` warnings | Implicit via `AcceptsManyMonotonicFrames` / roundtrip tests producing clean logs |
| `ReaderContext` member destruction order (UAF risk) | `ReaderContextHappy.DestroysCleanlyAfterFrameAccess` |
| `PerformHeavyLoading` swallowed exception silently | Implicit via `ReaderContextFailure.GarbageFileReturnsError` |

## gtest patterns used in this suite

Three patterns, one per use case:

### `TEST(Suite, Name)` — plain function-style

The default. Most files use this — tests are short, self-contained, no shared setup worth extracting.

```cpp
TEST(FlatArray, DefaultConstructsEmpty) {
    VTX::FlatArray<int> arr;
    EXPECT_EQ(arr.SubArrayCount(), 0u);
}
```

### `TEST_F(FixtureClass, Name)` — class-based fixture

Used when multiple tests share non-trivial setup (opening a file, building a scene). The class inherits from `::testing::Test`, runs `SetUp()` before each `TEST_F`, and exposes `protected:` members to the test body.

Example: `reader/test_reader_context.cpp` has a `ReaderContextHappy` fixture that writes a tiny `.vtx` and opens a `ReaderContext` before each test. Seven tests share that setup. The two failure-path tests (`NonexistentFileReturnsError`, `GarbageFileReturnsError`) use plain `TEST` because the happy-path setup would actively get in the way.

### `TEST_P(FixtureClass, Name)` + `INSTANTIATE_TEST_SUITE_P` — parametrized

Used to run the same test body against multiple inputs. The class inherits from `::testing::TestWithParam<T>` and calls `GetParam()` to read the current iteration value.

Example: `writer/test_roundtrip.cpp` parametrizes over `VTX::VtxFormat::{FlatBuffers, Protobuf}` so every round-trip assertion automatically runs against both serialization backends. Test names become `BothBackends/RoundtripTest.PreservesFrameData/FlatBuffers` etc.

## Writing new tests

1. **Pick the module**. New types go under `common/`; writer/reader/differ integration tests go under the matching directory.
2. **File naming**. One `.cpp` per concern, named `test_<subject>.cpp`.
3. **Pick the macro**. Use `TEST` unless you're either (a) sharing setup across 3+ tests (then `TEST_F`), or (b) running the same body against multiple inputs (then `TEST_P`). Don't reach for fixtures by default — ceremony without benefit on small tests.
4. **Add to CMakeLists**. List the new file in the `add_executable(vtx_tests ...)` call in `tests/CMakeLists.txt`. `gtest_discover_tests` picks up every `TEST*` macro automatically.
5. **Use helpers**. Call `VtxTest::FixturePath("...")` for input files and `VtxTest::OutputPath("...")` for transient artefacts — both are defined in `util/test_fixtures.h`.
6. **Keep tests self-contained**. Each test should be runnable in isolation; don't rely on order. For files written to `OutputPath`, derive the name from the current test (see `ReaderContextHappy::SetUp`) so parallel `ctest --parallel N` runs don't race.
7. **Regression tests get a `_Regression` suffix** and a comment referencing the bug + date. Future readers then know not to "simplify" the case away.

## Dependencies

- GoogleTest v1.15.2, fetched via CMake FetchContent at configure time. No network access needed after the initial configure (cached under `build/_deps/`).
- `flatc` (FlatBuffers compiler) and the zstd static library are also built from pinned FetchContent sources. No `zstd.dll` is shipped -- zstd is statically linked into the test binary.

## CI-friendly invocation

```bat
ctest --test-dir build --output-on-failure -C Release --parallel
```

Exit code is 0 only if every test passes. Use `--repeat until-fail:N` to flush flaky tests.
