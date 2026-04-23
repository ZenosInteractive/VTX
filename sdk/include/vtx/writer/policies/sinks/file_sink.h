#pragma once
#include <fstream>
#include <string>
#include <stdexcept>
#include <cstdint>
#include <vector>
#include <zstd.h>
#include "vtx/common/vtx_types.h"
#include "vtx/common/vtx_concepts.h"
namespace VTX {

    template <IVtxWriterPolicy Policy>
    class ChunkedFileSink {
    public:
        using SerializerPolicy = Policy;
        using FrameType = typename SerializerPolicy::FrameType;
        using SchemaType = typename SerializerPolicy::SchemaType;
        using HeaderType = typename SerializerPolicy::HeaderType;

        struct Config {
            std::string filename;
            HeaderType header_config;
            bool b_use_compression = true;
            int8_t compression_level = 10;
        };

        explicit ChunkedFileSink(Config config)
            : config_(std::move(config)) {
            file_.open(config_.filename, std::ios::binary | std::ios::out | std::ios::trunc);
            if (!file_.is_open())
                throw std::runtime_error("VTX: Could not open " + config_.filename);
        }

        void OnSessionStart(const SchemaType& schema) {
            //this writes the vrx format, ie  "VTXP"(protobuff) VTXF(flatbuffer)
            std::string magic_bytes = SerializerPolicy::GetMagicBytes();
            WriteBlob(magic_bytes);

            std::string header_payload = SerializerPolicy::SerializeHeader(config_.header_config, schema);
            header_payload = CompressIfBeneficial(std::move(header_payload));
            uint32_t final_size = static_cast<uint32_t>(header_payload.size());
            file_.write(reinterpret_cast<const char*>(&final_size), sizeof(final_size));
            file_.write(header_payload.data(), final_size);
        }

        void SaveChunk(std::vector<std::unique_ptr<FrameType>>& frames, const std::vector<int64_t>& created_utc,
                       int32_t start_frame, int32_t total_frames) {
            if (frames.empty())
                return;

            float chunk_start_time = 0.0f;
            float chunk_end_time = 0.0f;
            if (!created_utc.empty()) {
                chunk_start_time = static_cast<float>(created_utc.front());
                chunk_end_time = static_cast<float>(created_utc.back());
            }

            std::string payload = SerializerPolicy::SerializeChunk(frames, chunkIndex_, config_.b_use_compression);
            payload = CompressIfBeneficial(std::move(payload));

            uint64_t current_offset = file_.tellp();
            uint32_t final_size = static_cast<uint32_t>(payload.size());

            file_.write(reinterpret_cast<const char*>(&final_size), sizeof(final_size));
            file_.write(payload.data(), final_size);

            ChunkIndexData indexEntry;
            indexEntry.chunk_index = chunkIndex_++;
            indexEntry.file_offset = current_offset;
            indexEntry.start_frame = start_frame;
            indexEntry.end_frame = total_frames - 1;
            indexEntry.chunk_size_bytes = final_size + sizeof(uint32_t);
            seek_table_.push_back(indexEntry);
        }

        void Close(const SessionFooter& footerData) {
            if (!file_.is_open())
                return;
            std::string footer_payload = SerializerPolicy::SerializeFooter(seek_table_, footerData);
            footer_payload = CompressIfBeneficial(std::move(footer_payload));

            file_.write(footer_payload.data(), footer_payload.size());
            uint32_t final_size = static_cast<uint32_t>(footer_payload.size());
            file_.write(reinterpret_cast<const char*>(&final_size), sizeof(final_size));
            WriteBlob(SerializerPolicy::GetMagicBytes());
        }

    private:
        void WriteBlob(const std::string& data) { file_.write(data.data(), data.size()); }

        std::string CompressIfBeneficial(std::string payload) {
            if (!config_.b_use_compression || payload.size() < 512) {
                return payload;
            }

            size_t const max_size = ZSTD_compressBound(payload.size());
            std::string compressed_blob(max_size, '\0');

            size_t const compressed_size = ZSTD_compress(compressed_blob.data(), max_size, payload.data(),
                                                         payload.size(), config_.compression_level);

            if (ZSTD_isError(compressed_size)) {
                return payload;
            }

            if (compressed_size >= payload.size()) {
                return payload;
            }

            compressed_blob.resize(compressed_size);
            return compressed_blob;
        }

        Config config_;
        std::ofstream file_;
        int32_t chunkIndex_ = 0;
        std::vector<ChunkIndexData> seek_table_; //Generic tables, format agnostic
    };
}; // namespace VTX
