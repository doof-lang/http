#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
#include <curl/curl.h>
#include "doof_runtime.hpp"

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