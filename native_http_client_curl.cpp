#include "native_http_client.hpp"
#include "native_event.hpp"
#include "websocket.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>
#include <sys/select.h>

#include <curl/curl.h>

#ifndef CURLWS_TEXT
#define CURLWS_TEXT (1<<0)
#define CURLWS_BINARY (1<<1)
#define CURLWS_CONT (1<<2)
#define CURLWS_CLOSE (1<<3)
#define CURLWS_PING (1<<4)
#define CURLWS_PONG (1<<5)
#endif

namespace {

void ensureCurlGlobalInit() {
    static std::once_flag initFlag;
    std::call_once(initFlag, []() {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    });
}

bool startsWith(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

std::string classifyError(CURLcode code) {
    switch (code) {
        case CURLE_UNSUPPORTED_PROTOCOL:
        case CURLE_URL_MALFORMAT:
            return "invalid-url";
        case CURLE_COULDNT_RESOLVE_HOST:
        case CURLE_COULDNT_RESOLVE_PROXY:
            return "dns";
        case CURLE_COULDNT_CONNECT:
            return "connect";
        case CURLE_OPERATION_TIMEDOUT:
            return "timeout";
        case CURLE_PEER_FAILED_VERIFICATION:
        case CURLE_SSL_CONNECT_ERROR:
        case CURLE_SSL_CERTPROBLEM:
        case CURLE_SSL_CACERT_BADFILE:
        case CURLE_SSL_ISSUER_ERROR:
            return "tls";
        default:
            return "transport";
    }
}

std::string statusText(long statusCode) {
    switch (statusCode) {
        case 100: return "Continue";
        case 101: return "Switching Protocols";
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 307: return "Temporary Redirect";
        case 308: return "Permanent Redirect";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 410: return "Gone";
        case 415: return "Unsupported Media Type";
        case 418: return "I'm a teapot";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default: return "HTTP " + std::to_string(statusCode);
    }
}

std::string encodeError(CURLcode code, std::string_view message) {
    return classifyError(code) + "|" + std::to_string(static_cast<int>(code)) + "|" + std::string(message);
}

curl_slist* parseHeaderList(const std::string& headerText) {
    curl_slist* head = nullptr;
    std::string current;
    current.reserve(headerText.size());

    for (char ch : headerText) {
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            if (!current.empty()) {
                head = curl_slist_append(head, current.c_str());
                current.clear();
            }
            continue;
        }
        current.push_back(ch);
    }

    if (!current.empty()) {
        head = curl_slist_append(head, current.c_str());
    }

    return head;
}

long normalizedTimeoutMs(int32_t timeoutMs) {
    return timeoutMs > 0 ? static_cast<long>(timeoutMs) : 30000L;
}

std::vector<uint8_t> encodeClosePayload(int32_t code, const std::string& reason) {
    std::vector<uint8_t> payload;
    payload.reserve(2 + reason.size());
    payload.push_back(static_cast<uint8_t>((code >> 8) & 0xff));
    payload.push_back(static_cast<uint8_t>(code & 0xff));
    payload.insert(payload.end(), reason.begin(), reason.end());
    return payload;
}

std::shared_ptr<std_::http::types::HttpError> parseHttpError(const std::string& raw) {
    const size_t first = raw.find('|');
    if (first == std::string::npos) {
        return std::make_shared<std_::http::types::HttpError>("transport", "0", raw);
    }
    const std::string kind = raw.substr(0, first);
    const std::string remainder = raw.substr(first + 1);
    const size_t second = remainder.find('|');
    if (second == std::string::npos) {
        return std::make_shared<std_::http::types::HttpError>(kind, "0", remainder);
    }
    return std::make_shared<std_::http::types::HttpError>(
        kind,
        remainder.substr(0, second),
        remainder.substr(second + 1)
    );
}

} // namespace

