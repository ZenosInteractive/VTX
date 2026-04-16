#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>
#include "vtx_reader.h"

namespace VTX {

    /// Thread-safe snapshot of chunk loading state at a point in time.
    struct ReaderChunkSnapshot {
        std::vector<int32_t> loaded_chunks;
        std::vector<int32_t> loading_chunks;
    };


    class ReaderChunkState {
    public:
        void OnChunkLoadStarted(int32_t chunk_idx);
        void OnChunkLoadFinished(int32_t chunk_idx);
        void OnChunkEvicted(int32_t chunk_idx);

        ReaderChunkSnapshot GetSnapshot() const;
        void Reset();

    private:
        mutable std::mutex state_mutex_;
        std::vector<int32_t> loaded_chunks_;
        std::vector<int32_t> loading_chunks_;
    };


    class IVtxReaderFacade {
    public:
        virtual ~IVtxReaderFacade() = default;
        virtual int32_t GetChunkFrameCountSafe(int32_t chunk_index) = 0;
        virtual int32_t GetTotalFrames() const = 0;
        virtual bool GetFrame(int32_t frame_index, VTX::Frame& out_frame) = 0;
        virtual const VTX::Frame* GetFrame(int32_t frame_index) = 0;
        virtual const VTX::Frame* GetFrameSync(int frame_index) = 0;
        virtual void GetFrameRange(int32_t start_frame, int32_t range, std::vector<VTX::Frame>& out_frames) = 0;
        virtual std::vector<VTX::Frame> GetFrameContext(int32_t center_frame, int32_t back_range, int32_t forward_range) = 0;
        virtual const std::vector<ChunkIndexEntry>& GetSeekTable() const = 0;
        virtual VTX::FileHeader GetHeader() = 0;
        virtual VTX::FileFooter GetFooter() = 0;
        virtual VTX::ContextualSchema GetContextualSchema() = 0;
        virtual VTX::PropertyAddressCache GetPropertyAddressCache() = 0;
        virtual void SetEvents(const ReplayReaderEvents& events) = 0;
        virtual void SetCacheWindow(uint32_t backward, uint32_t forward) = 0;
        virtual void InspectChunkHeader(int32_t index) const = 0;
        virtual FrameAccessor CreateAccessor() const = 0;
        virtual std::span<const std::byte> GetRawFrameBytes(int32_t frame_index) = 0;
    };

    /// Bundles a reader, its detected format, chunk state, and file metadata.
    /// Returned by OpenReplayFile(). Chunk events are pre-wired automatically.
    struct ReaderContext {
        ReaderContext() : chunk_state(std::make_unique<ReaderChunkState>()) {}

        explicit operator bool() const { return reader != nullptr; }
        IVtxReaderFacade* operator->() const { return reader.get(); }
        bool Loaded() const { return reader != nullptr; }
        const VtxFormat& GetFormat() const { return format; }
        const std::string& GetError() const { return error; }
        void SetError(const std::string& err) { error = err; }

        void Reset() {
            reader.reset();
            if (chunk_state) chunk_state->Reset();
            error.clear();
            format = VtxFormat::Unknown;
            size_in_mb = 0.0f;
        }

        std::unique_ptr<IVtxReaderFacade> reader;
        std::unique_ptr<ReaderChunkState> chunk_state;
        VtxFormat format = VtxFormat::Unknown;
        std::string error;
        float size_in_mb = 0.0f;
    };

    std::unique_ptr<IVtxReaderFacade> CreateFlatBuffersFacade(const std::string& filepath);
    std::unique_ptr<IVtxReaderFacade> CreateProtobuffFacade(const std::string& filepath);
    ReaderContext OpenReplayFile(const std::string& filepath);

}
