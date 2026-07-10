#include "native_http_client.hpp"
#include "native_event.hpp"
#include "websocket.hpp"

#import <Foundation/Foundation.h>

#include <algorithm>
#include <mutex>
#include <utility>

namespace {

NSString* nsString(const std::string& value) {
    return [NSString stringWithUTF8String:value.c_str()];
}

std::string cppString(NSString* value) {
    if (value == nil) {
        return "";
    }
    return std::string([value UTF8String]);
}

std::shared_ptr<std::vector<uint8_t>> vectorFromData(NSData* data) {
    auto bytes = std::make_shared<std::vector<uint8_t>>();
    if (data == nil || data.length == 0) {
        return bytes;
    }
    const auto* raw = static_cast<const uint8_t*>(data.bytes);
    bytes->assign(raw, raw + data.length);
    return bytes;
}

NSData* dataFromVector(const std::vector<uint8_t>& bytes) {
    if (bytes.empty()) {
        return [NSData data];
    }
    return [NSData dataWithBytes:bytes.data() length:bytes.size()];
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

std::string classifyError(NSError* error) {
    if (error == nil) {
        return "transport";
    }
    if (![error.domain isEqualToString:NSURLErrorDomain]) {
        return "transport";
    }

    switch (error.code) {
        case NSURLErrorBadURL:
        case NSURLErrorUnsupportedURL:
            return "invalid-url";
        case NSURLErrorTimedOut:
            return "timeout";
        case NSURLErrorCannotFindHost:
        case NSURLErrorDNSLookupFailed:
            return "dns";
        case NSURLErrorCannotConnectToHost:
            return "connect";
        case NSURLErrorSecureConnectionFailed:
        case NSURLErrorServerCertificateHasBadDate:
        case NSURLErrorServerCertificateUntrusted:
        case NSURLErrorServerCertificateHasUnknownRoot:
        case NSURLErrorServerCertificateNotYetValid:
        case NSURLErrorClientCertificateRejected:
        case NSURLErrorClientCertificateRequired:
            return "tls";
        default:
            return "transport";
    }
}

std::string encodeError(NSError* error, const std::string& fallback) {
    if (error == nil) {
        return "transport|0|" + fallback;
    }
    std::string message = cppString(error.localizedDescription);
    if (message.empty()) {
        message = fallback;
    }
    return classifyError(error) + "|" + std::to_string(static_cast<long long>(error.code)) + "|" + message;
}

std::string encodeError(const std::string& kind, const std::string& code, const std::string& message) {
    return kind + "|" + code + "|" + message;
}

void applyHeaders(NSMutableURLRequest* request, const std::string& requestHeaders) {
    std::string current;
    current.reserve(requestHeaders.size());
    auto flush = [&]() {
        if (current.empty()) {
            return;
        }
        const size_t separator = current.find(':');
        if (separator == std::string::npos || separator == 0) {
            current.clear();
            return;
        }
        std::string name = current.substr(0, separator);
        std::string value = current.substr(separator + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
            value.erase(value.begin());
        }
        [request setValue:nsString(value) forHTTPHeaderField:nsString(name)];
        current.clear();
    };

    for (char ch : requestHeaders) {
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            flush();
        } else {
            current.push_back(ch);
        }
    }
    flush();
}

std::string renderHeaders(NSHTTPURLResponse* response) {
    if (response == nil) {
        return "";
    }
    std::string text;
    NSDictionary* headers = response.allHeaderFields;
    for (id key in headers) {
        id value = [headers objectForKey:key];
        if ([key respondsToSelector:@selector(description)] && [value respondsToSelector:@selector(description)]) {
            text += cppString([key description]);
            text += ": ";
            text += cppString([value description]);
            text += "\r\n";
        }
    }
    return text;
}

long normalizedTimeoutSeconds(int32_t timeoutMs) {
    return timeoutMs > 0 ? std::max<long>(1, timeoutMs / 1000) : 30;
}

NSURLSessionWebSocketCloseCode closeCodeFromInt(int32_t code) {
    switch (code) {
        case 1000: return NSURLSessionWebSocketCloseCodeNormalClosure;
        case 1001: return NSURLSessionWebSocketCloseCodeGoingAway;
        case 1002: return NSURLSessionWebSocketCloseCodeProtocolError;
        case 1003: return NSURLSessionWebSocketCloseCodeUnsupportedData;
        case 1007: return NSURLSessionWebSocketCloseCodeInvalidFramePayloadData;
        case 1008: return NSURLSessionWebSocketCloseCodePolicyViolation;
        case 1009: return NSURLSessionWebSocketCloseCodeMessageTooBig;
        case 1011: return NSURLSessionWebSocketCloseCodeInternalServerError;
        default: return NSURLSessionWebSocketCloseCodeNormalClosure;
    }
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

@interface DoofHttpRedirectDelegate : NSObject <NSURLSessionTaskDelegate>
- (instancetype)initWithFollowRedirects:(BOOL)followRedirects;
@end

@implementation DoofHttpRedirectDelegate {
    BOOL _followRedirects;
}

- (instancetype)initWithFollowRedirects:(BOOL)followRedirects {
    self = [super init];
    if (self != nil) {
        _followRedirects = followRedirects;
    }
    return self;
}

- (void)URLSession:(NSURLSession*)session
              task:(NSURLSessionTask*)task
willPerformHTTPRedirection:(NSHTTPURLResponse*)response
        newRequest:(NSURLRequest*)request
 completionHandler:(void (^)(NSURLRequest* _Nullable))completionHandler {
    completionHandler(_followRedirects ? request : nil);
}

@end

class NativeHttpClient::Impl {
public:
    Impl() : responseBody_(std::make_shared<std::vector<uint8_t>>()) {}

    doof::Result<int32_t, std::string> perform(
        const std::string& method,
        const std::string& url,
        const std::string& requestHeaders,
        std::shared_ptr<std::vector<uint8_t>> body,
        int32_t timeoutMs,
        bool followRedirects
    ) {
        responseStatusText_.clear();
        responseHeadersText_.clear();
        responseBody_ = std::make_shared<std::vector<uint8_t>>();

        @autoreleasepool {
            NSURL* nsUrl = [NSURL URLWithString:nsString(url)];
            if (nsUrl == nil || nsUrl.scheme == nil || nsUrl.host == nil) {
                return doof::Failure<std::string>{"invalid-url|0|invalid URL"};
            }

            NSMutableURLRequest* request = [NSMutableURLRequest requestWithURL:nsUrl];
            request.HTTPMethod = nsString(method);
            request.timeoutInterval = normalizedTimeoutSeconds(timeoutMs);
            [request setValue:@"doof-http-client/0.1" forHTTPHeaderField:@"User-Agent"];
            applyHeaders(request, requestHeaders);
            if (body != nullptr) {
                request.HTTPBody = dataFromVector(*body);
            }

            NSURLSessionConfiguration* config = [NSURLSessionConfiguration ephemeralSessionConfiguration];
            config.timeoutIntervalForRequest = request.timeoutInterval;
            config.timeoutIntervalForResource = request.timeoutInterval;
            DoofHttpRedirectDelegate* delegate = [[DoofHttpRedirectDelegate alloc] initWithFollowRedirects:followRedirects ? YES : NO];
            NSURLSession* session = [NSURLSession sessionWithConfiguration:config delegate:delegate delegateQueue:nil];

            dispatch_semaphore_t done = dispatch_semaphore_create(0);
            __block NSData* resultData = nil;
            __block NSURLResponse* resultResponse = nil;
            __block NSError* resultError = nil;

            NSURLSessionDataTask* task = [session dataTaskWithRequest:request completionHandler:^(NSData* data, NSURLResponse* response, NSError* error) {
                resultData = [data retain];
                resultResponse = [response retain];
                resultError = [error retain];
                dispatch_semaphore_signal(done);
            }];
            [task resume];
            dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
            [session finishTasksAndInvalidate];

            if (resultError != nil) {
                std::string encoded = encodeError(resultError, "HTTP request failed");
                [resultData release];
                [resultResponse release];
                [resultError release];
                [delegate release];
                return doof::Failure<std::string>{encoded};
            }

            NSHTTPURLResponse* httpResponse = [resultResponse isKindOfClass:[NSHTTPURLResponse class]]
                ? (NSHTTPURLResponse*)resultResponse
                : nil;
            if (httpResponse == nil) {
                [resultData release];
                [resultResponse release];
                [resultError release];
                [delegate release];
                return doof::Failure<std::string>{"transport|0|response was not HTTP"};
            }

            responseBody_ = vectorFromData(resultData);
            responseHeadersText_ = renderHeaders(httpResponse);
            responseStatusText_ = statusText(httpResponse.statusCode);
            [resultData release];
            [resultResponse release];
            [resultError release];
            [delegate release];
            return doof::Success<int32_t>{static_cast<int32_t>(httpResponse.statusCode)};
        }
    }

    std::string responseStatusText() const { return responseStatusText_; }
    std::string responseHeadersText() const { return responseHeadersText_; }
    std::shared_ptr<std::vector<uint8_t>> responseBody() const { return responseBody_; }

private:
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

class NativeHttpWebSocketConnectionImpl;

@interface DoofHttpWebSocketDelegate : NSObject <NSURLSessionWebSocketDelegate>
- (instancetype)initWithImpl:(std::weak_ptr<NativeHttpWebSocketConnectionImpl>)impl;
@end

class NativeHttpWebSocketConnectionImpl : public std::enable_shared_from_this<NativeHttpWebSocketConnectionImpl> {
public:
    static doof::Result<std::shared_ptr<NativeHttpWebSocketConnectionImpl>, std::string> connect(
        const std::string& url,
        const std::string& requestHeaders,
        int32_t timeoutMs,
        int32_t outboundCapacity,
        int32_t eventCapacity
    ) {
        (void)outboundCapacity;
        (void)eventCapacity;
        @autoreleasepool {
            NSURL* nsUrl = [NSURL URLWithString:nsString(url)];
            if (nsUrl == nil || nsUrl.scheme == nil || nsUrl.host == nil) {
                return doof::Failure<std::string>{"invalid-url|0|invalid websocket URL"};
            }
            NSString* scheme = nsUrl.scheme.lowercaseString;
            if (![scheme isEqualToString:@"ws"] && ![scheme isEqualToString:@"wss"]) {
                return doof::Failure<std::string>{"invalid-url|0|websocket URL must use ws or wss"};
            }

            auto impl = std::shared_ptr<NativeHttpWebSocketConnectionImpl>(
                new NativeHttpWebSocketConnectionImpl()
            );
            impl->initialize(nsUrl, requestHeaders, timeoutMs);
            return doof::Success<std::shared_ptr<NativeHttpWebSocketConnectionImpl>>{impl};
        }
    }

    ~NativeHttpWebSocketConnectionImpl() {
        shutdown();
    }

    void initialize(NSURL* url, const std::string& requestHeaders, int32_t timeoutMs) {
        NSMutableURLRequest* request = [NSMutableURLRequest requestWithURL:url];
        request.timeoutInterval = normalizedTimeoutSeconds(timeoutMs);
        [request setValue:@"doof-http-client/0.1" forHTTPHeaderField:@"User-Agent"];
        applyHeaders(request, requestHeaders);

        NSURLSessionConfiguration* config = [NSURLSessionConfiguration ephemeralSessionConfiguration];
        config.timeoutIntervalForRequest = request.timeoutInterval;
        config.timeoutIntervalForResource = request.timeoutInterval;
        delegate_ = [[DoofHttpWebSocketDelegate alloc] initWithImpl:weak_from_this()];
        session_ = [[NSURLSession sessionWithConfiguration:config delegate:delegate_ delegateQueue:nil] retain];
        task_ = [[session_ webSocketTaskWithRequest:request] retain];
        setState(NativeHttpWebSocketState::Open);
    }

    void start() {
        [task_ resume];
        receiveNext();
    }

    doof::Result<void, std::string> sendText(const std::string& text) {
        return enqueue(NativeHttpWebSocketEventKind::Text, text, {});
    }

    doof::Result<void, std::string> sendBinary(std::shared_ptr<std::vector<uint8_t>> bytes) {
        return enqueue(NativeHttpWebSocketEventKind::Binary, "", bytes ? *bytes : std::vector<uint8_t>());
    }

    doof::Result<void, std::string> ping() {
        if (isClosedOrClosing()) {
            return doof::Failure<std::string>{"closed|0|websocket is closed"};
        }
        auto self = shared_from_this();
        [task_ sendPingWithPongReceiveHandler:^(NSError* error) {
            if (error != nil) {
                self->markError(encodeError(error, "websocket ping failed"));
            }
        }];
        return doof::Success<void>{};
    }

    doof::Result<void, std::string> close(int32_t code, const std::string& reason) {
        if (reason.size() > 123) {
            return doof::Failure<std::string>{"invalid-close|0|websocket close reason exceeds 123 bytes"};
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_ == NativeHttpWebSocketState::Closed || state_ == NativeHttpWebSocketState::Error) {
                return doof::Failure<std::string>{"closed|0|websocket is closed"};
            }
            state_ = NativeHttpWebSocketState::Closing;
        }
        NSData* reasonData = reason.empty() ? nil : dataFromVector(std::vector<uint8_t>(reason.begin(), reason.end()));
        [task_ cancelWithCloseCode:closeCodeFromInt(code) reason:reasonData];
        return doof::Success<void>{};
    }

    void resumeInboundReads() {
        bool shouldReceive = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            inboundPaused_ = false;
            shouldReceive = started_ && !receivePending_ && state_ == NativeHttpWebSocketState::Open;
        }
        if (shouldReceive) {
            receiveNext();
        }
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

    void didClose(int32_t code, const std::string& reason) {
        markClosed(code == 0 ? 1000 : code, reason, true);
    }

private:
    NativeHttpWebSocketConnectionImpl() = default;

    void handleCommand(NativeHttpWebSocketConnection::PublicCommand command) {
        pauseCommandChannel();
        doof::Result<void, std::string> result = doof::Success<void>{};
        bool waitsForWritable = false;

        if (auto* text = std::get_if<std::shared_ptr<std_::http::websocket::WebSocketSendText>>(&command)) {
            result = sendText((*text)->text);
            waitsForWritable = doof::is_success(result);
        } else if (auto* binary = std::get_if<std::shared_ptr<std_::http::websocket::WebSocketSendBinary>>(&command)) {
            result = sendBinary((*binary)->bytes);
            waitsForWritable = doof::is_success(result);
        } else if (std::holds_alternative<std::shared_ptr<std_::http::websocket::WebSocketPing>>(command)) {
            result = ping();
        } else if (auto* closeCommand = std::get_if<std::shared_ptr<std_::http::websocket::WebSocketCloseCommand>>(&command)) {
            result = close((*closeCommand)->code, (*closeCommand)->reason);
            waitsForWritable = doof::is_success(result);
        }

        if (doof::is_failure(result)) {
            resumeCommandChannel();
            emitErrorToPublicChannel(doof::failure_error(result));
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

    doof::Result<void, std::string> enqueue(
        NativeHttpWebSocketEventKind kind,
        std::string text,
        std::vector<uint8_t> bytes
    ) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_ == NativeHttpWebSocketState::Closed || state_ == NativeHttpWebSocketState::Error) {
                return doof::Failure<std::string>{"closed|0|websocket is closed"};
            }
            if (state_ == NativeHttpWebSocketState::Closing) {
                return doof::Failure<std::string>{"closing|0|websocket is closing"};
            }
        }

        NSURLSessionWebSocketMessage* message = nil;
        if (kind == NativeHttpWebSocketEventKind::Text) {
            message = [[NSURLSessionWebSocketMessage alloc] initWithString:nsString(text)];
        } else {
            message = [[NSURLSessionWebSocketMessage alloc] initWithData:dataFromVector(bytes)];
        }

        auto self = shared_from_this();
        [task_ sendMessage:message completionHandler:^(NSError* error) {
            bool writable = false;
            {
                std::lock_guard<std::mutex> lock(self->mutex_);
                writable = self->state_ == NativeHttpWebSocketState::Open;
            }
            if (error != nil) {
                self->markError(encodeError(error, "websocket send failed"));
                return;
            }
            if (writable) {
                self->handlePressure(self->emit(NativeHttpWebSocketEventKind::Writable, "", {}, 0, "", true, ""));
            }
        }];
        [message release];
        return doof::Success<void>{};
    }

    void receiveNext() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (inboundPaused_ || receivePending_ || state_ != NativeHttpWebSocketState::Open) {
                return;
            }
            started_ = true;
            receivePending_ = true;
        }

