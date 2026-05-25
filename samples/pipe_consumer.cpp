// pipe_consumer.cpp -- Reads length-prefixed JSON frames from a pipe (stdin
// or a filesystem path) via PipeFrameDataSource and writes them to a .vtx
// replay using the FlatBuffers writer facade.
//
// Usage
//   vtx_sample_pipe_consumer [pipe] [output.vtx] [schema.json]
//
//     pipe        '-' or empty   => read from stdin
//                 'serve:<path>' => create the named pipe at <path> and wait
//                                   for a producer to connect (VTX is the
//                                   server -- the mode for an independent
//                                   external producer, e.g. a game injector)
//                 '<path>'       => connect to a pipe a producer already made
//     output.vtx   default: pipe_output.vtx
//     schema.json  default: content/writer/arena/arena_schema.json
//
// Examples
//   # Shell pipeline (anonymous pipe, single launcher):
//   vtx_sample_pipe_producer 200 | vtx_sample_pipe_consumer - out.vtx schema.json
//
//   # Independent processes via a named pipe (Windows):
//   vtx_sample_pipe_consumer serve:\\.\pipe\vtx out.vtx schema.json
//   vtx_sample_pipe_producer 200 > \\.\pipe\vtx
//
//   # Independent processes via a FIFO (POSIX):
//   vtx_sample_pipe_consumer serve:/tmp/vtx.fifo out.vtx schema.json &
//   vtx_sample_pipe_producer 200 > /tmp/vtx.fifo
//
// Build
//   Link: vtx_writer (vtx_common transitively brings in nlohmann/json).

#include <cstring>
#include <span>
#include <string>

#include <nlohmann/json.hpp>

#include "vtx/common/vtx_types.h"
#include "vtx/writer/core/vtx_writer_facade.h"
#include "vtx/writer/sources/pipe_frame_source.h"

namespace {

    /// Adapter that turns one JSON payload into a VTX::Frame + GameTimeRegister.
    struct JsonPipeAdapter {
        bool ParseFrame(std::span<const std::byte> payload, VTX::Frame& out_frame,
                        VTX::GameTime::GameTimeRegister& out_time) const {
            const char* text_begin = reinterpret_cast<const char*>(payload.data());
            const auto json = nlohmann::json::parse(text_begin, text_begin + payload.size(), nullptr,
                                                    /*allow_exceptions=*/false);
            if (json.is_discarded())
                return false;

            // Timing.
            if (json.contains("game_time"))
                out_time.game_time = json.at("game_time").get<float>();

            // Entities -> one default bucket called "entity".
            auto& bucket = out_frame.CreateBucket("entity");
            if (!json.contains("entities") || !json.at("entities").is_array())
                return true; // empty frame is allowed

            for (const auto& e : json.at("entities")) {
                VTX::PropertyContainer pc;
                pc.entity_type_id = e.value("type_id", -1);

                if (e.contains("floats"))
                    for (const auto& f : e.at("floats"))
                        pc.float_properties.push_back(f.get<float>());

                if (e.contains("translation") && e.at("translation").is_array() && e.at("translation").size() == 3) {
                    VTX::Transform t;
                    t.translation = {e.at("translation")[0].get<double>(), e.at("translation")[1].get<double>(),
                                     e.at("translation")[2].get<double>()};
                    pc.transform_properties.push_back(t);
                }

                bucket.unique_ids.push_back(e.value("id", std::string {}));
                bucket.entities.push_back(std::move(pc));
            }
            return true;
        }
    };

} // namespace

int main(int argc, char* argv[]) {
    const std::string pipe_arg = (argc > 1) ? argv[1] : "-";
    const std::string output_path = (argc > 2) ? argv[2] : "pipe_output.vtx";
    const std::string schema_path = (argc > 3) ? argv[3] : "content/writer/arena/arena_schema.json";

    //   "-"            -> stdin
    //   "serve:<path>" -> create the named pipe and wait for a producer
    //   "<path>"       -> connect to a pipe a producer already created
    VTX::PipeFrameDataSource<JsonPipeAdapter>::Config src_cfg;
    std::string mode_label;
    if (pipe_arg == "-" || pipe_arg.empty()) {
        mode_label = "<stdin>";
    } else if (pipe_arg.rfind("serve:", 0) == 0) {
        src_cfg.pipe_path = pipe_arg.substr(6);
        src_cfg.as_server = true;
        mode_label = "serve " + src_cfg.pipe_path;
    } else {
        src_cfg.pipe_path = pipe_arg;
        mode_label = "connect " + pipe_arg;
    }

    VTX::PipeFrameDataSource<JsonPipeAdapter> source(src_cfg);

    if (src_cfg.as_server)
        VTX_INFO("Pipe consumer: waiting for a producer on {}", src_cfg.pipe_path);

    if (!source.Initialize()) {
        VTX_ERROR("Failed to open pipe: {}", mode_label);
        return 1;
    }

    VTX::WriterFacadeConfig writer_cfg;
    writer_cfg.output_filepath = output_path;
    writer_cfg.schema_json_path = schema_path;
    writer_cfg.replay_name = "PipeConsumerSample";
    writer_cfg.replay_uuid = "pipe-0001";
    writer_cfg.default_fps = 60.0f;
    writer_cfg.chunk_max_frames = 500;
    writer_cfg.use_compression = true;

    auto writer = VTX::CreateFlatBuffersWriterFacade(writer_cfg);
    if (!writer) {
        VTX_ERROR("Failed to create writer. Schema path: {}", schema_path);
        return 1;
    }

    VTX::Frame frame;
    VTX::GameTime::GameTimeRegister time;
    size_t processed = 0;
    while (source.GetNextFrame(frame, time)) {
        writer->RecordFrame(frame, time);
        ++processed;
        frame = VTX::Frame {};
        time = VTX::GameTime::GameTimeRegister {};
    }

    writer->Flush();
    writer->Stop();

    VTX_INFO("Pipe consumer: processed {} frames into {}", processed, output_path);
    return 0;
}
