// ready_api.cpp -- Demonstrates the three consumption styles for the
// reader's chunk-0 "ready" signal introduced with the eager-warm change.
//
// Purpose
//   After OpenReplayFile() returns, an async load of chunk 0 is already in
//   flight.  The sample shows three ways a caller can wait for that load
//   to complete before the first GetFrame* call, plus how to observe a
//   failed load.
//
//   Style A -- Blocking wait with timeout    (simplest)
//   Style B -- Polling loop                   (useful when you have other
//                                              work to interleave, e.g. UI)
//   Style C -- Callback (OnReady / OnReadyFailed)
//                                              (reactive / pre-wired)
//
// Default input
//   content/reader/arena/arena_from_fbs_ds.vtx
//
//   (same file vtx_sample_read uses).  Any .vtx path can be passed as
//   argv[1] instead.
//
// Build
//   Link against vtx_reader (vtx_common is transitive).  See
//   samples/CMakeLists.txt.

#include "vtx/reader/core/vtx_reader_facade.h"
#include "vtx/common/vtx_logger.h"
#include "vtx/common/vtx_types.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

namespace {

    // --- Style A ---------------------------------------------------------
    // Block the current thread (with a deadline) until chunk 0 is ready
    // or the load fails.  WaitUntilReady(timeout) returns IsReady().
    int RunBlockingStyle(const std::string& path) {
        VTX_INFO("--- Style A: WaitUntilReady with 5s timeout ---");

        auto ctx = VTX::OpenReplayFile(path);
        if (!ctx) {
            VTX_ERROR("OpenReplayFile failed: {}", ctx.error);
            return 1;
        }

        const auto t0 = std::chrono::steady_clock::now();
        const bool ready = ctx.WaitUntilReady(std::chrono::seconds(5));
        const auto elapsed_ms
            = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

        if (!ready) {
            if (ctx.IsReadyFailed()) {
                VTX_ERROR("Chunk 0 failed after {} ms: {}", elapsed_ms, ctx.GetReadyError());
            } else {
                VTX_ERROR("Chunk 0 not ready after {} ms (timeout)", elapsed_ms);
            }
            return 1;
        }

        VTX_INFO("Ready after {} ms. Total frames: {}", elapsed_ms, ctx.reader->GetTotalFrames());

        // First frame access now hits the warm cache.
        const VTX::Frame* first = ctx.reader->GetFrameSync(0);
        VTX_INFO("Frame 0 buckets: {}", first ? first->GetBuckets().size() : 0);
        return 0;
    }

    // --- Style B ---------------------------------------------------------
    // Poll IsReady() in a loop while doing other work.  Good fit for UI
    // event loops that want to update a spinner / progress bar while the
    // reader warms up, without committing a whole thread to blocking.
    int RunPollingStyle(const std::string& path) {
        VTX_INFO("--- Style B: Polling IsReady() with UI-tick cadence ---");

        auto ctx = VTX::OpenReplayFile(path);
        if (!ctx) {
            VTX_ERROR("OpenReplayFile failed: {}", ctx.error);
            return 1;
        }

        constexpr auto kTick = std::chrono::milliseconds(16); // ~60 Hz
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        int ticks = 0;

        while (!ctx.IsReady() && !ctx.IsReadyFailed()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                VTX_ERROR("Timed out after {} polls", ticks);
                return 1;
            }
            // Imagine the UI advancing a spinner frame here.
            ++ticks;
            std::this_thread::sleep_for(kTick);
        }

        if (ctx.IsReadyFailed()) {
            VTX_ERROR("Chunk 0 failed after {} polls: {}", ticks, ctx.GetReadyError());
            return 1;
        }

        VTX_INFO("Ready after {} polls (~{} ms). Total frames: {}", ticks, ticks * 16, ctx.reader->GetTotalFrames());
        return 0;
    }

    // --- Style C ---------------------------------------------------------
    // Pre-wire OnReady / OnReadyFailed on a direct facade, then trigger
    // the warm ourselves.  This is the path to use when you want the
    // callback to run exactly once from the async worker thread without
    // any chance of a race with OpenReplayFile's own event wiring.
    //
    // Under the OpenReplayFile() flow the context's chunk-state events
    // are wired internally before WarmAt(0) fires, so user callbacks
    // registered AFTER OpenReplayFile() returns may miss the single-shot
    // signal (it's already fired).  Driving the facade directly avoids
    // that race.
    int RunCallbackStyle(const std::string& path) {
        VTX_INFO("--- Style C: Pre-wired OnReady / OnReadyFailed ---");

        auto facade = VTX::CreateFlatBuffersFacade(path);
        if (!facade) {
            VTX_ERROR("CreateFlatBuffersFacade failed for: {}", path);
            return 1;
        }

        std::atomic<bool> done {false};
        std::atomic<bool> succeeded {false};

        VTX::ReplayReaderEvents events;
        events.OnReady = [&]() {
            VTX_INFO("[callback] OnReady fired (from worker thread)");
            succeeded.store(true);
            done.store(true);
        };
        events.OnReadyFailed = [&](const std::string& err) {
            VTX_ERROR("[callback] OnReadyFailed: {}", err);
            done.store(true);
        };
        facade->SetEvents(events);

        // Kick off the async warm.  Returns immediately.
        facade->WarmAt(0);

        // Wait for either callback to fire.  In a real app you would
        // not spin -- you'd let the event loop run and handle the
        // callback when it lands.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!done.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        if (!done.load()) {
            VTX_ERROR("Callback did not fire within 5s");
            return 1;
        }
        if (!succeeded.load()) {
            return 1;
        }

        VTX_INFO("Total frames: {}", facade->GetTotalFrames());
        return 0;
    }

    void PrintUsage(const char* exe) {
        VTX_INFO("Usage: {} [--style=a|b|c|all] [replay.vtx]", exe);
        VTX_INFO("  --style=a    Blocking WaitUntilReady (default)");
        VTX_INFO("  --style=b    Polling loop");
        VTX_INFO("  --style=c    Pre-wired callback on direct facade");
        VTX_INFO("  --style=all  Run all three styles in sequence");
    }

} // namespace

int main(int argc, char* argv[]) {
    const char* style = "a";
    std::string path = "content/reader/arena/arena_from_fbs_ds.vtx";

    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strncmp(arg, "--style=", 8) == 0) {
            style = arg + 8;
        } else if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
            PrintUsage(argv[0]);
            return 0;
        } else {
            path = arg;
        }
    }

    VTX_INFO("Reading: {}", path);

    if (std::strcmp(style, "a") == 0)
        return RunBlockingStyle(path);
    if (std::strcmp(style, "b") == 0)
        return RunPollingStyle(path);
    if (std::strcmp(style, "c") == 0)
        return RunCallbackStyle(path);
    if (std::strcmp(style, "all") == 0) {
        int rc = RunBlockingStyle(path);
        if (rc != 0)
            return rc;
        rc = RunPollingStyle(path);
        if (rc != 0)
            return rc;
        return RunCallbackStyle(path);
    }

    VTX_ERROR("Unknown --style value: {}", style);
    PrintUsage(argv[0]);
    return 2;
}
