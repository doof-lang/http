#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "doof_runtime.hpp"

namespace std_::event::index {
template <typename T>
struct ChannelReceiver;
template <typename T>
struct ChannelSender;
}

namespace std_::http::types {
struct HttpError;
}

namespace std_::http::websocket {
struct WebSocketBinary;
struct WebSocketClose;
struct WebSocketCloseCommand;
struct WebSocketConnection;
struct WebSocketError;
struct WebSocketOpen;
struct WebSocketPing;
struct WebSocketSendBinary;
struct WebSocketSendText;
struct WebSocketText;
struct WebSocketWritable;
}

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
    using PublicCommand = std::variant<
        std::shared_ptr<std_::http::websocket::WebSocketSendText>,
        std::shared_ptr<std_::http::websocket::WebSocketSendBinary>,
        std::shared_ptr<std_::http::websocket::WebSocketPing>,
        std::shared_ptr<std_::http::websocket::WebSocketCloseCommand>
    >;
    using EventSender = std_::event::index::ChannelSender<PublicEvent>;
    using CommandReceiver = std_::event::index::ChannelReceiver<PublicCommand>;

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
    void attachChannels(
        std::shared_ptr<std_::http::websocket::WebSocketConnection> connection,
        std::shared_ptr<EventSender> eventSender,
        std::shared_ptr<CommandReceiver> commandReceiver
    );
    void resumeInboundReads();
    int32_t state() const;

private:
    std::shared_ptr<class NativeHttpWebSocketConnectionImpl> impl_;
};