NativeHttpWebSocketEvent::NativeHttpWebSocketEvent(
    NativeHttpWebSocketEventKind kind,
    std::string text,
    std::shared_ptr<std::vector<uint8_t>> bytes,
    int32_t code,
    std::string reason,
    bool wasClean,
    std::string error
) : kind_(kind),
    text_(std::move(text)),
    bytes_(bytes ? std::move(bytes) : std::make_shared<std::vector<uint8_t>>()),
    code_(code),
    reason_(std::move(reason)),
    wasClean_(wasClean),
    error_(std::move(error)) {}

int32_t NativeHttpWebSocketEvent::kind() const { return static_cast<int32_t>(kind_); }
std::string NativeHttpWebSocketEvent::text() const { return text_; }
std::shared_ptr<std::vector<uint8_t>> NativeHttpWebSocketEvent::bytes() const { return bytes_; }
int32_t NativeHttpWebSocketEvent::code() const { return code_; }
std::string NativeHttpWebSocketEvent::reason() const { return reason_; }
bool NativeHttpWebSocketEvent::wasClean() const { return wasClean_; }
std::string NativeHttpWebSocketEvent::error() const { return error_; }

class NativeHttpClient::Impl {
public:
    Impl() : responseBody_(std::make_shared<std::vector<uint8_t>>()) {
        ensureCurlGlobalInit();
    }

    doof::Result<int32_t, std::string> perform(
        const std::string& method,
        const std::string& url,
        const std::string& requestHeaders,
        std::shared_ptr<std::vector<uint8_t>> body,
        int32_t timeoutMs,
        bool followRedirects
    ) {
        const bool hasBody = body != nullptr;
        responseStatusText_.clear();
        responseHeadersText_.clear();
        responseBody_ = std::make_shared<std::vector<uint8_t>>();

        CURL* handle = curl_easy_init();
        if (handle == nullptr) {
            return doof::Result<int32_t, std::string>::failure("internal|0|failed to create libcurl handle");
        }

        curl_slist* headerList = parseHeaderList(requestHeaders);

        curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
        curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, followRedirects ? 1L : 0L);
        curl_easy_setopt(handle, CURLOPT_MAXREDIRS, 10L);
        curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, normalizedTimeoutMs(timeoutMs));
        curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(handle, CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(handle, CURLOPT_USERAGENT, "doof-http-client/0.1");
        curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, &NativeHttpClient::Impl::writeBodyCallback);
        curl_easy_setopt(handle, CURLOPT_WRITEDATA, this);
        curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, &NativeHttpClient::Impl::writeHeaderCallback);
        curl_easy_setopt(handle, CURLOPT_HEADERDATA, this);

        if (headerList != nullptr) {
            curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headerList);
        }

        const bool isGet = method == "GET";
        const bool isPost = method == "POST";
        if (isPost) {
            curl_easy_setopt(handle, CURLOPT_POST, 1L);
        } else if (isGet && body == nullptr) {
            curl_easy_setopt(handle, CURLOPT_HTTPGET, 1L);
        } else {
            curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, method.c_str());
        }

        if (hasBody) {
            curl_easy_setopt(handle, CURLOPT_POSTFIELDS, reinterpret_cast<const char*>(body->data()));
            curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body->size()));
        }

        const CURLcode result = curl_easy_perform(handle);
        if (result != CURLE_OK) {
            const std::string encoded = encodeError(result, curl_easy_strerror(result));
            if (headerList != nullptr) {
                curl_slist_free_all(headerList);
            }
            curl_easy_cleanup(handle);
            return doof::Result<int32_t, std::string>::failure(encoded);
        }

        long statusCode = 0;
        curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &statusCode);
        responseStatusText_ = statusText(statusCode);

        if (headerList != nullptr) {
            curl_slist_free_all(headerList);
        }
        curl_easy_cleanup(handle);
        return doof::Result<int32_t, std::string>::success(static_cast<int32_t>(statusCode));
    }

    std::string responseStatusText() const { return responseStatusText_; }
    std::string responseHeadersText() const { return responseHeadersText_; }
    std::shared_ptr<std::vector<uint8_t>> responseBody() const { return responseBody_; }

