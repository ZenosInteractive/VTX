#include "vtx_schema_generated.h"
#include "vtx_schema.pb.h"
#include "vtx/writer/core/vtx_writer_facade.h"

#include "vtx/writer/core/writer.h"

#include "vtx/common/readers/schema_reader/schema_registry.h"
#include "vtx/common/vtx_logger.h"
#include "vtx/writer/policies/formatters/flatbuffers_vtx_policy.h"
#include "vtx/writer/policies/formatters/protobuff_vtx_policy.h"
#include "vtx/writer/policies/sinks/file_sink.h"
#include "vtx/writer/policies/sinks/network_sink.h"

#include <filesystem>

namespace VTX {

    namespace {
        bool WriterSchemaIsAcceptable(const std::string& schema_json_path) {
            if (schema_json_path.empty()) {
                return true;
            }
            SchemaRegistry probe;
            if (!probe.LoadFromJson(schema_json_path)) {
                VTX_ERROR("Refusing to create writer: schema '{}' is missing or invalid.", schema_json_path);
                return false;
            }
            return true;
        }

        void EnsureOutputDir(const std::string& filepath) {
            const std::filesystem::path parent = std::filesystem::path(filepath).parent_path();
            if (parent.empty()) {
                return;
            }
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
        }
    } // namespace

    template <typename SinkPolicyType>
    class WriterFacadeImpl : public IVtxWriterFacade {
    public:
        WriterFacadeImpl(typename ReplayWriter<SinkPolicyType>::Config internal_config)
            : writer_(internal_config) {}

        ~WriterFacadeImpl() override {
            if (!stopped_) {
                try {
                    Stop();
                } catch (...) {}
            }
        }


        void RecordFrame(VTX::Frame& native_frame, const VTX::GameTime::GameTimeRegister& game_time_register) override {
            if (stopped_)
                return;
            writer_.RecordFrame(native_frame, game_time_register);
        }

        RecordResult TryRecordFrame(VTX::Frame& native_frame,
                                    const VTX::GameTime::GameTimeRegister& game_time_register) override {
            if (stopped_) {
                return RecordResult::MadeRejected(VtxErrorCode::InvalidArgument, "writer already stopped");
            }
            return writer_.TryRecordFrame(native_frame, game_time_register);
        }

        const VTX::Frame* GetLastFinalizedFrame() const override { return writer_.GetLastFinalizedFrame(); }

        const VTX::PropertyContainer* FindEntity(const std::string& bucket_name,
                                                 const std::string& unique_id) const override {
            return writer_.FindEntity(bucket_name, unique_id);
        }

        void Flush() override {
            if (stopped_)
                return;
            writer_.Flush();
        }

        void Stop() override {
            if (stopped_)
                return; // second Stop() is a no-op
            writer_.Stop();
            stopped_ = true;
        }

        VTX::SchemaRegistry& GetSchema() override { return writer_.GetRegistry(); }

        void SetPostProcessor(std::shared_ptr<IFramePostProcessor> processor) override {
            writer_.SetPostProcessor(std::move(processor));
        }
        std::shared_ptr<IFramePostProcessor> GetPostProcessor() const override { return writer_.GetPostProcessor(); }
        void ClearPostProcessor() override { writer_.ClearPostProcessor(); }

    private:
        ReplayWriter<SinkPolicyType> writer_;
        bool stopped_ = false;
    };


    std::unique_ptr<IVtxWriterFacade> CreateFlatBuffersWriterFacade(const WriterFacadeConfig& config) {
        if (!WriterSchemaIsAcceptable(config.schema_json_path)) {
            return nullptr;
        }
        if (config.create_output_dirs) {
            EnsureOutputDir(config.output_filepath);
        }
        using SinkType = ChunkedFileSink<VTX::FlatBuffersVtxPolicy>;

        ReplayWriter<SinkType>::Config internal_cfg;
        internal_cfg.default_fps = config.default_fps;
        internal_cfg.is_increasing = config.is_increasing;
        internal_cfg.chunker_config.max_frames = config.chunk_max_frames;
        internal_cfg.chunker_config.max_bytes = config.chunk_max_bytes;

        internal_cfg.sink_config.filename = config.output_filepath;
        internal_cfg.schema_json_path = config.schema_json_path;
        internal_cfg.retain_finalized_snapshot = config.retain_finalized_snapshot;
        internal_cfg.sink_config.header_config.replay_name = config.replay_name;
        internal_cfg.sink_config.header_config.replay_uuid = config.replay_uuid;
        internal_cfg.sink_config.b_use_compression = config.use_compression;

        return std::make_unique<WriterFacadeImpl<SinkType>>(internal_cfg);
    }

