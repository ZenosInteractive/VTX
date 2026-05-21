#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "vtx/common/vtx_types.h"
#include "vtx/writer/core/vtx_data_source.h"
#include "vtx/writer/sources/detail/websocket_client.h"
#include "vtx/writer/sources/frame_payload_adapter.h"

namespace VTX {

    template <IFramePayloadAdapter Adapter>
    class WebSocketFrameDataSource : public IFrameDataSource {
    public:
        struct Config {
            std::string url;
            Adapter     adapter;
        };

        explicit WebSocketFrameDataSource(Config config) : config_(std::move(config)) {}

        bool Initialize() override { return client_.Connect(config_.url); }

        bool GetNextFrame(VTX::Frame& out_frame, VTX::GameTime::GameTimeRegister& out_time) override {
            // ReadMessage blocks until one full message arrives and returns false once the stream has ended and all buffered messages are
            if (!client_.ReadMessage(message_))
                return false;

            const std::span<const std::byte> payload(message_.data(), message_.size());
            return config_.adapter.ParseFrame(payload, out_frame, out_time);
        }

        size_t GetExpectedTotalFrames() const override { return 0; }

    private:
        Config config_;
        ws_detail::WebSocketClient client_;
        std::vector<std::byte> message_;
    };

} // namespace VTX
