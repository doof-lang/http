#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <variant>
#include <vector>

#include "doof_runtime.hpp"

namespace std_::http::types {
struct HttpError;
}

namespace std_::http::websocket {
struct WebSocketBinary;
struct WebSocketClose;
struct WebSocketConnection;
struct WebSocketError;
struct WebSocketOpen;
struct WebSocketText;
struct WebSocketWritable;
}

namespace doof_http_detail {

// Posts WebSocket events to a Doof handler on the handler's owning actor.
// It counts events posted but not yet handled so a connection can pause
// inbound reads, and keeps the root event loop alive until the terminal
// event is posted.
template <typename Event>
class PostedEventWindow {
public:
    using Handler = doof::callback<void(Event)>;

    ~PostedEventWindow() {
        releaseKeepAlive();
    }

    void attach(Handler handler, int32_t capacity) {
        bool retain = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            handler_ = std::move(handler);
            capacity_ = std::max<int32_t>(capacity, 1);
            retain = !keepingAlive_;
            keepingAlive_ = true;
        }
        if (retain) {
            doof::detail::ApplicationDomain::shared().add_keep_alive_source(true);
        }
    }

    // Returns 0 to keep reading, 1 to pause reads until `handled` reports a
    // resume, or 2 when the handler's actor cannot accept events. Terminal
    // events are never shed for a merely busy owner, and release the handler.
    int32_t deliver(Event event, bool terminal) {
        Handler handler;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            handler = handler_;
            if (terminal) {
                handler_ = {};
            }
            if (!handler) {
                return 0;
            }
            ++pending_;
        }

        int32_t pressure = 0;
        auto posted = handler.tryPost(event);
        if (doof::is_success(posted)) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!terminal && pending_ >= capacity_) {
                paused_ = true;
                pressure = 1;
            }
        } else if (terminal && doof::failure_error(posted) == "full") {
            handler.dispatch(std::move(event));
        } else {
            std::lock_guard<std::mutex> lock(mutex_);
            --pending_;
            pressure = terminal ? 0 : 2;
        }
        if (terminal) {
            releaseKeepAlive();
        }
        return pressure;
    }

    // Called on the handler's actor after each event. Returns true when
    // paused reads should resume because half the window has drained.
    bool handled() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_ > 0) {
            --pending_;
        }
        if (paused_ && pending_ <= capacity_ / 2) {
            paused_ = false;
            return true;
        }
        return false;
    }

private:
    void releaseKeepAlive() {
        bool release = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            release = keepingAlive_;
            keepingAlive_ = false;
        }
        if (release) {
            doof::detail::ApplicationDomain::shared().remove_keep_alive_source(true);
        }
    }

    std::mutex mutex_;
    Handler handler_;
    int32_t capacity_ = 1;
    int32_t pending_ = 0;
    bool paused_ = false;
    bool keepingAlive_ = false;
};

}  // namespace doof_http_detail

class NativeHttpClient {
public:
    NativeHttpClient();
    ~NativeHttpClient();

    doof::Result<int32_t, std::string> perform(
        const std::string& method,
        const std::string& url,
        const std::string& requestHeaders,
        std::shared_ptr<std::vector<uint8_t>> body,
        int32_t timeoutMs,
        bool followRedirects
    );

    std::string responseStatusText() const;
    std::string responseHeadersText() const;
    std::shared_ptr<std::vector<uint8_t>> responseBody() const;

private:
    class Impl;
    std::shared_ptr<Impl> impl_;
};

enum class NativeHttpWebSocketState : int32_t {
    Connecting = 0,
    Open = 1,
    Closing = 2,
    Closed = 3,
    Error = 4,
};

enum class NativeHttpWebSocketEventKind : int32_t {
    Open = 0,
    Text = 1,
    Binary = 2,
    Writable = 3,
    Close = 4,
    Error = 5,
};

class NativeHttpWebSocketEvent {
public:
    NativeHttpWebSocketEvent(
        NativeHttpWebSocketEventKind kind,
        std::string text,
        std::shared_ptr<std::vector<uint8_t>> bytes,
        int32_t code,
        std::string reason,
        bool wasClean,
        std::string error
    );

    int32_t kind() const;
    std::string text() const;
    std::shared_ptr<std::vector<uint8_t>> bytes() const;
    int32_t code() const;
    std::string reason() const;
    bool wasClean() const;
    std::string error() const;

private:
    NativeHttpWebSocketEventKind kind_;
    std::string text_;
    std::shared_ptr<std::vector<uint8_t>> bytes_;
    int32_t code_;
    std::string reason_;
    bool wasClean_;
    std::string error_;
};

class NativeHttpWebSocketConnection : public std::enable_shared_from_this<NativeHttpWebSocketConnection> {
public:
    using PublicEvent = std::variant<
        std::shared_ptr<std_::http::websocket::WebSocketOpen>,
        std::shared_ptr<std_::http::websocket::WebSocketText>,
        std::shared_ptr<std_::http::websocket::WebSocketBinary>,
        std::shared_ptr<std_::http::websocket::WebSocketWritable>,
        std::shared_ptr<std_::http::websocket::WebSocketClose>,
        std::shared_ptr<std_::http::websocket::WebSocketError>
    >;
    using EventHandler = doof::callback<void(PublicEvent)>;
    static doof::Result<std::shared_ptr<NativeHttpWebSocketConnection>, std::string> connect(
        const std::string& url,
        const std::string& requestHeaders,
        int32_t timeoutMs,
        int32_t outboundCapacity,
        int32_t eventCapacity
    );

    explicit NativeHttpWebSocketConnection(std::shared_ptr<class NativeHttpWebSocketConnectionImpl> impl);
    ~NativeHttpWebSocketConnection();

    void start();
    doof::Result<void, std::string> sendText(const std::string& text);
    doof::Result<void, std::string> sendBinary(std::shared_ptr<std::vector<uint8_t>> bytes);
    doof::Result<void, std::string> ping();
    doof::Result<void, std::string> close(int32_t code, const std::string& reason);
    void attachHandler(
        std::shared_ptr<std_::http::websocket::WebSocketConnection> connection,
        EventHandler onEvent,
        int32_t eventCapacity
    );
    void eventHandled();
    void resumeInboundReads();
    int32_t state() const;

private:
    std::shared_ptr<class NativeHttpWebSocketConnectionImpl> impl_;
};
