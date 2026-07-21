#pragma once

#include <memory>
#include <string>

#include "schema_sanitizer.h"
#include "vtx/common/vtx_types.h"
#include "vtx/common/readers/schema_reader/schema_registry.h"
#include "vtx/writer/core/vtx_frame_post_processor.h"
#include "vtx/writer/core/vtx_writer_result.h"


namespace VTX {

    class IFileSinkPerfObserver; // defined in vtx/writer/policies/sinks/file_sink.h

    class IVtxWriterFacade {
    public:
        virtual ~IVtxWriterFacade() = default;

        virtual void RecordFrame(VTX::Frame& native_frame,
                                 const VTX::GameTime::GameTimeRegister& game_time_register) = 0;

        virtual RecordResult TryRecordFrame(VTX::Frame& native_frame,
                                            const VTX::GameTime::GameTimeRegister& game_time_register) = 0;
        virtual const VTX::Frame* GetLastFinalizedFrame() const = 0;
        virtual const VTX::PropertyContainer* FindEntity(const std::string& bucket_name,
                                                         const std::string& unique_id) const = 0;

        virtual void Flush() = 0;
        virtual void Stop() = 0;
        virtual VTX::SchemaRegistry& GetSchema() = 0;

        // Durability barrier for async recordings (async_io=true): block until every frame
        // accepted so far is durable on disk. Under async, Flush() only closes the current chunk
        // and enqueues its write -- it is NOT a durability barrier; Drain() is. For synchronous
        // recordings every accepted frame is already durable, so Drain() returns immediately.
        // Returns the latched sink error if the async I/O worker failed, else a None error.
        virtual VtxError Drain() = 0;

        // The last async-sink I/O error, or a None error if none has occurred (always None for
        // synchronous recordings). Pairs with TryRecordFrame returning VtxErrorCode::SinkFailed.
        virtual VtxError GetLastError() const = 0;

        // Depth of the async I/O queue (0 for synchronous recordings). Telemetry for backpressure.
        virtual size_t GetQueueDepth() const = 0;

        // The processor's Process() runs on every RecordFrame() call,
        // after timer validation and BEFORE serialisation to disk: its
        // mutations are what end up in the .vtx file.  Call before
        // RecordFrame to take effect; safe to swap mid-recording (new
        // processor takes over on the next RecordFrame).
        virtual void SetPostProcessor(std::shared_ptr<IFramePostProcessor> processor) = 0;
        virtual std::shared_ptr<IFramePostProcessor> GetPostProcessor() const = 0;
        virtual void ClearPostProcessor() = 0;
    };

    struct WriterFacadeConfig {
        std::string replay_name = "";
        std::string replay_uuid = "";
        std::string output_filepath = "";
        float default_fps = 60.0f;
        bool is_increasing = true;
        int32_t chunk_max_frames = 1000;
        size_t chunk_max_bytes = 10 * 1024 * 1024; // 10 MB
        bool use_compression = true;
        std::string schema_json_path = "";
        std::string schema_json_content = "";                      // in-memory schema JSON (wins over schema_json_path)
        std::shared_ptr<SchemaRegistry> schema_registry = nullptr; // pre-built registry (wins over content/path)
        bool retain_finalized_snapshot = false;
        bool create_output_dirs = true;
        IFileSinkPerfObserver* perf_observer = nullptr; ///< optional file-sink timings; null = no-op.

        // --- Durability / crash-recovery (exposed at the facade at last) ---
        bool durable_writes = true;          ///< fsync each chunk/journal commit (crash + power-loss safe).
        bool enable_recovery_journal = true; ///< maintain the ".recovery" sidecar for crash recovery.

        // --- Async I/O (opt-in) ---
        bool async_io = false;             ///< move chunk/journal I/O to a worker thread; caller never waits on disk.
        size_t async_max_queue_frames = 0; ///< backpressure bound (items). 0 -> 2 * chunk_max_frames.
    };

    enum class SerializationFormat : uint8_t {
        Flatbuffers,
        Protobuf,
    };

    std::unique_ptr<IVtxWriterFacade> CreateFlatBuffersWriterFacade(const WriterFacadeConfig& config);

    std::unique_ptr<IVtxWriterFacade> CreateProtobufWriterFacade(const WriterFacadeConfig& config);

    struct NetworkWriterFacadeConfig {
        std::string replay_name = "";
        std::string replay_uuid = "";
        std::string host = "127.0.0.1";
        uint16_t port = 0;
        float default_fps = 60.0f;
        bool is_increasing = true;
        int32_t chunk_max_frames = 1000;
        size_t chunk_max_bytes = 10 * 1024 * 1024; // 10 MB
        bool use_compression = true;
        std::string schema_json_path = "";
        std::string schema_json_content = "";                      // in-memory schema JSON (wins over schema_json_path)
        std::shared_ptr<SchemaRegistry> schema_registry = nullptr; // pre-built registry (wins over content/path)
        bool retain_finalized_snapshot = false;
    };

    std::unique_ptr<IVtxWriterFacade> CreateFlatBuffersNetworkWriterFacade(const NetworkWriterFacadeConfig& config);

    std::unique_ptr<IVtxWriterFacade> CreateProtobufNetworkWriterFacade(const NetworkWriterFacadeConfig& config);

} // namespace VTX