private:
    static size_t writeBodyCallback(char* data, size_t size, size_t count, void* userData) {
        const std::size_t bytes = size * count;
        auto* self = static_cast<NativeHttpClient::Impl*>(userData);
        self->responseBody_->insert(self->responseBody_->end(), data, data + bytes);
        return bytes;
    }

    static size_t writeHeaderCallback(char* data, size_t size, size_t count, void* userData) {
        const std::size_t bytes = size * count;
        auto* self = static_cast<NativeHttpClient::Impl*>(userData);
        const std::string_view line(data, bytes);

        if (startsWith(line, "HTTP/")) {
            self->responseHeadersText_.clear();
            return bytes;
        }

        if (line == "\r\n") {
            return bytes;
        }

        self->responseHeadersText_.append(data, bytes);
        return bytes;
    }

    std::string responseStatusText_;
    std::string responseHeadersText_;
    std::shared_ptr<std::vector<uint8_t>> responseBody_;
};

NativeHttpClient::NativeHttpClient() : impl_(std::make_shared<Impl>()) {}
NativeHttpClient::~NativeHttpClient() = default;

doof::Result<int32_t, std::string> NativeHttpClient::perform(
    const std::string& method,
    const std::string& url,
    const std::string& requestHeaders,
    std::shared_ptr<std::vector<uint8_t>> body,
    int32_t timeoutMs,
    bool followRedirects
) {
    return impl_->perform(method, url, requestHeaders, std::move(body), timeoutMs, followRedirects);
}

std::string NativeHttpClient::responseStatusText() const { return impl_->responseStatusText(); }
std::string NativeHttpClient::responseHeadersText() const { return impl_->responseHeadersText(); }
std::shared_ptr<std::vector<uint8_t>> NativeHttpClient::responseBody() const { return impl_->responseBody(); }

class NativeHttpWebSocketConnectionImpl : public std::enable_shared_from_this<NativeHttpWebSocketConnectionImpl> {
public:
    static doof::Result<std::shared_ptr<NativeHttpWebSocketConnectionImpl>, std::string> connect(
        const std::string& url,
        const std::string& requestHeaders,
        int32_t timeoutMs,
        int32_t outboundCapacity,
        int32_t eventCapacity
    ) {
#if LIBCURL_VERSION_NUM >= 0x075600
        (void)outboundCapacity;
        (void)eventCapacity;
        ensureCurlGlobalInit();
        CURL* handle = curl_easy_init();
        if (handle == nullptr) {
            return doof::Result<std::shared_ptr<NativeHttpWebSocketConnectionImpl>, std::string>::failure(
                "internal|0|failed to create libcurl handle"
            );
        }

        curl_slist* headerList = parseHeaderList(requestHeaders);
        curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
        curl_easy_setopt(handle, CURLOPT_CONNECT_ONLY, 2L);
        curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, normalizedTimeoutMs(timeoutMs));
        curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(handle, CURLOPT_USERAGENT, "doof-http-client/0.1");
        if (headerList != nullptr) {
            curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headerList);
        }

        const CURLcode result = curl_easy_perform(handle);
        if (result != CURLE_OK) {
            const std::string encoded = encodeError(result, curl_easy_strerror(result));
            if (headerList != nullptr) {
                curl_slist_free_all(headerList);
            }
            curl_easy_cleanup(handle);
            return doof::Result<std::shared_ptr<NativeHttpWebSocketConnectionImpl>, std::string>::failure(encoded);
        }

        if (headerList != nullptr) {
            curl_slist_free_all(headerList);
        }

        auto connection = std::shared_ptr<NativeHttpWebSocketConnectionImpl>(
            new NativeHttpWebSocketConnectionImpl(handle)
        );
        return doof::Result<std::shared_ptr<NativeHttpWebSocketConnectionImpl>, std::string>::success(connection);
