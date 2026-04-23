#pragma once

#include <memory>
#include <string>

#include "schema_sanitizer.h"
#include "vtx/common/vtx_types.h"
#include "vtx/common/readers/schema_reader/schema_registry.h"


namespace VTX {


    class IVtxWriterFacade {
    public:
        virtual ~IVtxWriterFacade() = default;

        virtual void RecordFrame(VTX::Frame& native_frame,
                                 const VTX::GameTime::GameTimeRegister& game_time_register) = 0;
        virtual void Flush() = 0;
        virtual void Stop() = 0;
        virtual VTX::SchemaRegistry& GetSchema() = 0;
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
    };
    enum class SerializationFormat : uint8_t {
        Flatbuffers,
        Protobuffs,
    };

    std::unique_ptr<IVtxWriterFacade> CreateFlatBuffersWriterFacade(const WriterFacadeConfig& config);

    std::unique_ptr<IVtxWriterFacade> CreateProtobuffWriterFacade(const WriterFacadeConfig& config);
} // namespace VTX
