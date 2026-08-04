#include "native_http_client.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <utility>

#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

namespace {

std::string errorText(const char* kind, const char* action, DWORD error = ::GetLastError()) {
    return std::string(kind) + "|" + std::to_string(error) + "|" + action + " (Windows error " + std::to_string(error) + ")";
}

doof::Result<std::wstring, std::string> utf8ToWide(const std::string& value) {
    if (value.empty()) return doof::Success<std::wstring>{std::wstring()};
    const int size = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size == 0) return doof::Failure<std::string>{errorText("invalid-url", "Failed to decode UTF-8")};
    std::wstring result(static_cast<size_t>(size), L'\0');
    if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size) == 0) {
        return doof::Failure<std::string>{errorText("invalid-url", "Failed to decode UTF-8")};
    }
    return doof::Success<std::wstring>{result};
}

doof::Result<std::string, std::string> wideToUtf8(const std::wstring& value) {
    if (value.empty()) return doof::Success<std::string>{std::string()};
    const int size = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size == 0) return doof::Failure<std::string>{errorText("transport", "Failed to encode UTF-8")};
    std::string result(static_cast<size_t>(size), '\0');
    if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr) == 0) {
        return doof::Failure<std::string>{errorText("transport", "Failed to encode UTF-8")};
    }
    return doof::Success<std::string>{result};
}

class InternetHandle {
public:
    InternetHandle() = default;
    explicit InternetHandle(HINTERNET value) : value_(value) {}
    ~InternetHandle() { if (value_ != nullptr) ::WinHttpCloseHandle(value_); }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
    HINTERNET get() const { return value_; }
private:
    HINTERNET value_ = nullptr;
};

doof::Result<std::wstring, std::string> queryHeaderString(HINTERNET request, DWORD query) {
    DWORD size = 0;
    ::WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &size, WINHTTP_NO_HEADER_INDEX);
    if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER) return doof::Failure<std::string>{errorText("transport", "Failed to query HTTP response header")};
    std::vector<wchar_t> buffer(size / sizeof(wchar_t));
    if (!::WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX, buffer.data(), &size, WINHTTP_NO_HEADER_INDEX)) {
        return doof::Failure<std::string>{errorText("transport", "Failed to read HTTP response header")};
    }
    const size_t length = size / sizeof(wchar_t);
    return doof::Success<std::wstring>{std::wstring(buffer.data(), length > 0 && buffer[length - 1] == L'\0' ? length - 1 : length)};
}

}  // namespace

class NativeHttpClient::Impl {
public:
    Impl() : responseBody_(std::make_shared<std::vector<uint8_t>>()) {}

