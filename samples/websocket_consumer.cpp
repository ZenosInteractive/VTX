// websocket_consumer.cpp -- Connects (as a WebSocket client) to a server
// that pushes one JSON frame per message, and records the stream into a
// .vtx replay via the FlatBuffers writer facade.
//
// Each WebSocket message is a single frame, e.g.:
//
//     {
//       "game_time": 0.0167,
//       "entities": [
//         {"id": "player_0", "type_id": 0,
//          "floats": [100.5],
//          "translation": [0.0, 0.0, 50.0]}
//       ]
//     }
//
// The JSON -> C++ step is declarative: plain structs (WsFrame / WsEntity)
// plus VTX::JsonMapping<T> specializations are walked by
// VTX::UniversalDeserializer -- the same pattern as samples/arena_mappings.h.
//
// Usage
//   vtx_sample_websocket_consumer [url] [output.vtx] [schema.json]
//
// Args (all optional, positional)
//   url         default: ws://127.0.0.1:8765/
//               use wss://... for a TLS connection
//   output.vtx  default: websocket_output.vtx
//   schema.json default: content/writer/arena/arena_schema.json
//
// Test it with the bundled Python server:
//   python samples/websocket_server.py
//   vtx_sample_websocket_consumer ws://127.0.0.1:8765/ out.vtx schema.json
//
// Build
//   Link: vtx_writer (vtx_common transitively brings in nlohmann/json).

#include <cstdlib>
#include <span>
#include <string>
#include <tuple>
#include <vector>

#include <nlohmann/json.hpp>

#include "vtx/common/adapters/json/json_policy.h"
#include "vtx/common/readers/frame_reader/universal_deserializer.h"
#include "vtx/common/vtx_types.h"
#include "vtx/writer/core/vtx_writer_facade.h"
#include "vtx/writer/sources/websocket_frame_source.h"

// ===================================================================
//  Wire data model -- plain structs mirroring the JSON each message carries.
// ===================================================================

struct WsEntity {
    std::string id;
    int type_id = -1;
    std::vector<float> floats;
    std::vector<double> translation;
};

struct WsFrame {
    float game_time = 0.0f;
    std::vector<WsEntity> entities;
};

// ===================================================================
//  JsonMapping<T> specializations  (JSON key -> C++ member)
// ===================================================================

template <>
struct VTX::JsonMapping<WsEntity> {
    static constexpr auto GetFields() {
        return std::make_tuple(MakeField("id", &WsEntity::id), MakeField("type_id", &WsEntity::type_id),
                               MakeField("floats", &WsEntity::floats),
                               MakeField("translation", &WsEntity::translation));
    }
};

template <>
struct VTX::JsonMapping<WsFrame> {
    static constexpr auto GetFields() {
        return std::make_tuple(MakeField("game_time", &WsFrame::game_time), MakeField("entities", &WsFrame::entities));
    }
};

namespace {

    struct JsonWebSocketAdapter {
        bool ParseFrame(std::span<const std::byte> message, VTX::Frame& out_frame,
                        VTX::GameTime::GameTimeRegister& out_time) const {
            const char* text = reinterpret_cast<const char*>(message.data());
            const auto json = nlohmann::json::parse(text, text + message.size(), nullptr,
                                                    /*allow_exceptions=*/false);
            if (json.is_discarded())
                return false;

            WsFrame wf;
            try {
                wf = VTX::UniversalDeserializer<>::Load<WsFrame>(VTX::JsonAdapter(json));
            } catch (const std::exception& e) {
                VTX_ERROR("WebSocket frame decode failed: {}", e.what());
                return false;
            }

            out_time.game_time = wf.game_time;

            auto& bucket = out_frame.CreateBucket("entity");
            for (const auto& e : wf.entities) {
                VTX::PropertyContainer pc;
                pc.entity_type_id = e.type_id;
                pc.float_properties = e.floats;

                if (e.translation.size() == 3) {
                    VTX::Transform t;
                    t.translation = {e.translation[0], e.translation[1], e.translation[2]};
                    pc.transform_properties.push_back(t);
                }

                bucket.unique_ids.push_back(e.id);
                bucket.entities.push_back(std::move(pc));
            }
            return true;
        }
    };

} // namespace

int main(int argc, char* argv[]) {
    const std::string url = (argc > 1) ? argv[1] : "ws://127.0.0.1:8765/";
    const std::string output_path = (argc > 2) ? argv[2] : "websocket_output.vtx";
    const std::string schema_path = (argc > 3) ? argv[3] : "content/writer/arena/arena_schema.json";

    // 1. Build the streaming WebSocket data source.
    VTX::WebSocketFrameDataSource<JsonWebSocketAdapter>::Config src_cfg;
    src_cfg.url = url;
    VTX::WebSocketFrameDataSource<JsonWebSocketAdapter> source(src_cfg);
    if (!source.Initialize()) {
        VTX_ERROR("Failed to connect / handshake with {}", url);
        return 1;
    }

    // 2. Build the writer (FlatBuffers sink to disk).
    VTX::WriterFacadeConfig writer_cfg;
    writer_cfg.output_filepath = output_path;
    writer_cfg.schema_json_path = schema_path;
    writer_cfg.replay_name = "WebSocketConsumerSample";
    writer_cfg.replay_uuid = "websocket-0001";
    writer_cfg.default_fps = 60.0f;
    writer_cfg.chunk_max_frames = 500;
    writer_cfg.use_compression = true;

    auto writer = VTX::CreateFlatBuffersWriterFacade(writer_cfg);
    if (!writer) {
        VTX_ERROR("Failed to create writer. Schema path: {}", schema_path);
        return 1;
    }

    // 3. Pull-driven loop: every message becomes a recorded frame.
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

    VTX_INFO("WebSocket consumer: processed {} frames into {}", processed, output_path);
    return 0;
}
