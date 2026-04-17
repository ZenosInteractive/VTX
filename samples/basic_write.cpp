// basic_write.cpp -- Record synthetic frames into a .vtx replay file.
//
// Purpose
//   Minimal writer demo: builds 100 VTX::Frame objects in memory (no external
//   data source) and feeds them to a FlatBuffers writer facade.  Useful as a
//   smoke test for the writer API and as a template when hand-crafting
//   PropertyContainers.
//
// Args
//   argv[1] -- schema JSON path       (default: content/writer/arena/arena_schema.json)
//   argv[2] -- output .vtx path       (default: sample_output.vtx)
//
// Build
//   Link against vtx_writer (vtx_common is transitive).

#include "vtx/writer/core/vtx_writer_facade.h"
#include "vtx/common/vtx_types.h"

#include <string>

int main(int argc, char* argv[])
{
    const std::string schema_path = (argc > 1)
        ? argv[1]
        : "content/writer/arena/arena_schema.json";
    const std::string output_path = (argc > 2)
        ? argv[2]
        : "sample_output.vtx";

    // Configure the writer.
    VTX::WriterFacadeConfig config;
    config.output_filepath  = output_path;
    config.schema_json_path = schema_path;
    config.replay_name      = "BasicWriteSample";
    config.replay_uuid      = "sample-0001";
    config.default_fps      = 60.0f;
    config.chunk_max_frames = 500;
    config.use_compression  = true;

    // Create a FlatBuffers writer (use CreateProtobuffWriterFacade for Protobuf).
    auto writer = VTX::CreateFlatBuffersWriterFacade(config);
    if (!writer) {
        VTX_ERROR("Failed to create writer. Check schema path: {}", schema_path);
        return 1;
    }

    // Record 100 synthetic frames.
    for (int i = 0; i < 100; ++i) {
        VTX::Frame frame;

        // Create a bucket and add one entity per frame.
        VTX::Bucket& bucket = frame.CreateBucket("Players");

        VTX::PropertyContainer entity;
        entity.entity_type_id = 0;

        // Add a float property (index 0).
        entity.float_properties.push_back(static_cast<float>(i) * 1.5f);

        // Add a transform property (index 0).
        VTX::Transform t;
        t.translation = { static_cast<double>(i), 0.0, 50.0 };
        t.rotation    = { 0.0f, 0.0f, 0.0f, 1.0f };
        t.scale       = { 1.0, 1.0, 1.0 };
        entity.transform_properties.push_back(t);

        bucket.unique_ids.push_back("player_" + std::to_string(i % 10));
        bucket.entities.push_back(std::move(entity));

        // Build the game time register for this frame.
        VTX::GameTime::GameTimeRegister game_time;
        game_time.game_time = static_cast<float>(i) / 60.0f;

        writer->RecordFrame(frame, game_time);
    }

    writer->Flush();
    writer->Stop();

    VTX_INFO("Wrote 100 frames to {}", output_path);
    return 0;
}