#else
        (void)url;
        (void)requestHeaders;
        (void)timeoutMs;
        (void)outboundCapacity;
        (void)eventCapacity;
        return doof::Result<std::shared_ptr<NativeHttpWebSocketConnectionImpl>, std::string>::failure(
            "unsupported|0|libcurl websocket support requires libcurl 7.86.0 or newer"
        );
#endif
    }

    ~NativeHttpWebSocketConnectionImpl() {
        requestStop();
        if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) {
            worker_.join();
        }
    }

    void start() {
        auto self = shared_from_this();
        worker_ = std::thread([self] {
            self->workerLoop();
        });
    }

    doof::Result<void, std::string> sendText(const std::string& text) {
        return enqueue(OutboundKind::Text, std::vector<uint8_t>(text.begin(), text.end()), 0, "");
    }

    doof::Result<void, std::string> sendBinary(std::shared_ptr<std::vector<uint8_t>> bytes) {
        return enqueue(OutboundKind::Binary, bytes ? *bytes : std::vector<uint8_t>(), 0, "");
    }

    doof::Result<void, std::string> ping() {
        return enqueue(OutboundKind::Ping, {}, 0, "");
    }

    doof::Result<void, std::string> close(int32_t code, const std::string& reason) {
        if (reason.size() > 123) {
            return doof::Result<void, std::string>::failure("invalid-close|0|websocket close reason exceeds 123 bytes");
        }
        return enqueue(OutboundKind::Close, encodeClosePayload(code, reason), code, reason);
    }

    void resumeInboundReads() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            inboundPaused_ = false;
        }
        cv_.notify_all();
    }

    int32_t state() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<int32_t>(state_);
    }

    void attachChannels(
        std::shared_ptr<std_::http::websocket::WebSocketConnection> connection,
        std::shared_ptr<NativeHttpWebSocketConnection::EventSender> eventSender,
        std::shared_ptr<NativeHttpWebSocketConnection::CommandReceiver> commandReceiver
    ) {
        std::shared_ptr<doof_event::NativeChannel> eventChannel = eventSender ? eventSender->native : nullptr;
        std::shared_ptr<doof_event::NativeChannel> commandChannel = commandReceiver ? commandReceiver->native : nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            connection_ = connection;
            eventChannel_ = eventChannel;
            commandChannel_ = commandChannel;
        }
        if (eventChannel) {
            auto weak = weak_from_this();
            eventChannel->registerNativeSenderReady([weak]() {
                if (auto self = weak.lock()) {
                    self->resumeInboundReads();
                }
            });
            eventChannel->registerNativeSenderClosed([weak]() {
                if (auto self = weak.lock()) {
                    (void)self->close(std_::http::websocket::WEBSOCKET_CLOSE_NORMAL, "");
                }
            });
        }
        if (commandChannel) {
            auto weak = weak_from_this();
            commandChannel->registerNativeReceiverMessage<NativeHttpWebSocketConnection::PublicCommand>(
                [weak](NativeHttpWebSocketConnection::PublicCommand command) {
                    if (auto self = weak.lock()) {
                        self->handleCommand(std::move(command));
                    }
                }
            );
            commandChannel->registerNativeReceiverClosed([weak]() {
                if (auto self = weak.lock()) {
                    (void)self->close(std_::http::websocket::WEBSOCKET_CLOSE_NORMAL, "");
                }
            });
        }
    }

private:
    enum class OutboundKind {
        Text,
        Binary,
        Ping,
        Close,
    };

    struct OutboundMessage {
        OutboundKind kind;
        std::vector<uint8_t> payload;
        int32_t closeCode = 0;
        std::string closeReason;
        size_t offset = 0;
    };

    explicit NativeHttpWebSocketConnectionImpl(CURL* handle)
        : handle_(handle) {
        setState(NativeHttpWebSocketState::Open);
    }