    doof::Result<int32_t, std::string> perform(const std::string& method, const std::string& url,
        const std::string& requestHeaders, std::shared_ptr<std::vector<uint8_t>> body,
        int32_t timeoutMs, bool followRedirects) {
        responseStatusText_.clear();
        responseHeadersText_.clear();
        responseBody_ = std::make_shared<std::vector<uint8_t>>();
        auto wideUrl = utf8ToWide(url);
        auto wideMethod = utf8ToWide(method);
        auto wideHeaders = utf8ToWide(requestHeaders);
        if (doof::is_failure(wideUrl)) return doof::Failure<std::string>{doof::failure_error(wideUrl)};
        if (doof::is_failure(wideMethod)) return doof::Failure<std::string>{doof::failure_error(wideMethod)};
        if (doof::is_failure(wideHeaders)) return doof::Failure<std::string>{doof::failure_error(wideHeaders)};

        URL_COMPONENTS components{};
        components.dwStructSize = sizeof(components);
        components.dwSchemeLength = static_cast<DWORD>(-1);
        components.dwHostNameLength = static_cast<DWORD>(-1);
        components.dwUrlPathLength = static_cast<DWORD>(-1);
        components.dwExtraInfoLength = static_cast<DWORD>(-1);
        if (!::WinHttpCrackUrl(doof::success_value(wideUrl).c_str(), 0, 0, &components)) {
            return doof::Failure<std::string>{errorText("invalid-url", "Failed to parse URL")};
        }
        const std::wstring host(components.lpszHostName, components.dwHostNameLength);
        std::wstring target(components.lpszUrlPath, components.dwUrlPathLength);
        target.append(components.lpszExtraInfo, components.dwExtraInfoLength);
        if (target.empty()) target = L"/";

        InternetHandle session(::WinHttpOpen(L"doof-http-client/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
        if (session.get() == nullptr) return doof::Failure<std::string>{errorText("transport", "Failed to create HTTP session")};
        const int timeout = timeoutMs > 0 ? timeoutMs : 30000;
        if (!::WinHttpSetTimeouts(session.get(), timeout, timeout, timeout, timeout)) return doof::Failure<std::string>{errorText("transport", "Failed to configure HTTP timeout")};
        InternetHandle connection(::WinHttpConnect(session.get(), host.c_str(), components.nPort, 0));
        if (connection.get() == nullptr) return doof::Failure<std::string>{errorText("connect", "Failed to connect to HTTP host")};
        const DWORD flags = components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
        InternetHandle request(::WinHttpOpenRequest(connection.get(), doof::success_value(wideMethod).c_str(), target.c_str(),
            nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
        if (request.get() == nullptr) return doof::Failure<std::string>{errorText("transport", "Failed to create HTTP request")};
        if (!followRedirects) {
            const DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
            if (!::WinHttpSetOption(request.get(), WINHTTP_OPTION_REDIRECT_POLICY, const_cast<DWORD*>(&policy), sizeof(policy))) {
                return doof::Failure<std::string>{errorText("transport", "Failed to configure redirect policy")};
            }
        }
        const DWORD bodySize = body == nullptr ? 0 : static_cast<DWORD>(std::min<size_t>(body->size(), std::numeric_limits<DWORD>::max()));
        if (body != nullptr && body->size() > std::numeric_limits<DWORD>::max()) return doof::Failure<std::string>{"transport|0|HTTP request body is too large"};
        const wchar_t* headers = requestHeaders.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : doof::success_value(wideHeaders).c_str();
        const DWORD headerLength = requestHeaders.empty() ? 0 : static_cast<DWORD>(-1);
        if (!::WinHttpSendRequest(request.get(), headers, headerLength, bodySize == 0 ? WINHTTP_NO_REQUEST_DATA : body->data(),
            bodySize, bodySize, 0)) return doof::Failure<std::string>{errorText(::GetLastError() == ERROR_WINHTTP_TIMEOUT ? "timeout" : "transport", "Failed to send HTTP request")};
        if (!::WinHttpReceiveResponse(request.get(), nullptr)) return doof::Failure<std::string>{errorText(::GetLastError() == ERROR_WINHTTP_TIMEOUT ? "timeout" : "transport", "Failed to receive HTTP response")};

        DWORD status = 0, statusSize = sizeof(status);
        if (!::WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX)) return doof::Failure<std::string>{errorText("transport", "Failed to read HTTP status")};
        auto statusText = queryHeaderString(request.get(), WINHTTP_QUERY_STATUS_TEXT);
        auto rawHeaders = queryHeaderString(request.get(), WINHTTP_QUERY_RAW_HEADERS_CRLF);
        if (doof::is_success(statusText)) {
            auto converted = wideToUtf8(doof::success_value(statusText));
            if (doof::is_success(converted)) responseStatusText_ = doof::success_value(converted);
        }
        if (doof::is_success(rawHeaders)) {
            auto converted = wideToUtf8(doof::success_value(rawHeaders));
            if (doof::is_success(converted)) responseHeadersText_ = doof::success_value(converted);
        }
        while (true) {
            DWORD available = 0;
            if (!::WinHttpQueryDataAvailable(request.get(), &available)) return doof::Failure<std::string>{errorText("transport", "Failed to query HTTP response body")};
            if (available == 0) break;
            const size_t offset = responseBody_->size();
            responseBody_->resize(offset + available);
            DWORD read = 0;
            if (!::WinHttpReadData(request.get(), responseBody_->data() + offset, available, &read)) return doof::Failure<std::string>{errorText("transport", "Failed to read HTTP response body")};
            responseBody_->resize(offset + read);
        }
        return doof::Success<int32_t>{static_cast<int32_t>(status)};
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
doof::Result<int32_t, std::string> NativeHttpClient::perform(const std::string& method, const std::string& url,
    const std::string& requestHeaders, std::shared_ptr<std::vector<uint8_t>> body, int32_t timeoutMs, bool followRedirects) {
    return impl_->perform(method, url, requestHeaders, std::move(body), timeoutMs, followRedirects);
}
std::string NativeHttpClient::responseStatusText() const { return impl_->responseStatusText(); }
std::string NativeHttpClient::responseHeadersText() const { return impl_->responseHeadersText(); }
std::shared_ptr<std::vector<uint8_t>> NativeHttpClient::responseBody() const { return impl_->responseBody(); }

class NativeHttpWebSocketConnectionImpl {};

doof::Result<std::shared_ptr<NativeHttpWebSocketConnection>, std::string> NativeHttpWebSocketConnection::connect(
    const std::string&, const std::string&, int32_t, int32_t, int32_t) {
    return doof::Failure<std::string>{"unsupported|0|WebSocket support is not implemented on Windows"};
}
NativeHttpWebSocketConnection::NativeHttpWebSocketConnection(std::shared_ptr<NativeHttpWebSocketConnectionImpl> impl) : impl_(std::move(impl)) {}
NativeHttpWebSocketConnection::~NativeHttpWebSocketConnection() = default;
void NativeHttpWebSocketConnection::start() {}
doof::Result<void, std::string> NativeHttpWebSocketConnection::sendText(const std::string&) { return doof::Failure<std::string>{"WebSocket support is not implemented on Windows"}; }
doof::Result<void, std::string> NativeHttpWebSocketConnection::sendBinary(std::shared_ptr<std::vector<uint8_t>>) { return doof::Failure<std::string>{"WebSocket support is not implemented on Windows"}; }
doof::Result<void, std::string> NativeHttpWebSocketConnection::ping() { return doof::Failure<std::string>{"WebSocket support is not implemented on Windows"}; }
doof::Result<void, std::string> NativeHttpWebSocketConnection::close(int32_t, const std::string&) { return doof::Failure<std::string>{"WebSocket support is not implemented on Windows"}; }
void NativeHttpWebSocketConnection::attachChannels(std::shared_ptr<std_::http::websocket::WebSocketConnection>, std::shared_ptr<EventSender>, std::shared_ptr<CommandReceiver>) {}
void NativeHttpWebSocketConnection::resumeInboundReads() {}
int32_t NativeHttpWebSocketConnection::state() const { return static_cast<int32_t>(NativeHttpWebSocketState::Error); }
