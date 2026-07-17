#pragma once
#include <string>
#include <stdexcept>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <zstd.h>
#include <xxh3.h>
#include "vtx/common/vtx_types.h"
#include "vtx/common/vtx_concepts.h"
#include "vtx/writer/policies/sinks/durable_file.h"
#include "vtx/writer/policies/sinks/recovery_journal.h"
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
            bool durable_writes = true;           ///< fsync each chunk to physical disk (crash/power-loss safe).
            bool enable_recovery_journal = true;  ///< maintain a ".recovery" sidecar for crash recovery.
        };

        explicit ChunkedFileSink(Config config)
            : config_(std::move(config)) {
            if (!file_.Open(config_.filename))
                throw std::runtime_error("VTX: Could not open " + config_.filename);
        }

        void OnSessionStart(const SchemaType& schema) {
            //this writes the vrx format, ie  "VTXP"(protobuff) VTXF(flatbuffer)
            std::string magic_bytes = SerializerPolicy::GetMagicBytes();
            WriteBlob(magic_bytes);

            std::string header_payload = SerializerPolicy::SerializeHeader(config_.header_config, schema);
            header_payload = CompressIfBeneficial(std::move(header_payload));
            uint32_t final_size = static_cast<uint32_t>(header_payload.size());
            file_.Write(&final_size, sizeof(final_size));
            file_.Write(header_payload.data(), final_size);
            if (config_.durable_writes)
                file_.Sync();

            // Start the crash-recovery journal only once the header is durable.
            if (config_.enable_recovery_journal) {
                journal_.Open(RecoveryJournal::PathFor(config_.filename), SerializerPolicy::GetMagicBytes(),
                              config_.durable_writes);
            }
        }

        void SaveChunk(std::vector<std::unique_ptr<FrameType>>& frames, const std::vector<int64_t>& created_utc,
                       int32_t start_frame, int32_t total_frames) {
            if (frames.empty())
                return;

            std::string payload = SerializerPolicy::SerializeChunk(frames, chunkIndex_, config_.b_use_compression);
            payload = CompressIfBeneficial(std::move(payload));

            uint64_t current_offset = file_.Tell();
            uint32_t final_size = static_cast<uint32_t>(payload.size());

            file_.Write(&final_size, sizeof(final_size));
            file_.Write(payload.data(), final_size);
            if (config_.durable_writes)
                file_.Sync();

            ChunkIndexData indexEntry;
            indexEntry.chunk_index = chunkIndex_++;
            indexEntry.file_offset = current_offset;
            indexEntry.start_frame = start_frame;
            indexEntry.end_frame = total_frames - 1;
            indexEntry.chunk_size_bytes = final_size + sizeof(uint32_t);
            indexEntry.checksum = XXH3_64bits(payload.data(), payload.size());
            seek_table_.push_back(indexEntry);

            // Journal the committed chunk AFTER its bytes are durable on disk, so
            // the journal never references a chunk that isn't there (data-before-journal).
            if (journal_.IsOpen())
                journal_.AppendChunk(indexEntry);
        }

        void Close(const SessionFooter& footerData) {
            if (!file_.IsOpen())
                return;
            std::string footer_payload = SerializerPolicy::SerializeFooter(seek_table_, footerData);
            footer_payload = CompressIfBeneficial(std::move(footer_payload));

            file_.Write(footer_payload.data(), footer_payload.size());
            uint32_t final_size = static_cast<uint32_t>(footer_payload.size());
            file_.Write(&final_size, sizeof(final_size));
            WriteBlob(SerializerPolicy::GetMagicBytes());
            if (config_.durable_writes)
                file_.Sync();

            // Clean shutdown: the footer is durable, so the recovery journal is no
            // longer needed. Its absence signals a clean file to the repair path.
            if (journal_.IsOpen()) {
                journal_.Close();
                std::remove(RecoveryJournal::PathFor(config_.filename).c_str());
            }
        }

    private:
        void WriteBlob(const std::string& data) { file_.Write(data.data(), data.size()); }

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
        DurableFile file_;
        RecoveryJournal journal_;
        int32_t chunkIndex_ = 0;
        std::vector<ChunkIndexData> seek_table_; //Generic tables, format agnostic
    };
}; // namespace VTX