private:
    void handleCommand(NativeHttpWebSocketConnection::PublicCommand command) {
        pauseCommandChannel();
        doof::Result<void, std::string> result = doof::Result<void, std::string>::success();
        bool waitsForWritable = false;

        if (auto* text = std::get_if<std::shared_ptr<std_::http::websocket::WebSocketSendText>>(&command)) {
            result = sendText((*text)->text);
            waitsForWritable = result.isSuccess();
        } else if (auto* binary = std::get_if<std::shared_ptr<std_::http::websocket::WebSocketSendBinary>>(&command)) {
            result = sendBinary((*binary)->bytes);
            waitsForWritable = result.isSuccess();
        } else if (std::holds_alternative<std::shared_ptr<std_::http::websocket::WebSocketPing>>(command)) {
            result = ping();
        } else if (auto* closeCommand = std::get_if<std::shared_ptr<std_::http::websocket::WebSocketCloseCommand>>(&command)) {
            result = close((*closeCommand)->code, (*closeCommand)->reason);
            waitsForWritable = result.isSuccess();
        }

        if (result.isFailure()) {
            resumeCommandChannel();
            emitErrorToPublicChannel(result.error());
            return;
        }
        if (!waitsForWritable) {
            resumeCommandChannel();
        }
    }

    void pauseCommandChannel() {
        std::shared_ptr<doof_event::NativeChannel> commandChannel;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            commandChannel = commandChannel_;
        }
        if (commandChannel) {
            commandChannel->pauseReceiver();
        }
    }

    void resumeCommandChannel() {
        std::shared_ptr<doof_event::NativeChannel> commandChannel;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            commandChannel = commandChannel_;
        }
        if (commandChannel) {
            commandChannel->resumeReceiver();
        }
    }

    void emitErrorToPublicChannel(const std::string& raw) {
        emitPublicEvent(std::make_shared<std_::http::websocket::WebSocketError>(
            publicConnection(),
            parseHttpError(raw)
        ), false);
    }

    std::shared_ptr<std_::http::websocket::WebSocketConnection> publicConnection() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return connection_;
    }

    void requestStop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopRequested_ = true;
        }
        cv_.notify_all();
    }

    doof::Result<void, std::string> enqueue(
        OutboundKind kind,
        std::vector<uint8_t> payload,
        int32_t closeCode,
        std::string closeReason
    ) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_ == NativeHttpWebSocketState::Closed || state_ == NativeHttpWebSocketState::Error) {
                return doof::Result<void, std::string>::failure("closed|0|websocket is closed");
            }
            if (state_ == NativeHttpWebSocketState::Closing && kind != OutboundKind::Close) {
                return doof::Result<void, std::string>::failure("closing|0|websocket is closing");
            }
            if (outbound_.has_value()) {
                return doof::Result<void, std::string>::failure("backpressure|0|websocket outbound queue is full");
            }
            if (kind == OutboundKind::Close) {
                state_ = NativeHttpWebSocketState::Closing;
            }
            outbound_ = OutboundMessage {
                kind,
                std::move(payload),
                closeCode,
                std::move(closeReason),
                0,
            };
        }
        cv_.notify_all();
        return doof::Result<void, std::string>::success();
    }

    void workerLoop() {
#if LIBCURL_VERSION_NUM >= 0x075600
        while (true) {
            if (shouldStop()) {
                break;
            }

            bool didWork = false;
            if (hasOutbound()) {
                didWork = sendOne() || didWork;
            }
            if (!isInboundPaused()) {
                didWork = receiveOne() || didWork;
            }

            if (!didWork) {
                waitForActivity();
            }
        }
        cleanupHandle();
#endif
    }

    bool shouldStop() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return stopRequested_;
    }

    bool hasOutbound() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return outbound_.has_value();
    }

    bool isInboundPaused() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return inboundPaused_;
    }

    void waitForActivity() {
#if LIBCURL_VERSION_NUM >= 0x075600
        curl_socket_t socket = CURL_SOCKET_BAD;
        curl_easy_getinfo(handle_, CURLINFO_ACTIVESOCKET, &socket);

        if (socket == CURL_SOCKET_BAD) {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(25));
            return;
        }

        fd_set readfds;
        fd_set writefds;
        FD_ZERO(&readfds);
        FD_ZERO(&writefds);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!inboundPaused_) {
                FD_SET(socket, &readfds);
            }
            if (outbound_.has_value()) {
                FD_SET(socket, &writefds);
            }
        }

        timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 25000;
        (void)select(static_cast<int>(socket + 1), &readfds, &writefds, nullptr, &timeout);
