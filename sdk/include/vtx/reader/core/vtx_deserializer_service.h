#pragma once
#include <vector>
#include <string>
#include <memory>

namespace cppvtx {
    class FrameChunk;
    class Frame;
} // namespace cppvtx

namespace VTX {
    namespace ReplayUnpacker {
        std::string Decompress(const std::string& compressed_data);
        std::vector<std::unique_ptr<cppvtx::Frame>> Unpack(const cppvtx::FrameChunk& chunk);
    }; // namespace ReplayUnpacker
} // namespace VTX