        auto self = shared_from_this();
        [task_ receiveMessageWithCompletionHandler:^(NSURLSessionWebSocketMessage* message, NSError* error) {
            {
                std::lock_guard<std::mutex> lock(self->mutex_);
                self->receivePending_ = false;
            }
            if (error != nil) {
                self->markError(encodeError(error, "websocket receive failed"));
                return;
            }
            if (message == nil) {
                self->markClosed(1006, "", false);
                return;
            }

            int32_t pressure = 0;
            if (message.type == NSURLSessionWebSocketMessageTypeString) {
                pressure = self->emit(NativeHttpWebSocketEventKind::Text, cppString(message.string), {}, 0, "", true, "");
            } else {
                pressure = self->emit(NativeHttpWebSocketEventKind::Binary, "", vectorFromData(message.data), 0, "", true, "");
            }
            self->handlePressure(pressure);
            self->receiveNext();
        }];
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
        shutdown();
    }

    void markClosed(int32_t code, const std::string& reason, bool wasClean) {
        setState(NativeHttpWebSocketState::Closed);
        emit(NativeHttpWebSocketEventKind::Close, "", {}, code, reason, wasClean, "");
        closeEventChannel();
        shutdown();
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

    void shutdown() {
        NSURLSessionWebSocketTask* task = nil;
        NSURLSession* session = nil;
        DoofHttpWebSocketDelegate* delegate = nil;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            task = task_;
            session = session_;
            delegate = delegate_;
            task_ = nil;
            session_ = nil;
            delegate_ = nil;
        }
        if (task != nil) {
            [task cancelWithCloseCode:NSURLSessionWebSocketCloseCodeNormalClosure reason:nil];
            [task release];
        }
        if (session != nil) {
            [session invalidateAndCancel];
            [session release];
        }
        if (delegate != nil) {
            [delegate release];
        }
    }

    bool isClosedOrClosing() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return state_ == NativeHttpWebSocketState::Closed
            || state_ == NativeHttpWebSocketState::Closing
            || state_ == NativeHttpWebSocketState::Error;
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

    mutable std::mutex mutex_;
    NSURLSession* session_ = nil;
    NSURLSessionWebSocketTask* task_ = nil;
    DoofHttpWebSocketDelegate* delegate_ = nil;
    NativeHttpWebSocketState state_ = NativeHttpWebSocketState::Connecting;
    std::shared_ptr<doof_event::NativeChannel> eventChannel_;
    std::shared_ptr<doof_event::NativeChannel> commandChannel_;
    std::shared_ptr<std_::http::websocket::WebSocketConnection> connection_;
    bool inboundPaused_ = false;
    bool receivePending_ = false;
    bool started_ = false;
};