    std::unique_ptr<IVtxWriterFacade> CreateProtobuffWriterFacade(const WriterFacadeConfig& config) {
        if (!WriterSchemaIsAcceptable(config.schema_json_path)) {
            return nullptr;
        }
        if (config.create_output_dirs) {
            EnsureOutputDir(config.output_filepath);
        }
        using SinkType = VTX::ChunkedFileSink<VTX::ProtobufVtxPolicy>;

        ReplayWriter<SinkType>::Config internal_cfg;
        internal_cfg.sink_config.header_config.replay_name = config.replay_name;
        internal_cfg.sink_config.header_config.replay_uuid = config.replay_uuid;
        internal_cfg.default_fps = config.default_fps;
        internal_cfg.is_increasing = config.is_increasing;
        internal_cfg.chunker_config.max_frames = config.chunk_max_frames;
        internal_cfg.chunker_config.max_bytes = config.chunk_max_bytes;

        internal_cfg.sink_config.filename = config.output_filepath;
        internal_cfg.schema_json_path = config.schema_json_path;
        internal_cfg.retain_finalized_snapshot = config.retain_finalized_snapshot;

        internal_cfg.sink_config.b_use_compression = config.use_compression;
        return std::make_unique<WriterFacadeImpl<SinkType>>(internal_cfg);
    }

    std::unique_ptr<IVtxWriterFacade> CreateFlatBuffersNetworkWriterFacade(const NetworkWriterFacadeConfig& config) {
        if (!WriterSchemaIsAcceptable(config.schema_json_path)) {
            return nullptr;
        }
        using SinkType = ChunkedNetworkSink<VTX::FlatBuffersVtxPolicy>;

        ReplayWriter<SinkType>::Config internal_cfg;
        internal_cfg.default_fps = config.default_fps;
        internal_cfg.is_increasing = config.is_increasing;
        internal_cfg.chunker_config.max_frames = config.chunk_max_frames;
        internal_cfg.chunker_config.max_bytes = config.chunk_max_bytes;
        internal_cfg.schema_json_path = config.schema_json_path;
        internal_cfg.retain_finalized_snapshot = config.retain_finalized_snapshot;
        internal_cfg.sink_config.host = config.host;
        internal_cfg.sink_config.port = config.port;
        internal_cfg.sink_config.header_config.replay_name = config.replay_name;
        internal_cfg.sink_config.header_config.replay_uuid = config.replay_uuid;
        internal_cfg.sink_config.b_use_compression = config.use_compression;

        return std::make_unique<WriterFacadeImpl<SinkType>>(internal_cfg);
    }

    std::unique_ptr<IVtxWriterFacade> CreateProtobuffNetworkWriterFacade(const NetworkWriterFacadeConfig& config) {
        if (!WriterSchemaIsAcceptable(config.schema_json_path)) {
            return nullptr;
        }
        using SinkType = ChunkedNetworkSink<VTX::ProtobufVtxPolicy>;

        ReplayWriter<SinkType>::Config internal_cfg;
        internal_cfg.default_fps = config.default_fps;
        internal_cfg.is_increasing = config.is_increasing;
        internal_cfg.chunker_config.max_frames = config.chunk_max_frames;
        internal_cfg.chunker_config.max_bytes = config.chunk_max_bytes;
        internal_cfg.schema_json_path = config.schema_json_path;
        internal_cfg.retain_finalized_snapshot = config.retain_finalized_snapshot;
        internal_cfg.sink_config.host = config.host;
        internal_cfg.sink_config.port = config.port;
        internal_cfg.sink_config.header_config.replay_name = config.replay_name;
        internal_cfg.sink_config.header_config.replay_uuid = config.replay_uuid;
        internal_cfg.sink_config.b_use_compression = config.use_compression;

        return std::make_unique<WriterFacadeImpl<SinkType>>(internal_cfg);
    }

} // namespace VTX
