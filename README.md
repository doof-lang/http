# http

HTTP client library backed by [libcurl](https://curl.se/libcurl/). Provides convenience functions for common request patterns and a lower-level `send` function for full control over headers, body, and timeouts.

> **Requires libcurl** — link via `"linkLibraries": ["curl"]` in `doof.json` (already configured in this package).

## Usage

```doof
import {
  Cookie,
  SetCookie,
  cookieValue,
  createClient,
  get,
  parseCookieHeader,
  postJsonValue,
  renderCookieHeader,
  renderSetCookieHeader,
  send,
  HttpRequest,
  HttpHeader,
  connectWebSocket,
  WebSocketSendText,
  WebSocketText,
} from "http"

client := createClient()

// GET
response := try get(client, "https://api.example.com/users")
if response.ok() {
  println(response.getText())
}

// POST JSON
body: JsonValue := { name: "Alice", role: "admin" }
response := try postJsonValue(client, "https://api.example.com/users", body)

// Custom request
request := HttpRequest {
  method: "DELETE",
  url: "https://api.example.com/users/42",
  headers: [HttpHeader { name: "Authorization", value: "Bearer ${token}" }],
  timeoutMs: 5000,
}
response := try send(client, request)

// Cookies
cookies := parseCookieHeader("session=abc; theme=dark")
session := cookieValue(cookies, "session")

cookieHeader := renderCookieHeader(readonly [
  Cookie { name: "session", value: "abc" },
  Cookie { name: "theme", value: "dark" },
])

setCookieHeader := renderSetCookieHeader(SetCookie {
  name: "session",
  value: "abc",
  path: "/",
  httpOnly: true,
  secure: true,
  sameSite: "Lax",
})

// WebSocket
socket := try connectWebSocket("wss://example.com/socket")
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

## Exports

### `createClient(): HttpClient`

Create a new `HttpClient` instance. Clients are stateless between requests and safe to reuse.

---

### `get(client: HttpClient, url: string): Result<HttpResponse, HttpError>`

Send a `GET` request to `url`.

---

### `postJsonValue(client: HttpClient, url: string, body: JsonValue): Result<HttpResponse, HttpError>`

Send a `POST` request with a JSON-serialized body. Automatically sets `Content-Type: application/json`.

---

### `send(client: HttpClient, request: HttpRequest): Result<HttpResponse, HttpError>`

Send an arbitrary `HttpRequest`. Use this for full control over method, headers, body, timeout, and redirect behaviour.

---

### `parseCookieHeader(header: string): readonly Cookie[]`

Parse a `Cookie` header into ordered `Cookie` entries. Parsing is lenient: empty or malformed pairs are ignored, names and values are trimmed, duplicate names are preserved, and values are not decoded.

---

### `renderCookieHeader(cookies: readonly Cookie[]): string`

Render cookies as a `Cookie` request header value. Empty cookie names are skipped. Names and values are emitted as provided; callers are responsible for validation and encoding.

---

### `cookieValue(cookies: readonly Cookie[], name: string): string | null`

Return the value of the first cookie matching `name`, or `null` if none exists.

---

### `parseSetCookieHeader(header: string): SetCookie | null`

Parse a `Set-Cookie` response header into a `SetCookie`, or `null` when the required `name=value` pair is missing or invalid. Common attributes are parsed case-insensitively: `Expires`, `Max-Age`, `Domain`, `Path`, `SameSite`, `Secure`, and `HttpOnly`. Unknown attributes are ignored.

---

### `renderSetCookieHeader(cookie: SetCookie): string`

Render a `Set-Cookie` header value using stable attribute order: `Expires`, `Max-Age`, `Domain`, `Path`, `SameSite`, `Secure`, `HttpOnly`. Values are emitted as provided; callers are responsible for validation and encoding.

---

### `connectWebSocket(url: string, options: WebSocketOptions = WebSocketOptions {}): Result<WebSocketConnection, HttpError>`

Open a `ws://` or `wss://` WebSocket using libcurl's built-in WebSocket support. The returned connection has paired channels:

- `events: ChannelReceiver<WebSocketEvent>` for open, text, binary, writable, close, and error events.
- `commands: ChannelSender<WebSocketCommand>` for text, binary, ping, and close commands.

Both channels are bounded. If inbound events reach the high-water mark, native socket reads pause until the event channel reports ready again. Command sends use the command channel's normal `Backpressure` / `SendError` result, and the native outbound queue is bounded by `commandCapacity`.

---

### `WebSocketOptions`

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `eventCapacity` | `int` | `1024` | Maximum queued inbound events before reads pause |
| `commandCapacity` | `int` | `1024` | Maximum queued outbound commands |
| `headers` | `readonly HttpHeader[]` | `[]` | Additional handshake headers |
| `timeoutMs` | `int` | `30000` | Connection timeout in milliseconds |

---

### `WebSocketConnection`

| Field | Type | Description |
|-------|------|-------------|
| `url` | `string` | Original WebSocket URL |
| `events` | `ChannelReceiver<WebSocketEvent>` | Inbound event channel |
| `commands` | `ChannelSender<WebSocketCommand>` | Outbound command channel |

#### `state(): WebSocketState`

Return the current connection state.

#### `close(): void`

Queue a normal WebSocket close and close the public channels.

---

### `HttpClient`

An opaque client handle returned by `createClient`. Pass it to `get`, `postJsonValue`, or `send`.

---

### `HttpRequest`

Describes an outgoing HTTP request.

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `method` | `string` | — | HTTP method (`"GET"`, `"POST"`, etc.) |
| `url` | `string` | — | Request URL |
| `headers` | `HttpHeader[]` | `[]` | Request headers |
| `body` | `readonly byte[] \| null` | `null` | Raw request body |
| `timeoutMs` | `int` | `30000` | Request timeout in milliseconds |
| `followRedirects` | `bool` | `true` | Whether to follow HTTP redirects |

#### `header(name: string): string | null`

Return the value of the first header matching `name` (case-insensitive), or `null`.

---

### `HttpResponse`

Describes a completed HTTP response.

| Field | Type | Description |
|-------|------|-------------|
| `status` | `int` | HTTP status code (e.g. `200`, `404`) |
| `statusText` | `string` | Status line text (e.g. `"OK"`) |
| `headers` | `readonly HttpHeader[]` | Response headers |
| `body` | `readonly byte[]` | Raw response body bytes |

#### `ok(): bool`

Return `true` if the status code is in the `200`–`299` range.

#### `header(name: string): string | null`

Return the value of the first response header matching `name` (case-insensitive), or `null`.

#### `getBlob(): readonly byte[]`

Return the raw body bytes.

#### `getText(): string`

Decode the body as a UTF-8 string.

#### `getLineStream(): Stream<string>`

Return the body as a lazy `Stream<string>`, yielding one line at a time.

#### `getJsonValue(): Result<JsonValue, string>`

Parse the body text as JSON. Returns a `Failure` with a message if parsing fails.

---

### `HttpHeader`

A name/value pair representing a single HTTP header.

| Field | Type | Description |
|-------|------|-------------|
| `name` | `string` | Header name |
| `value` | `string` | Header value |

---

### `Cookie`

A name/value pair parsed from or rendered into a `Cookie` request header.

| Field | Type | Description |
|-------|------|-------------|
| `name` | `string` | Cookie name |
| `value` | `string` | Cookie value |

---

### `SetCookie`

A cookie plus common attributes parsed from or rendered into a `Set-Cookie` response header. Attribute values are raw strings.

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `name` | `string` | — | Cookie name |
| `value` | `string` | — | Cookie value |
| `expires` | `string \| null` | `null` | Raw `Expires` attribute |
| `maxAge` | `string \| null` | `null` | Raw `Max-Age` attribute |
| `domain` | `string \| null` | `null` | Cookie domain |
| `path` | `string \| null` | `null` | Cookie path |
| `sameSite` | `string \| null` | `null` | Raw `SameSite` attribute |
| `secure` | `bool` | `false` | Whether to render `Secure` |
| `httpOnly` | `bool` | `false` | Whether to render `HttpOnly` |

---

### `HttpError`

Returned when a request fails at the transport level (not for non-2xx status codes — those are returned as successful `HttpResponse` values).

| Field | Type | Description |
|-------|------|-------------|
| `kind` | `string` | Error category (e.g. `"transport"`) |
| `code` | `string` | Numeric error code from libcurl |
| `message` | `string` | Human-readable error description |