@implementation DoofHttpWebSocketDelegate {
    std::weak_ptr<NativeHttpWebSocketConnectionImpl> _impl;
}

- (instancetype)initWithImpl:(std::weak_ptr<NativeHttpWebSocketConnectionImpl>)impl {
    self = [super init];
    if (self != nil) {
        _impl = std::move(impl);
    }
    return self;
}

- (void)URLSession:(NSURLSession*)session
          webSocketTask:(NSURLSessionWebSocketTask*)webSocketTask
 didCloseWithCloseCode:(NSURLSessionWebSocketCloseCode)closeCode
              reason:(NSData*)reason {
    auto impl = _impl.lock();
    if (!impl) {
        return;
    }
    std::string reasonText;
    if (reason != nil && reason.length > 0) {
        NSString* text = [[NSString alloc] initWithData:reason encoding:NSUTF8StringEncoding];
        reasonText = cppString(text);
        [text release];
    }
    impl->didClose(static_cast<int32_t>(closeCode), reasonText);
}

@end

doof::Result<std::shared_ptr<NativeHttpWebSocketConnection>, std::string> NativeHttpWebSocketConnection::connect(
    const std::string& url,
    const std::string& requestHeaders,
    int32_t timeoutMs,
    int32_t outboundCapacity,
    int32_t eventCapacity
) {
    auto result = NativeHttpWebSocketConnectionImpl::connect(url, requestHeaders, timeoutMs, outboundCapacity, eventCapacity);
    if (doof::is_failure(result)) {
        return doof::Failure<std::string>{doof::failure_error(result)};
    }
    return doof::Success<std::shared_ptr<NativeHttpWebSocketConnection>>{std::make_shared<NativeHttpWebSocketConnection>(std::move(doof::success_value(result)))};
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
