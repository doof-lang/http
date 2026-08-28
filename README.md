# std/http

`std/http` is Doof's outbound HTTP client module. It covers ordinary HTTP
requests, JSON request and response helpers, cookie header utilities, and
client-side WebSocket connections.

Apple targets use Foundation transports. Windows uses WinHTTP, Linux uses the
pinned, statically built curl transport, and browser WebAssembly uses the
host's `fetch()` through JavaScript Promise Integration (JSPI). Application
code uses the same Doof API on every backend.

## Documentation

- [Guide and API reference](docs/API.md) has the complete API map.
- Tests can be run with `doof test http`.
- Browser bridge tests can be run with
  `node --test http/tests/doof-http.test.mjs`.

## Quick Start

```doof
import { createClient, get } from "std/http"

client := createClient()
response := try! get(client, "https://example.com")

if response.ok() {
  println(response.getText())
} else {
  println("HTTP ${response.status}: ${response.statusText}")
}
```

Create one `HttpClient` with `createClient()` and reuse it. The client is an
opaque native handle; method, URL, headers, body, timeout, and redirect behavior
all live on each request.

HTTP status codes such as `404` and `500` still return `Success<HttpResponse>`.
Only transport-level failures, invalid URLs, and native backend errors return
`Failure<HttpError>`.

## Browser WebAssembly

The WebAssembly backend preserves the synchronous Doof API. While `send`,
`get`, or `postJsonValue` is waiting for browser `fetch()`, JSPI suspends the
WebAssembly computation and resumes it when the Promise settles.

The JavaScript host must attach the included bridge when instantiating the
module. Any WebAssembly export that can reach an HTTP request must be invoked
through `WebAssembly.promising`:

```js
import { createHttpBridge } from "./std/http/doof-http.js";

const http = createHttpBridge();
const result = await WebAssembly.instantiateStreaming(fetch("./app.wasm"), {
  doof_http: http.imports,
  // Include the application's normal WASI and other stdlib imports here.
});
const instance = result.instance ?? result;
http.attach(instance);

const load = WebAssembly.promising(instance.exports.doof_export_load);
const resultPointer = await load(parametersPointer);
```

The normal Doof WebAssembly JSON ABI still applies to exported function
arguments and results. A host loader should wrap exports centrally rather than
requiring individual call sites to use `WebAssembly.promising`.

Browser security and fetch semantics apply. CORS controls cross-origin access,
forbidden request headers cannot be set, `Set-Cookie` is not exposed, cookies
use fetch's default `same-origin` credentials policy, and manual cross-origin
redirects can produce opaque responses. Bodies are currently buffered. Hosts
may set a limit with `createHttpBridge({ maxResponseBytes })`; the default is
unlimited. WebSockets do not yet have a WebAssembly transport.

The bridge checks for JSPI when it is created and reports a clear error on
unsupported browsers.

Unicode hostname labels in caller-supplied `http`, `https`, `ws`, and `wss`
URLs are converted to RFC 3492 `xn--` form before reaching the native backend.
The original `HttpRequest.url` and `WebSocketConnection.url` remain unchanged.
This is Punycode conversion rather than full IDNA processing: it does not apply
Unicode normalization, case mapping, UTS #46 rules, or DNS character and length
validation. Redirect URLs remain managed by the native transport.

## Sending Requests

Use `get` and `postJsonValue` for common calls:

```doof
import { createClient, get, postJsonValue } from "std/http"

client := createClient()

users := try! get(client, "https://api.example.com/users")

created := try! postJsonValue(client, "https://api.example.com/users", {
  name: "Alice",
  role: "admin",
})
```

Use `send` with `HttpRequest` when you need custom methods, headers, a raw body,
timeouts, or redirect control:

```doof
import { createClient, send, HttpHeader, HttpRequest } from "std/http"

request := HttpRequest {
  method: "DELETE",
  url: "https://api.example.com/users/42",
  headers: [
    HttpHeader {
      name: "Authorization",
      value: "Bearer ${token}",
    },
  ],
  timeoutMs: 5000,
  followRedirects: false,
}

response := try! send(createClient(), request)
```

`HttpRequest.header(name)` returns the first matching request header, ignoring
case.

## Reading Responses

`HttpResponse` stores the status, status text, response headers, and buffered
body bytes.

```doof
response := try! get(client, "https://api.example.com/users/42")

contentType := response.header("Content-Type")
bodyText := response.getText()
bodyBytes := response.getBlob()
json := try! response.getJsonValue()
```

Available body helpers:

- `getBlob()` returns the raw body bytes.
- `getText()` decodes the body as UTF-8.
- `getLineStream()` exposes the buffered body as a `Stream<string>`.
- `getJsonValue()` parses the body text as JSON.

`HttpResponse.header(name)` returns the first matching response header, ignoring
case.

## Cookies

The cookie helpers work with header values rather than managing a cookie jar.
This keeps request state explicit and lets callers choose their own persistence
and validation policy.

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
```

Parsing is intentionally lenient: malformed pairs are skipped, duplicate names
are preserved, common `Set-Cookie` attributes are parsed case-insensitively, and
unknown attributes are ignored. Values are emitted as provided, so callers are
responsible for validating and encoding names and values before producing
headers.

## WebSockets

`connectWebSocket(url, options)` opens a `ws://` or `wss://` connection and
returns paired bounded channels:

- `events` receives open, text, binary, writable, close, and error events.
- `commands` sends text, binary, ping, and close commands.

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

Inbound backpressure pauses native reads when the event channel is full.
Outbound backpressure is governed by the command channel. Use
`WebSocketOptions` to set handshake headers, timeout, and channel capacities.

## API Overview

HTTP client:

- `createClient(): HttpClient`
- `get(client, url): Result<HttpResponse, HttpError>`
- `postJsonValue(client, url, body): Result<HttpResponse, HttpError>`
- `send(client, request): Result<HttpResponse, HttpError>`

Request and response types:

- `HttpClient`
- `HttpRequest`
- `HttpResponse`
- `HttpHeader`
- `HttpError`

Cookie helpers:

- `Cookie`
- `SetCookie`
- `parseCookieHeader`
- `renderCookieHeader`
- `parseSetCookieHeader`
- `renderSetCookieHeader`
- `cookieValue`

WebSocket types:

- `WebSocketOptions`
- `WebSocketConnection`
- `WebSocketEvent`
- `WebSocketCommand`
- `WebSocketSendText`
- `WebSocketSendBinary`
- `WebSocketPing`
- `WebSocketCloseCommand`
- `connectWebSocket`

See [docs/API.md](docs/API.md) for field tables and detailed behavior.