#endif
    }

    bool sendOne() {
#if LIBCURL_VERSION_NUM >= 0x075600
        OutboundMessage current;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!outbound_.has_value()) {
                return false;
            }
            current = *outbound_;
        }

        const unsigned int flags = flagsFor(current.kind);
        size_t sent = 0;
        const uint8_t* data = current.payload.empty() ? nullptr : current.payload.data() + current.offset;
        const size_t remaining = current.payload.size() - current.offset;
        const CURLcode result = curl_ws_send(handle_, data, remaining, &sent, 0, flags);
        if (result == CURLE_AGAIN) {
            return false;
        }
        if (result != CURLE_OK) {
            markError(encodeError(result, curl_easy_strerror(result)));
            return true;
        }

        bool becameWritable = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!outbound_.has_value()) {
                return true;
            }
            outbound_->offset += sent;
            if (outbound_->offset >= outbound_->payload.size()) {
                outbound_.reset();
                becameWritable = true;
            }
        }
        if (becameWritable) {
            handlePressure(emit(NativeHttpWebSocketEventKind::Writable, "", {}, 0, "", true, ""));
        }
        return true;
#else
        return false;
#endif
    }

    bool receiveOne() {
#if LIBCURL_VERSION_NUM >= 0x075600
        std::vector<uint8_t> buffer(4096);
        size_t received = 0;
        const curl_ws_frame* meta = nullptr;
        const CURLcode result = curl_ws_recv(handle_, buffer.data(), buffer.size(), &received, &meta);
        if (result == CURLE_AGAIN) {
            return false;
        }
        if (result == CURLE_GOT_NOTHING) {
            markClosed(1006, "", false);
            requestStop();
            return true;
        }
        if (result != CURLE_OK) {
            markError(encodeError(result, curl_easy_strerror(result)));
            return true;
        }
        if (meta == nullptr) {
            return true;
        }

        buffer.resize(received);
        if ((meta->flags & CURLWS_CLOSE) != 0) {
            int32_t code = 1000;
            std::string reason;
            if (buffer.size() >= 2) {
                code = (static_cast<int32_t>(buffer[0]) << 8) | static_cast<int32_t>(buffer[1]);
                reason = std::string(buffer.begin() + 2, buffer.end());
            }
            markClosed(code, reason, true);
            requestStop();
            return true;
        }
        if ((meta->flags & CURLWS_PING) != 0 || (meta->flags & CURLWS_PONG) != 0) {
            return true;
        }

        appendInbound(meta->flags, std::move(buffer));
        if (meta->bytesleft == 0 && (meta->flags & CURLWS_CONT) == 0) {
            emitInbound(meta->flags);
        }
        return true;
#else
        return false;
#endif
    }

#if LIBCURL_VERSION_NUM >= 0x075600
    unsigned int flagsFor(OutboundKind kind) const {
        switch (kind) {
            case OutboundKind::Text: return CURLWS_TEXT;
            case OutboundKind::Binary: return CURLWS_BINARY;
            case OutboundKind::Ping: return CURLWS_PING;
            case OutboundKind::Close: return CURLWS_CLOSE;
        }
        return CURLWS_BINARY;
    }
