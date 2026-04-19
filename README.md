# http

HTTP client library backed by [libcurl](https://curl.se/libcurl/). Provides convenience functions for common request patterns and a lower-level `send` function for full control over headers, body, and timeouts.

> **Requires libcurl** — link via `"linkLibraries": ["curl"]` in `doof.json` (already configured in this package).

## Usage

```doof
import { createClient, get, postJsonValue, send, HttpRequest, HttpHeader } from "http"

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

### `HttpError`

Returned when a request fails at the transport level (not for non-2xx status codes — those are returned as successful `HttpResponse` values).

| Field | Type | Description |
|-------|------|-------------|
| `kind` | `string` | Error category (e.g. `"transport"`) |
| `code` | `string` | Numeric error code from libcurl |
| `message` | `string` | Human-readable error description |
