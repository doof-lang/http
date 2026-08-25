#include "native_http_client.hpp"

#include <cstdint>
#include <string>
#include <utility>

#if defined(__EMSCRIPTEN__)
extern "C" {
__attribute__((import_module("doof_http"), import_name("perform_request")))
int32_t doof_http_perform_request(
    const char* method,
    const char* url,
    const char* request_headers,
    const uint8_t* body,
    int32_t body_size,
    int32_t has_body,
    int32_t timeout_ms,
    int32_t follow_redirects
);

__attribute__((import_module("doof_http"), import_name("response_status")))
int32_t doof_http_response_status(int32_t response_id);

__attribute__((import_module("doof_http"), import_name("response_size")))
int32_t doof_http_response_size(int32_t response_id, int32_t field);

__attribute__((import_module("doof_http"), import_name("response_copy")))
int32_t doof_http_response_copy(
    int32_t response_id,
    int32_t field,
    uint8_t* destination,
    int32_t capacity
);

__attribute__((import_module("doof_http"), import_name("release_response")))
void doof_http_release_response(int32_t response_id);
}
#endif

namespace {

enum class ResponseField : int32_t {
    StatusText = 0,
    Headers = 1,
    Body = 2,
    Error = 3,
};

class ResponseLease {
public:
    explicit ResponseLease(int32_t responseId) : responseId_(responseId) {}
    ~ResponseLease() {
#if defined(__EMSCRIPTEN__)
        if (responseId_ > 0) doof_http_release_response(responseId_);
#endif
    }

    ResponseLease(const ResponseLease&) = delete;
    ResponseLease& operator=(const ResponseLease&) = delete;

private:
    int32_t responseId_;
};

#if defined(__EMSCRIPTEN__)
bool readResponseBytes(
    int32_t responseId,
    ResponseField field,
    std::vector<uint8_t>& destination,
    std::string& bridgeError
) {
    const int32_t size = doof_http_response_size(responseId, static_cast<int32_t>(field));
    if (size < 0) {
        bridgeError = "transport|0|The WebAssembly HTTP bridge returned an invalid response field";
        return false;
    }

    destination.resize(static_cast<std::size_t>(size));
    if (size == 0) return true;

    const int32_t copied = doof_http_response_copy(
        responseId,
        static_cast<int32_t>(field),
        destination.data(),
        size
    );
    if (copied != size) {
        bridgeError = "transport|0|The WebAssembly HTTP bridge could not copy a response field";
        return false;
    }
    return true;
}

bool readResponseString(
    int32_t responseId,
    ResponseField field,
    std::string& destination,
    std::string& bridgeError
) {
    std::vector<uint8_t> bytes;
    if (!readResponseBytes(responseId, field, bytes, bridgeError)) return false;
    destination.assign(bytes.begin(), bytes.end());
    return true;
}
#endif

}  // namespace

class NativeHttpClient::Impl {
public:
    std::string statusText;
    std::string responseHeaders;
    std::shared_ptr<std::vector<uint8_t>> body = std::make_shared<std::vector<uint8_t>>();
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
#if defined(__EMSCRIPTEN__)
    impl_->statusText.clear();
    impl_->responseHeaders.clear();
    impl_->body = std::make_shared<std::vector<uint8_t>>();

    const uint8_t* bodyData = body && !body->empty() ? body->data() : nullptr;
    const int32_t bodySize = body ? static_cast<int32_t>(body->size()) : 0;
    const int32_t responseId = doof_http_perform_request(
        method.c_str(),
        url.c_str(),
        requestHeaders.c_str(),
        bodyData,
        bodySize,
        body ? 1 : 0,
        timeoutMs,
        followRedirects ? 1 : 0
    );
    if (responseId <= 0) {
        return doof::Failure<std::string>{
            "transport|0|The WebAssembly host did not return an HTTP response"};
    }
    ResponseLease response(responseId);

    std::string bridgeError;
    std::string requestError;
    if (!readResponseString(responseId, ResponseField::Error, requestError, bridgeError)) {
        return doof::Failure<std::string>{std::move(bridgeError)};
    }
    if (!requestError.empty()) {
        return doof::Failure<std::string>{std::move(requestError)};
    }

    const int32_t status = doof_http_response_status(responseId);
    if (status < 0) {
        return doof::Failure<std::string>{
            "transport|0|The WebAssembly HTTP bridge returned an invalid response"};
    }
    if (!readResponseString(
            responseId, ResponseField::StatusText, impl_->statusText, bridgeError) ||
        !readResponseString(
            responseId, ResponseField::Headers, impl_->responseHeaders, bridgeError) ||
        !readResponseBytes(responseId, ResponseField::Body, *impl_->body, bridgeError)) {
        return doof::Failure<std::string>{std::move(bridgeError)};
    }
    return doof::Success<int32_t>{status};
#else
    (void)method;
    (void)url;
    (void)requestHeaders;
    (void)body;
    (void)timeoutMs;
    (void)followRedirects;
    return doof::Failure<std::string>{
        "unsupported|0|The WebAssembly HTTP backend requires an Emscripten target"};
#endif
}

std::string NativeHttpClient::responseStatusText() const { return impl_->statusText; }
std::string NativeHttpClient::responseHeadersText() const { return impl_->responseHeaders; }
std::shared_ptr<std::vector<uint8_t>> NativeHttpClient::responseBody() const {
    return impl_->body;
}

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

class NativeHttpWebSocketConnectionImpl {};

doof::Result<std::shared_ptr<NativeHttpWebSocketConnection>, std::string>
NativeHttpWebSocketConnection::connect(
    const std::string&, const std::string&, int32_t, int32_t, int32_t
) {
    return doof::Failure<std::string>{
        "unsupported|0|WebSocket support is not yet available in WebAssembly"};
}

NativeHttpWebSocketConnection::NativeHttpWebSocketConnection(
    std::shared_ptr<NativeHttpWebSocketConnectionImpl> impl
) : impl_(std::move(impl)) {}
NativeHttpWebSocketConnection::~NativeHttpWebSocketConnection() = default;
void NativeHttpWebSocketConnection::start() {}
doof::Result<void, std::string> NativeHttpWebSocketConnection::sendText(const std::string&) {
    return doof::Failure<std::string>{"WebSocket support is not implemented in WebAssembly"};
}
doof::Result<void, std::string> NativeHttpWebSocketConnection::sendBinary(
    std::shared_ptr<std::vector<uint8_t>>
) {
    return doof::Failure<std::string>{"WebSocket support is not implemented in WebAssembly"};
}
doof::Result<void, std::string> NativeHttpWebSocketConnection::ping() {
    return doof::Failure<std::string>{"WebSocket support is not implemented in WebAssembly"};
}
doof::Result<void, std::string> NativeHttpWebSocketConnection::close(
    int32_t, const std::string&
) {
    return doof::Failure<std::string>{"WebSocket support is not implemented in WebAssembly"};
}
void NativeHttpWebSocketConnection::attachChannels(
    std::shared_ptr<std_::http::websocket::WebSocketConnection>,
    std::shared_ptr<EventSender>,
    std::shared_ptr<CommandReceiver>
) {}
void NativeHttpWebSocketConnection::resumeInboundReads() {}
int32_t NativeHttpWebSocketConnection::state() const {
    return static_cast<int32_t>(NativeHttpWebSocketState::Error);
}