#endif

    void appendInbound(int flags, std::vector<uint8_t> chunk) {
        std::lock_guard<std::mutex> lock(mutex_);
        if ((flags & CURLWS_TEXT) != 0) {
            inboundText_ = true;
        } else if ((flags & CURLWS_BINARY) != 0) {
            inboundText_ = false;
        }
        inboundBuffer_.insert(inboundBuffer_.end(), chunk.begin(), chunk.end());
    }

    void emitInbound(int flags) {
        std::vector<uint8_t> bytes;
        bool asText = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            bytes.swap(inboundBuffer_);
            asText = inboundText_ || (flags & CURLWS_TEXT) != 0;
        }
        int32_t pressure = 0;
        if (asText) {
            pressure = emit(NativeHttpWebSocketEventKind::Text, std::string(bytes.begin(), bytes.end()), {}, 0, "", true, "");
        } else {
            pressure = emit(
                NativeHttpWebSocketEventKind::Binary,
                "",
                std::make_shared<std::vector<uint8_t>>(std::move(bytes)),
                0,
                "",
                true,
                ""
            );
        }
        handlePressure(pressure);
    }

    void handlePressure(int32_t pressure) {
        if (pressure == 1) {
            std::lock_guard<std::mutex> lock(mutex_);
            inboundPaused_ = true;
        } else if (pressure >= 2) {
            markError("backpressure|0|websocket event channel is full or closed");
        }
    }

    void markError(const std::string& error) {
        setState(NativeHttpWebSocketState::Error);
        emit(NativeHttpWebSocketEventKind::Error, "", {}, 0, "", false, error);
        closeEventChannel();
        requestStop();
    }

    void markClosed(int32_t code, const std::string& reason, bool wasClean) {
        setState(NativeHttpWebSocketState::Closed);
        emit(NativeHttpWebSocketEventKind::Close, "", {}, code, reason, wasClean, "");
        closeEventChannel();
    }

    void closeEventChannel() {
        std::shared_ptr<doof_event::NativeChannel> eventChannel;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            eventChannel = eventChannel_;
        }
        if (eventChannel) {
            eventChannel->tryClose();
        }
    }

    void setState(NativeHttpWebSocketState state) {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = state;
    }

    int32_t emit(
        NativeHttpWebSocketEventKind kind,
        std::string text,
        std::shared_ptr<std::vector<uint8_t>> bytes,
        int32_t code,
        std::string reason,
        bool wasClean,
        std::string error
    ) {
        auto connection = publicConnection();
        if (!connection) {
            return 0;
        }

        NativeHttpWebSocketConnection::PublicEvent publicEvent = std::make_shared<std_::http::websocket::WebSocketOpen>(connection);
        bool keyed = false;
        switch (kind) {
            case NativeHttpWebSocketEventKind::Text:
                publicEvent = std::make_shared<std_::http::websocket::WebSocketText>(connection, std::move(text));
                break;
            case NativeHttpWebSocketEventKind::Binary:
                publicEvent = std::make_shared<std_::http::websocket::WebSocketBinary>(
                    connection,
                    bytes ? std::move(bytes) : std::make_shared<std::vector<uint8_t>>()
                );
                break;
            case NativeHttpWebSocketEventKind::Writable:
                resumeCommandChannel();
                keyed = true;
                publicEvent = std::make_shared<std_::http::websocket::WebSocketWritable>(connection);
                break;
            case NativeHttpWebSocketEventKind::Close:
                publicEvent = std::make_shared<std_::http::websocket::WebSocketClose>(
                    connection,
                    code,
                    std::move(reason),
                    wasClean
                );
                break;
            case NativeHttpWebSocketEventKind::Error:
                publicEvent = std::make_shared<std_::http::websocket::WebSocketError>(connection, parseHttpError(error));
                break;
            case NativeHttpWebSocketEventKind::Open:
                break;
        }

        const int32_t pressure = emitPublicEvent(std::move(publicEvent), keyed);
        if (kind == NativeHttpWebSocketEventKind::Close || kind == NativeHttpWebSocketEventKind::Error) {
            closePublicChannels();
        }
        return pressure;
    }

    int32_t emitPublicEvent(NativeHttpWebSocketConnection::PublicEvent event, bool keyed) {
        std::shared_ptr<doof_event::NativeChannel> eventChannel;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            eventChannel = eventChannel_;
        }
        if (!eventChannel) {
            return 0;
        }
        return eventChannel->trySendMessage(std::move(event), keyed, "websocket:writable");
    }

    void closePublicChannels() {
        std::shared_ptr<doof_event::NativeChannel> eventChannel;
        std::shared_ptr<doof_event::NativeChannel> commandChannel;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            eventChannel = eventChannel_;
            commandChannel = commandChannel_;
            connection_.reset();
        }
        if (commandChannel) {
            commandChannel->tryClose();
        }
        if (eventChannel) {
            eventChannel->tryClose();
        }
    }

    void cleanupHandle() {
        CURL* handle = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            handle = handle_;
            handle_ = nullptr;
        }
        if (handle != nullptr) {
            curl_easy_cleanup(handle);
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    CURL* handle_ = nullptr;
    NativeHttpWebSocketState state_ = NativeHttpWebSocketState::Connecting;
    std::shared_ptr<doof_event::NativeChannel> eventChannel_;
    std::shared_ptr<doof_event::NativeChannel> commandChannel_;
    std::shared_ptr<std_::http::websocket::WebSocketConnection> connection_;
    std::optional<OutboundMessage> outbound_;
    std::vector<uint8_t> inboundBuffer_;
    std::thread worker_;
    bool inboundText_ = true;
    bool inboundPaused_ = false;
    bool stopRequested_ = false;
};

