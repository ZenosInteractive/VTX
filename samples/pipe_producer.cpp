// pipe_producer.cpp -- Generates synthetic game-data frames as length-prefixed
// JSON messages and writes them to stdout.
//
// Wire format (matches PipeFrameDataSource):
//
//     [uint32 LE size][JSON payload of `size` bytes]    (repeated, then a
//                                                        zero-size sentinel)
//
// Each JSON payload is a single frame, e.g.:
//
//     {
//       "game_time": 0.0167,
//       "entities": [
//         {"id": "player_0", "type_id": 0,
//          "floats": [100.5],
//          "translation": [0.0, 0.0, 50.0]}
//       ]
//     }
//
// Usage
//   vtx_sample_pipe_producer [num_frames] [delay_ms] > out.bin
//   vtx_sample_pipe_producer 100 | vtx_sample_pipe_consumer - out.vtx schema.json
//
// Args
//   argv[1] -- frame count.  > 0 sends exactly that many frames;
//              0 streams CONTINUOUSLY until you press Enter in this terminal,
//              then stops cleanly.  (default: 100)
//   argv[2] -- delay between frames in ms.  (default: 0 -- as fast as
//              possible; in continuous mode 0 is paced up to ~60 fps)
//
// Build
//   Link: vtx_writer (drags in nlohmann/json transitively via vtx_common).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {

    std::atomic<bool> g_stop {false};

    bool EmitFrame(const nlohmann::json& frame) {
        const std::string serialised = frame.dump();
        const uint32_t size = static_cast<uint32_t>(serialised.size());

        std::fwrite(&size, sizeof(size), 1, stdout);
        std::fwrite(serialised.data(), 1, size, stdout);
        std::fflush(stdout);
        return std::ferror(stdout) == 0;
    }

    nlohmann::json BuildFrame(int frame_index, float fps) {
        nlohmann::json frame;
        frame["game_time"] = static_cast<float>(frame_index) / fps;

        nlohmann::json entity;
        entity["id"] = "player_" + std::to_string(frame_index % 10);
        entity["type_id"] = 0;
        entity["floats"] = {std::max(0.0f, 100.0f - static_cast<float>(frame_index))};
        entity["translation"] = {static_cast<double>(frame_index), 0.0, 50.0};

        frame["entities"] = nlohmann::json::array({entity});
        return frame;
    }

} // namespace

int main(int argc, char* argv[]) {
    const int num_frames = (argc > 1) ? std::max(0, std::atoi(argv[1])) : 100;
    int delay_ms = (argc > 2) ? std::max(0, std::atoi(argv[2])) : 0;
    const bool continuous = (num_frames == 0);

    if (continuous && delay_ms == 0)
        delay_ms = 16; // ~60 fps

    constexpr float kFps = 60.0f;

#ifdef _WIN32
    ::_setmode(::_fileno(stdout), _O_BINARY);
#endif


    if (continuous) {
        std::fprintf(stderr, "pipe_producer: streaming continuously -- press Enter to stop.\n");
        std::thread([] {
            std::string line;
            std::getline(std::cin, line);
            g_stop.store(true);
        }).detach();
    }

    int sent = 0;
    for (int i = 0;; ++i) {
        if (continuous) {
            if (g_stop.load())
                break;
        } else if (i >= num_frames) {
            break;
        }

        if (!EmitFrame(BuildFrame(i, kFps)))
            break; // consumer disconnected
        ++sent;

        if (delay_ms > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }

    const uint32_t kSentinel = 0;
    std::fwrite(&kSentinel, sizeof(kSentinel), 1, stdout);
    std::fflush(stdout);

    std::fprintf(stderr, "pipe_producer: sent %d frames.\n", sent);
    return 0;
}
