# std/http Guide And API Reference

`std/http` is the outbound HTTP client package. The public API is organized
around reusable clients, explicit request and response objects, cookie header
helpers, and outbound WebSockets.

Apple targets use Foundation transports. Non-Apple targets use a pinned,
vendored curl build. The transport backend is hidden behind the Doof API.

## Client Lifecycle

Create one `HttpClient` with `createClient()` and reuse it:

```doof
import { createClient, get } from "std/http"

client := createClient()
response := try! get(client, "https://example.com")
```

`HttpClient` is an opaque native handle. Per-request behavior belongs on
`HttpRequest`, so a client can be shared across many calls.

## Request Flow

Use `get` and `postJsonValue` for common requests. Use `send` with
`HttpRequest` when you need custom methods, headers, body bytes, timeouts, or
redirect behavior.

```doof
import { createClient, send, HttpHeader, HttpRequest } from "std/http"

client := createClient()

request := HttpRequest {
  method: "PATCH",
  url: "https://api.example.com/users/42",
  headers: [
    HttpHeader {
      name: "Content-Type",
      value: "application/json",
    },
  ],
  timeoutMs: 5000,
}

response := try! send(client, request)
```

HTTP error status codes are represented as successful `HttpResponse` values.
Check `response.ok()` or inspect `response.status` for application-level
handling. Transport errors, invalid URLs, and backend failures return
`Failure<HttpError>`.

## Response Bodies

Responses are buffered by the native backend and exposed through helpers:

- `getBlob()` returns raw response bytes.
- `getText()` decodes bytes as UTF-8.
- `getLineStream()` wraps the buffered body in a line stream.
- `getJsonValue()` parses the body text as JSON.

```doof
response := try! get(client, "https://api.example.com/config")
config := try! response.getJsonValue()
```

## Cookies

Cookie helpers operate on header values. They do not maintain a cookie jar or
perform validation, encoding, expiry, domain, or path policy.

`parseCookieHeader` preserves order and duplicate names, skips malformed pairs,
trims names and values, and does not decode values. `parseSetCookieHeader`
parses common attributes case-insensitively and ignores unknown attributes.
Rendering functions emit values as provided.

```doof
import {
  Cookie,
  SetCookie,
  cookieValue,
  parseCookieHeader,
  renderCookieHeader,
  renderSetCookieHeader,
} from "std/http"

cookies := parseCookieHeader("session=abc; theme=dark")
session := cookieValue(cookies, "session")

requestHeader := renderCookieHeader(readonly [
  Cookie { name: "session", value: "abc" },
])

responseHeader := renderSetCookieHeader(SetCookie {
  name: "session",
  value: "abc",
  path: "/",
  secure: true,
  httpOnly: true,
})
```

## WebSockets

`connectWebSocket(url, options)` supports `ws://` and `wss://` URLs. The
returned connection has paired bounded channels:

- `events` receives open, text, binary, writable, close, and error events.
- `commands` sends text, binary, ping, and close commands.

Inbound backpressure pauses native reads when the event channel is full.
Outbound backpressure is governed by the command channel. Message commands can
carry an optional `coalesceKey`; command coalescing is handled by the command
channel.

```doof
import {
  WebSocketSendText,
  WebSocketText,
  connectWebSocket,
} from "std/http"

socket := try! connectWebSocket("wss://example.com/socket")

socket.events.onMessage((event): void => {
  text := event as WebSocketText
  case text {
    s: Success -> println(s.value.text)
    _: Failure -> {}
  }
})

try! socket.commands.send(WebSocketSendText {
  text: "hello",
})
```

## HTTP Functions

### `createClient(): HttpClient`

Create a new `HttpClient`. Reuse clients across requests.

### `get(client: HttpClient, url: string): Result<HttpResponse, HttpError>`

Send a `GET` request to `url`.

### `postJsonValue(client: HttpClient, url: string, body: JsonValue): Result<HttpResponse, HttpError>`

Send a `POST` request with a JSON-serialized body. Automatically sets
`Content-Type: application/json`.

### `send(client: HttpClient, request: HttpRequest): Result<HttpResponse, HttpError>`

Send an arbitrary request.

## Request And Response Types

### `HttpClient`

An opaque client handle returned by `createClient()`.

### `HttpRequest`

Describes an outgoing HTTP request.

| Field | Type | Default | Description |
| --- | --- | --- | --- |
| `method` | `string` | Required | HTTP method, such as `"GET"` or `"POST"` |
| `url` | `string` | Required | Request URL |
| `headers` | `HttpHeader[]` | `[]` | Request headers |
| `body` | `readonly byte[] \| null` | `null` | Raw request body |
| `timeoutMs` | `int` | `30000` | Request timeout in milliseconds |
| `followRedirects` | `bool` | `true` | Whether the backend follows redirects |

#### `header(name: string): string | null`

Return the first matching request header value, ignoring case.

### `HttpResponse`

Describes a completed HTTP response.

| Field | Type | Description |
| --- | --- | --- |
| `status` | `int` | HTTP status code, such as `200` or `404` |
| `statusText` | `string` | Status line text, such as `"OK"` |
| `headers` | `readonly HttpHeader[]` | Response headers |
| `body` | `readonly byte[]` | Raw response body bytes |

