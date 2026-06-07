#pragma once

#include <algorithm>
#include <cstdint>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <sys/select.h>
#include <curl/curl.h>
#include "doof_runtime.hpp"

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

} // namespace

class NativeHttpClient {
public:
    NativeHttpClient()
        : responseBody_(std::make_shared<std::vector<uint8_t>>()) {
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
        curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, &NativeHttpClient::writeBodyCallback);
        curl_easy_setopt(handle, CURLOPT_WRITEDATA, this);
        curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, &NativeHttpClient::writeHeaderCallback);
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

    std::string responseStatusText() const {
        return responseStatusText_;
    }

    std::string responseHeadersText() const {
        return responseHeadersText_;
    }

    std::shared_ptr<std::vector<uint8_t>> responseBody() const {
        return responseBody_;
    }

private:
    static size_t writeBodyCallback(char* data, size_t size, size_t count, void* userData) {
        const std::size_t bytes = size * count;
        auto* self = static_cast<NativeHttpClient*>(userData);
        self->responseBody_->insert(self->responseBody_->end(), data, data + bytes);
        return bytes;
    }

    static size_t writeHeaderCallback(char* data, size_t size, size_t count, void* userData) {
        const std::size_t bytes = size * count;
        auto* self = static_cast<NativeHttpClient*>(userData);
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
    ) : kind_(kind),
        text_(std::move(text)),
        bytes_(bytes ? std::move(bytes) : std::make_shared<std::vector<uint8_t>>()),
        code_(code),
        reason_(std::move(reason)),
        wasClean_(wasClean),
        error_(std::move(error)) {}

    int32_t kind() const { return static_cast<int32_t>(kind_); }
    std::string text() const { return text_; }
    std::shared_ptr<std::vector<uint8_t>> bytes() const { return bytes_; }
    int32_t code() const { return code_; }
    std::string reason() const { return reason_; }
    bool wasClean() const { return wasClean_; }
    std::string error() const { return error_; }

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
    using EventCallback = doof::callback<int32_t(std::shared_ptr<NativeHttpWebSocketEvent>)>;

    static doof::Result<std::shared_ptr<NativeHttpWebSocketConnection>, std::string> connect(
        const std::string& url,
        const std::string& requestHeaders,
        int32_t timeoutMs,
        int32_t outboundCapacity,
        EventCallback callback
    ) {
#if LIBCURL_VERSION_NUM >= 0x075600
        ensureCurlGlobalInit();
        CURL* handle = curl_easy_init();
        if (handle == nullptr) {
            return doof::Result<std::shared_ptr<NativeHttpWebSocketConnection>, std::string>::failure(
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
            return doof::Result<std::shared_ptr<NativeHttpWebSocketConnection>, std::string>::failure(encoded);
        }

        if (headerList != nullptr) {
            curl_slist_free_all(headerList);
        }

        auto connection = std::shared_ptr<NativeHttpWebSocketConnection>(
            new NativeHttpWebSocketConnection(handle, std::max<int32_t>(1, outboundCapacity), std::move(callback))
        );
        return doof::Result<std::shared_ptr<NativeHttpWebSocketConnection>, std::string>::success(connection);
#else
        (void)url;
        (void)requestHeaders;
        (void)timeoutMs;
        (void)outboundCapacity;
        (void)callback;
        return doof::Result<std::shared_ptr<NativeHttpWebSocketConnection>, std::string>::failure(
            "unsupported|0|libcurl websocket support requires libcurl 7.86.0 or newer"
        );
#endif
    }

    ~NativeHttpWebSocketConnection() {
        requestStop();
        if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) {
            worker_.join();
        }
    }

    void start() {
        startWorker();
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

    NativeHttpWebSocketConnection(
        CURL* handle,
        int32_t outboundCapacity,
        EventCallback callback
    ) : handle_(handle),
        outboundCapacity_(outboundCapacity),
        outboundLowWater_(std::max<int32_t>(0, outboundCapacity / 2)),
        callback_(std::move(callback)) {
        setState(NativeHttpWebSocketState::Open);
    }

    void startWorker() {
        auto self = shared_from_this();
        worker_ = std::thread([self] {
            self->workerLoop();
        });
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
            if (outbound_.size() >= static_cast<size_t>(outboundCapacity_)) {
                return doof::Result<void, std::string>::failure("backpressure|0|websocket outbound queue is full");
            }
            if (kind == OutboundKind::Close) {
                state_ = NativeHttpWebSocketState::Closing;
            }
            outbound_.push_back(OutboundMessage {
                kind,
                std::move(payload),
                closeCode,
                std::move(closeReason),
                0,
            });
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
        return !outbound_.empty();
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
            if (!outbound_.empty()) {
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
            if (outbound_.empty()) {
                return false;
            }
            current = outbound_.front();
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
            if (outbound_.empty()) {
                return true;
            }
            outbound_.front().offset += sent;
            if (outbound_.front().offset >= outbound_.front().payload.size()) {
                outbound_.pop_front();
                becameWritable = outbound_.size() <= static_cast<size_t>(outboundLowWater_);
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
            pressure = emit(
                NativeHttpWebSocketEventKind::Text,
                std::string(bytes.begin(), bytes.end()),
                {},
                0,
                "",
                true,
                ""
            );
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
        requestStop();
    }

    void markClosed(int32_t code, const std::string& reason, bool wasClean) {
        setState(NativeHttpWebSocketState::Closed);
        emit(NativeHttpWebSocketEventKind::Close, "", {}, code, reason, wasClean, "");
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
        EventCallback callback;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            callback = callback_;
        }
        if (!callback) {
            return 0;
        }
        return doof::detail::call_callback_unchecked(callback, std::make_shared<NativeHttpWebSocketEvent>(
            kind,
            std::move(text),
            std::move(bytes),
            code,
            std::move(reason),
            wasClean,
            std::move(error)
        ));
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
    int32_t outboundCapacity_ = 1;
    int32_t outboundLowWater_ = 0;
    NativeHttpWebSocketState state_ = NativeHttpWebSocketState::Connecting;
    EventCallback callback_;
    std::deque<OutboundMessage> outbound_;
    std::vector<uint8_t> inboundBuffer_;
    std::thread worker_;
    bool inboundText_ = true;
    bool inboundPaused_ = false;
    bool stopRequested_ = false;
};
