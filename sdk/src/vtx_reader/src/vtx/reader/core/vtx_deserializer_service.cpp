#include "vtx/reader/core/vtx_deserializer_service.h"
#include <stdexcept>
#include <string>
#include <zstd.h>

#include "vtx/common/vtx_replay_framing.h"
#include "vtx_schema.pb.h"
using namespace VTX;

std::string VTX::ReplayUnpacker::Decompress(const std::string& compressed_data) {
    // Blobs are stored raw when zstd would not help; the zstd magic decides (one
    // definition for every producer and consumer in vtx_replay_framing.h).
    if (!Framing::LooksLikeZstd(compressed_data)) {
        return compressed_data;
    }

    unsigned long long const rSize = ZSTD_getFrameContentSize(compressed_data.data(), compressed_data.size());

    if (rSize == ZSTD_CONTENTSIZE_ERROR) {
        throw std::runtime_error("ZSTD: It's not a standard ZSTD compressed block");
    }
    if (rSize == ZSTD_CONTENTSIZE_UNKNOWN) {
        throw std::runtime_error("ZSTD: Original size unknown");
    }

    std::string decompressedBuffer;
    decompressedBuffer.resize(rSize);

    size_t const dSize =
        ZSTD_decompress(decompressedBuffer.data(), rSize, compressed_data.data(), compressed_data.size());

    if (ZSTD_isError(dSize)) {
        throw std::runtime_error("ZSTD Decompress Error: " + std::string(ZSTD_getErrorName(dSize)));
    }

    return decompressedBuffer;
}

std::vector<std::unique_ptr<cppvtx::Frame>> VTX::ReplayUnpacker::Unpack(const cppvtx::FrameChunk& chunk) {
    std::string uncompressed_data = "";

    if (chunk.is_compressed() && !chunk.compressed_data().empty()) {
        const std::string& compressed = chunk.compressed_data();

        unsigned long long const c_size = ZSTD_getFrameContentSize(compressed.data(), compressed.size());
        if (c_size == ZSTD_CONTENTSIZE_ERROR || c_size == ZSTD_CONTENTSIZE_UNKNOWN) {
            throw std::runtime_error("VTX Reader: Unknown ZSTD original size");
        }

        uncompressed_data.resize(c_size);
        size_t const dSize = ZSTD_decompress(uncompressed_data.data(), c_size, compressed.data(), compressed.size());

        if (ZSTD_isError(dSize)) {
            throw std::runtime_error("VTX Reader: Error decompressing ZSTD");
        }
    } else {
        uncompressed_data = chunk.compressed_data();
    }

    cppvtx::FrameChunk innerContainer;
    if (!uncompressed_data.empty()) {
        if (!innerContainer.ParseFromString(uncompressed_data)) {
            throw std::runtime_error("VTX Reader: Failed to parse FrameChunk");
        }
    } else {
        innerContainer = chunk;
    }

    std::vector<std::unique_ptr<cppvtx::Frame>> frames;
    for (int i = 0; i < innerContainer.frames_size(); ++i) {
        auto f = std::make_unique<cppvtx::Frame>();
        f->Swap(innerContainer.mutable_frames(i));
        frames.push_back(std::move(f));
    }

    return frames;
}