#### `ok(): bool`

Return `true` for `2xx` status codes.

#### `header(name: string): string | null`

Return the first matching response header value, ignoring case.

#### `getBlob(): readonly byte[]`

Return the raw response body bytes.

#### `getText(): string`

Decode the response body as UTF-8.

#### `getLineStream(): Stream<string>`

Return the response body as a line stream.

#### `getJsonValue(): Result<JsonValue, string>`

Parse the response body as JSON.

### `HttpHeader`

| Field | Type | Description |
| --- | --- | --- |
| `name` | `string` | Header name |
| `value` | `string` | Header value |

### `HttpError`

Returned when a request fails at the transport level.

| Field | Type | Description |
| --- | --- | --- |
| `kind` | `string` | Error category, such as `"transport"` |
| `code` | `string` | Backend error code, such as a curl or Foundation code |
| `message` | `string` | Human-readable error description |

## Cookie API

### `parseCookieHeader(header: string): readonly Cookie[]`

Parse a `Cookie` request header into ordered cookie entries.

### `renderCookieHeader(cookies: readonly Cookie[]): string`

Render cookies as a `Cookie` request header value. Cookies with empty names are
skipped.

### `cookieValue(cookies: readonly Cookie[], name: string): string | null`

Return the first cookie value matching `name`, or `null`.

### `parseSetCookieHeader(header: string): SetCookie | null`

Parse a `Set-Cookie` response header into a `SetCookie`, or return `null` when
the required name/value pair is missing or invalid.

### `renderSetCookieHeader(cookie: SetCookie): string`

Render a `Set-Cookie` response header value. Attributes are emitted in stable
order: `Expires`, `Max-Age`, `Domain`, `Path`, `SameSite`, `Secure`,
`HttpOnly`.

### `Cookie`

| Field | Type | Description |
| --- | --- | --- |
| `name` | `string` | Cookie name |
| `value` | `string` | Cookie value |

### `SetCookie`

| Field | Type | Default | Description |
| --- | --- | --- | --- |
| `name` | `string` | Required | Cookie name |
| `value` | `string` | Required | Cookie value |
| `expires` | `string \| null` | `null` | Raw `Expires` attribute |
| `maxAge` | `string \| null` | `null` | Raw `Max-Age` attribute |
| `domain` | `string \| null` | `null` | Cookie domain |
| `path` | `string \| null` | `null` | Cookie path |
| `sameSite` | `string \| null` | `null` | Raw `SameSite` attribute |
| `secure` | `bool` | `false` | Whether to render `Secure` |
| `httpOnly` | `bool` | `false` | Whether to render `HttpOnly` |

## WebSocket API

### `connectWebSocket(url: string, options: WebSocketOptions = WebSocketOptions {}): Result<WebSocketConnection, HttpError>`

Open a WebSocket connection.

### `WebSocketOptions`

| Field | Type | Default | Description |
| --- | --- | --- | --- |
| `eventCapacity` | `int` | `1024` | Maximum queued inbound events before reads pause |
| `commandCapacity` | `int` | `1024` | Maximum queued outbound commands |
| `headers` | `readonly HttpHeader[]` | `[]` | Additional handshake headers |
| `timeoutMs` | `int` | `30000` | Connection timeout in milliseconds |

### `WebSocketConnection`

| Field | Type | Description |
| --- | --- | --- |
| `url` | `string` | Original WebSocket URL |
| `events` | `ChannelReceiver<WebSocketEvent>` | Inbound event channel |
| `commands` | `ChannelSender<WebSocketCommand>` | Outbound command channel |

#### `state(): WebSocketState`

Return the current connection state.

#### `close(): void`

Queue a normal close and close the public channels.

### `WebSocketEvent`

Union of:

- `WebSocketOpen`
- `WebSocketText`
- `WebSocketBinary`
- `WebSocketWritable`
- `WebSocketClose`
- `WebSocketError`

### `WebSocketCommand`

Union of:

- `WebSocketSendText`
- `WebSocketSendBinary`
- `WebSocketPing`
- `WebSocketCloseCommand`

### `WebSocketSendText`

| Field | Type | Default | Description |
| --- | --- | --- | --- |
| `text` | `string` | Required | Text message to send |
| `coalesceKey` | `string \| null` | `null` | Optional command coalescing key |

### `WebSocketSendBinary`

| Field | Type | Default | Description |
| --- | --- | --- | --- |
| `bytes` | `readonly byte[]` | Required | Binary message to send |
| `coalesceKey` | `string \| null` | `null` | Optional command coalescing key |

### `WebSocketPing`

Send a ping frame.

### `WebSocketCloseCommand`

| Field | Type | Default | Description |
| --- | --- | --- | --- |
| `code` | `int` | `1000` | WebSocket close code |
| `reason` | `string` | `""` | WebSocket close reason |

## Backend Notes

Non-Apple targets acquire the pinned curl source archive into `vendor/curl`,
build a static curl archive under `vendor/curl/.doof-build/<target>`, and link
against that archive through the package's native build metadata. Apple targets
link against Foundation and do not build curl.
