

#include "vtx/writer/sources/detail/websocket_client.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>

namespace VTX::ws_detail {

    struct WebSocketClient::Impl {
        ix::WebSocket                      ws;
        mutable std::mutex                 mtx;
        std::condition_variable            cv;
        std::queue<std::vector<std::byte>> queue;
        bool                               open     = false;
        bool                               finished = false;

        Impl() { ix::initNetSystem(); }

        ~Impl() {
            ws.stop();
            ix::uninitNetSystem();
        }
    };

    WebSocketClient::WebSocketClient() : impl_(std::make_unique<Impl>()) {}

    WebSocketClient::~WebSocketClient() = default;

    bool WebSocketClient::Connect(const std::string& url) {
        impl_->ws.setUrl(url);
        impl_->ws.disableAutomaticReconnection();

        ix::SocketTLSOptions tls;
        tls.caFile = "SYSTEM";
        impl_->ws.setTLSOptions(tls);

        Impl* impl = impl_.get();
        impl_->ws.setOnMessageCallback([impl](const ix::WebSocketMessagePtr& msg) {
            std::scoped_lock lock(impl->mtx);
            switch (msg->type) {
            case ix::WebSocketMessageType::Open:
                impl->open = true;
                break;
            case ix::WebSocketMessageType::Message: {
                const auto* p = reinterpret_cast<const std::byte*>(msg->str.data());
                impl->queue.emplace(p, p + msg->str.size());
                break;
            }
            case ix::WebSocketMessageType::Close:
            case ix::WebSocketMessageType::Error:
                impl->finished = true;
                break;
            default:
                break;
            }
            impl->cv.notify_all();
        });

        impl_->ws.start();

        std::unique_lock<std::mutex> lock(impl_->mtx);
        const bool signalled = impl_->cv.wait_for(lock, std::chrono::seconds(10),
                                                  [impl] { return impl->open || impl->finished; });
        return signalled && impl_->open && !impl_->finished;
    }

    bool WebSocketClient::ReadMessage(std::vector<std::byte>& out) {
        std::unique_lock<std::mutex> lock(impl_->mtx);
        impl_->cv.wait(lock, [this] { return !impl_->queue.empty() || impl_->finished; });

        if (!impl_->queue.empty()) {
            out = std::move(impl_->queue.front());
            impl_->queue.pop();
            return true;
        }
        return false;
    }

    void WebSocketClient::Close() {
        impl_->ws.stop();
    }

    bool WebSocketClient::IsConnected() const {
        std::scoped_lock lock(impl_->mtx);
        return impl_->open && !impl_->finished;
    }

} // namespace VTX::ws_detail
