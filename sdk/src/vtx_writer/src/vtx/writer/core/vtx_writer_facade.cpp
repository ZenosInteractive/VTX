#include "vtx_schema_generated.h"
#include "vtx_schema.pb.h"
#include "vtx/writer/core/vtx_writer_facade.h"

#include "vtx/writer/core/writer.h"

#include "vtx/writer/policies/formatters/flatbuffers_vtx_policy.h"
#include "vtx/writer/policies/formatters/protobuff_vtx_policy.h"
#include "vtx/writer/policies/sinks/file_sink.h"

namespace VTX {

    template <typename SinkPolicyType>
    class WriterFacadeImpl : public IVtxWriterFacade {
    public:
        WriterFacadeImpl(typename ReplayWriter<SinkPolicyType>::Config internal_config)
            : writer_(internal_config) 
        {
        }

        void RecordFrame(VTX::Frame& native_frame, const VTX::GameTime::GameTimeRegister& game_time_register) override {
            writer_.RecordFrame(native_frame, game_time_register);
        }

        void Flush() override {
            writer_.Flush();
        }

        void Stop() override {
            writer_.Stop();
        }
        
        VTX::SchemaRegistry& GetSchema()override
        {
            return writer_.GetRegistry();
        }

    private:
        ReplayWriter<SinkPolicyType> writer_;
    };
    
    
    std::unique_ptr<IVtxWriterFacade> CreateFlatBuffersWriterFacade(
        const WriterFacadeConfig& config) 
    {
        using SinkType = ChunkedFileSink<VTX::FlatBuffersVtxPolicy>;
        
        ReplayWriter<SinkType>::Config internal_cfg;
        internal_cfg.default_fps = config.default_fps;
        internal_cfg.is_increasing = config.is_increasing;
        internal_cfg.chunker_config.max_frames = config.chunk_max_frames;
        internal_cfg.chunker_config.max_bytes = config.chunk_max_bytes;
        
        internal_cfg.sink_config.filename = config.output_filepath;
        internal_cfg.schema_json_path = config.schema_json_path;
         internal_cfg.sink_config.header_config.replay_name = config.replay_name;
        internal_cfg.sink_config.header_config.replay_uuid = config.replay_uuid;
        internal_cfg.sink_config.b_use_compression = config.use_compression;

        return std::make_unique<WriterFacadeImpl<SinkType>>(internal_cfg);
    }

    std::unique_ptr<IVtxWriterFacade> CreateProtobuffWriterFacade(
        const WriterFacadeConfig& config) 
    {
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

        internal_cfg.sink_config.b_use_compression = config.use_compression;
        return std::make_unique<WriterFacadeImpl<SinkType>>(internal_cfg);
    }
}