doof::Result<std::shared_ptr<NativeHttpWebSocketConnection>, std::string> NativeHttpWebSocketConnection::connect(
    const std::string& url,
    const std::string& requestHeaders,
    int32_t timeoutMs,
    int32_t outboundCapacity,
    int32_t eventCapacity
) {
    auto result = NativeHttpWebSocketConnectionImpl::connect(url, requestHeaders, timeoutMs, outboundCapacity, eventCapacity);
    if (result.isFailure()) {
        return doof::Result<std::shared_ptr<NativeHttpWebSocketConnection>, std::string>::failure(result.error());
    }
    return doof::Result<std::shared_ptr<NativeHttpWebSocketConnection>, std::string>::success(
        std::make_shared<NativeHttpWebSocketConnection>(std::move(result.value()))
    );
}

NativeHttpWebSocketConnection::NativeHttpWebSocketConnection(std::shared_ptr<NativeHttpWebSocketConnectionImpl> impl)
    : impl_(std::move(impl)) {}

NativeHttpWebSocketConnection::~NativeHttpWebSocketConnection() = default;

void NativeHttpWebSocketConnection::start() { impl_->start(); }
doof::Result<void, std::string> NativeHttpWebSocketConnection::sendText(const std::string& text) { return impl_->sendText(text); }
doof::Result<void, std::string> NativeHttpWebSocketConnection::sendBinary(std::shared_ptr<std::vector<uint8_t>> bytes) { return impl_->sendBinary(std::move(bytes)); }
doof::Result<void, std::string> NativeHttpWebSocketConnection::ping() { return impl_->ping(); }
doof::Result<void, std::string> NativeHttpWebSocketConnection::close(int32_t code, const std::string& reason) { return impl_->close(code, reason); }
void NativeHttpWebSocketConnection::attachChannels(
    std::shared_ptr<std_::http::websocket::WebSocketConnection> connection,
    std::shared_ptr<EventSender> eventSender,
    std::shared_ptr<CommandReceiver> commandReceiver
) {
    impl_->attachChannels(std::move(connection), std::move(eventSender), std::move(commandReceiver));
}
void NativeHttpWebSocketConnection::resumeInboundReads() { impl_->resumeInboundReads(); }
int32_t NativeHttpWebSocketConnection::state() const { return impl_->state(); }
