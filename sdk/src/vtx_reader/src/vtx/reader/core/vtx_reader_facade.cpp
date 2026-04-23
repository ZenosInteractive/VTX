#include "vtx_schema_generated.h"
#include "vtx_schema.pb.h"

#include <algorithm>
#include <fstream>

#include "vtx/reader/core/vtx_reader_facade.h"
#include "vtx/reader/core/vtx_reader.h"
#include "vtx/reader/policies/formatters/flatbuffer_reader_policy.h"
#include "vtx/reader/policies/formatters/protobuff_reader_policy.h"

namespace VTX {


    void ReaderChunkState::OnChunkLoadStarted(int32_t chunk_idx) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        loading_chunks_.push_back(chunk_idx);
        VTX_DEBUG("Chunk {} load started.", chunk_idx);
    }

    void ReaderChunkState::OnChunkLoadFinished(int32_t chunk_idx) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = std::remove(loading_chunks_.begin(), loading_chunks_.end(), chunk_idx);
        if (it != loading_chunks_.end()) {
            loading_chunks_.erase(it, loading_chunks_.end());
        }
        loaded_chunks_.push_back(chunk_idx);
        std::sort(loaded_chunks_.begin(), loaded_chunks_.end());
        VTX_DEBUG("Chunk {} loaded into RAM.", chunk_idx);
    }

    void ReaderChunkState::OnChunkEvicted(int32_t chunk_idx) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = std::remove(loaded_chunks_.begin(), loaded_chunks_.end(), chunk_idx);
        if (it != loaded_chunks_.end()) {
            loaded_chunks_.erase(it, loaded_chunks_.end());
        }
        VTX_DEBUG("Chunk {} evicted from RAM.", chunk_idx);
    }

    ReaderChunkSnapshot ReaderChunkState::GetSnapshot() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return ReaderChunkSnapshot {
            .loaded_chunks = loaded_chunks_,
            .loading_chunks = loading_chunks_,
        };
    }

    void ReaderChunkState::Reset() {
        std::lock_guard<std::mutex> lock(state_mutex_);
        loaded_chunks_.clear();
        loading_chunks_.clear();
    }


    class FlatBuffersFacadeImpl : public IVtxReaderFacade {
    public:
        FlatBuffersFacadeImpl(const std::string& filepath)
            : InternalReader(filepath) {}

        void SetEvents(const ReplayReaderEvents& events) override { InternalReader.SetEvents(events); }

        VTX::FileHeader GetHeader() override { return InternalReader.GetFileHeader(); }

        VTX::FileFooter GetFooter() override { return InternalReader.GetFileFooter(); }

        VTX::ContextualSchema GetContextualSchema() override { return InternalReader.GetContextualSchema(); }

        VTX::PropertyAddressCache GetPropertyAddressCache() override {
            return InternalReader.GetPropertyAddressCache();
        }

        int32_t GetTotalFrames() const override { return InternalReader.GetTotalFrames(); }

        const std::vector<ChunkIndexEntry>& GetSeekTable() const override { return InternalReader.GetSeekTable(); }

        void SetCacheWindow(uint32_t backward, uint32_t forward) override {
            InternalReader.SetCacheWindow(backward, forward);
        }

        void WarmAt(int32_t frame_index) override { InternalReader.WarmAt(frame_index); }

        bool GetFrame(int32_t frame_index, VTX::Frame& out_frame) override {
            return InternalReader.GetFrame(frame_index, out_frame);
        }

        const VTX::Frame* GetFrame(int32_t frame_index) override { return InternalReader.GetFramePtr(frame_index); }

        const VTX::Frame* GetFrameSync(int frame_index) override { return InternalReader.GetFramePtrSync(frame_index); }

        void GetFrameRange(int32_t start_frame, int32_t range, std::vector<VTX::Frame>& out_frames) override {
            InternalReader.GetFrameRange(start_frame, range, out_frames);
        }

        std::vector<VTX::Frame> GetFrameContext(int32_t center_frame, int32_t back_range,
                                                int32_t forward_range) override {
            return InternalReader.GetFrameContext(center_frame, back_range, forward_range);
        }

        int32_t GetChunkFrameCountSafe(int32_t chunk_index) override {
            return InternalReader.GetChunkFrameCountSafe(chunk_index);
        }

        void InspectChunkHeader(int32_t index) const override { InternalReader.InspectChunkHeader(index); }

        FrameAccessor CreateAccessor() const override { return InternalReader.CreateAccessor(); }

        std::span<const std::byte> GetRawFrameBytes(int32_t frame_index) override {
            return InternalReader.GetRawFrameBytes(frame_index);
        }

    private:
        VTX::ReplayReader<VTX::FlatBuffersReaderPolicy> InternalReader;
    };


    class ProtobuffFacadeImpl : public IVtxReaderFacade {
    public:
        ProtobuffFacadeImpl(const std::string& filepath)
            : InternalReader(filepath) {}

        void SetEvents(const ReplayReaderEvents& events) override { InternalReader.SetEvents(events); }

        VTX::FileHeader GetHeader() override { return InternalReader.GetFileHeader(); }

        VTX::FileFooter GetFooter() override { return InternalReader.GetFileFooter(); }

        VTX::ContextualSchema GetContextualSchema() override { return InternalReader.GetContextualSchema(); }

        VTX::PropertyAddressCache GetPropertyAddressCache() override {
            return InternalReader.GetPropertyAddressCache();
        }

        int32_t GetTotalFrames() const override { return InternalReader.GetTotalFrames(); }

        const std::vector<ChunkIndexEntry>& GetSeekTable() const override { return InternalReader.GetSeekTable(); }

        void SetCacheWindow(uint32_t backward, uint32_t forward) override {
            InternalReader.SetCacheWindow(backward, forward);
        }

        void WarmAt(int32_t frame_index) override { InternalReader.WarmAt(frame_index); }

        bool GetFrame(int32_t frame_index, VTX::Frame& out_frame) override {
            return InternalReader.GetFrame(frame_index, out_frame);
        }

        const VTX::Frame* GetFrame(int32_t frame_index) override { return InternalReader.GetFramePtr(frame_index); }

        const VTX::Frame* GetFrameSync(int frame_index) override { return InternalReader.GetFramePtrSync(frame_index); }

        void GetFrameRange(int32_t start_frame, int32_t range, std::vector<VTX::Frame>& out_frames) override {
            InternalReader.GetFrameRange(start_frame, range, out_frames);
        }

        std::vector<VTX::Frame> GetFrameContext(int32_t center_frame, int32_t back_range,
                                                int32_t forward_range) override {
            return InternalReader.GetFrameContext(center_frame, back_range, forward_range);
        }

        int32_t GetChunkFrameCountSafe(int32_t chunk_index) override {
            return InternalReader.GetChunkFrameCountSafe(chunk_index);
        }

        void InspectChunkHeader(int32_t index) const override { InternalReader.InspectChunkHeader(index); }

        FrameAccessor CreateAccessor() const override { return InternalReader.CreateAccessor(); }

        std::span<const std::byte> GetRawFrameBytes(int32_t frame_index) override {
            return InternalReader.GetRawFrameBytes(frame_index);
        }

    private:
        VTX::ReplayReader<VTX::ProtobufReaderPolicy> InternalReader;
    };


    std::unique_ptr<IVtxReaderFacade> CreateFlatBuffersFacade(const std::string& filepath) {
        return std::make_unique<FlatBuffersFacadeImpl>(filepath);
    }

    std::unique_ptr<IVtxReaderFacade> CreateProtobuffFacade(const std::string& filepath) {
        return std::make_unique<ProtobuffFacadeImpl>(filepath);
    }


    ReaderContext OpenReplayFile(const std::string& filepath) {
        ReaderContext result;
        try {
            std::ifstream file(filepath, std::ios::binary | std::ios::in);
            if (!file.is_open()) {
                result.SetError("Failed to open file: " + filepath);
                return result;
            }

            // Calculate file size
            file.seekg(0, std::ios::end);
            const auto file_size = file.tellg();
            result.size_in_mb = static_cast<float>(file_size) / (1024.0f * 1024.0f);
            file.seekg(0);

            // Read magic bytes
            std::string magic(4, '\0');
            file.read(magic.data(), 4);
            file.close();

            if (magic == "VTXF") {
                result.format = VtxFormat::FlatBuffers;
                result.reader = std::make_unique<FlatBuffersFacadeImpl>(filepath);
            } else if (magic == "VTXP") {
                result.format = VtxFormat::Protobuf;
                result.reader = std::make_unique<ProtobuffFacadeImpl>(filepath);
            } else {
                result.SetError("Unknown file format. Magic: " + magic);
                return result;
            }

            if (!result.reader) {
                result.SetError("Reader creation failed.");
                return result;
            }

            // Wire chunk events to the built-in state tracker
            auto* cs = result.chunk_state.get();
            cs->Reset();

            ReplayReaderEvents events;
            events.OnChunkLoadStarted = [cs](int32_t chunk_idx) {
                cs->OnChunkLoadStarted(chunk_idx);
            };
            events.OnChunkLoadFinished = [cs](int32_t chunk_idx) {
                cs->OnChunkLoadFinished(chunk_idx);
            };
            events.OnChunkEvicted = [cs](int32_t chunk_idx) {
                cs->OnChunkEvicted(chunk_idx);
            };
            result.reader->SetEvents(events);

        } catch (const std::exception& e) {
            result.SetError(std::string("Error opening replay: ") + e.what());
            result.reader.reset();
        } catch (...) {
            result.SetError("Unknown error while opening replay.");
            result.reader.reset();
        }
        return result;
    }

} // namespace VTX
