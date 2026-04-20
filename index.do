import { BlobReader, BlobBuilder } from "std/blob"
import { blobStreamToLineStream } from "std/stream"
import { parseJsonValue, formatJsonValue } from "std/json"

// Candidate HTTP client library backed by a small libcurl bridge.

export import class NativeHttpClient from "./native_http_client.hpp" {
  perform(method: string, url: string, requestHeaders: string, body: readonly byte[] | null,
          timeoutMs: int, followRedirects: bool): Result<int, string>
  responseStatusText(): string
  responseHeadersText(): string
  responseBody(): readonly byte[]
}

class BodyChunkStream implements Stream<readonly byte[]> {
  chunk: readonly byte[] = []
  consumed: bool = false

  next(): readonly byte[] | null {
    if this.consumed {
      return null
    }

    this.consumed = true
    return this.chunk
  }
}

export class HttpHeader {
  readonly name: string
  readonly value: string
}

export class HttpRequest {
  readonly method: string
  readonly url: string
  readonly headers: HttpHeader[] = []
  readonly body: readonly byte[] | null = null
  readonly timeoutMs: int = 30000
  readonly followRedirects: bool = true

  header(name: string): string | null {
    lowerName := name.toLowerCase()
    for entry of headers {
      if entry.name.toLowerCase() == lowerName {
        return entry.value
      }
    }
    return null
  }
}

export class HttpResponse {
  readonly status: int
  readonly statusText: string
  readonly headers: HttpHeader[]
  readonly body: readonly byte[]

  ok(): bool {
    return this.status >= 200 && this.status < 300
  }

  header(name: string): string | null {
    lowerName := name.toLowerCase()
    for entry of headers {
      if entry.name.toLowerCase() == lowerName {
        return entry.value
      }
    }
    return null
  }

  getBlob(): readonly byte[] {
    return this.body
  }

  getText(): string {
    reader := BlobReader(this.body)
    return reader.readString(reader.remaining())
  }

  getLineStream(): Stream<string> {
    return blobStreamToLineStream(BodyChunkStream {
      chunk: this.body,
    })
  }

  getJsonValue(): Result<JsonValue, string> {
    return parseJsonValue(this.getText())
  }
}

export class HttpError {
  readonly kind: string
  readonly code: string
  readonly message: string
}

export class HttpClient {
  readonly native: NativeHttpClient
}

export function createClient(): HttpClient {
  return HttpClient {
    native: NativeHttpClient(),
  }
}

function newRequest(method: string, url: string): HttpRequest {
  return HttpRequest {
    method,
    url,
    body: null,
  }
}

export function get(client: HttpClient, url: string): Result<HttpResponse, HttpError> {
  return send(client, newRequest("GET", url))
}

export function postJsonValue(client: HttpClient, url: string, body: JsonValue): Result<HttpResponse, HttpError> {
  builder := BlobBuilder()
  builder.writeString(formatJsonValue(body))
  headers := readonly [HttpHeader {
    name: "Content-Type",
    value: "application/json",
  }]

  return send(client, HttpRequest {
    method: "POST",
    url,
    headers,
    body: builder.build(),
    timeoutMs: 30000,
    followRedirects: true,
  })
}

export function send(client: HttpClient, request: HttpRequest): Result<HttpResponse, HttpError> {
  nativeResult := client.native.perform(
    request.method,
    request.url,
    renderHeaders(request.headers),
    request.body,
    request.timeoutMs,
    request.followRedirects,
  )

  return case nativeResult {
    s: Success => Success {
      value: HttpResponse {
        status: s.value,
        statusText: client.native.responseStatusText(),
        headers: parseHeaders(client.native.responseHeadersText()),
        body: client.native.responseBody(),
      }
    },
    f: Failure => Failure {
      error: parseError(f.error)
    }
  }
}

function renderHeaders(headers: readonly HttpHeader[]): string {
  let text = ""
  for header of headers {
    text += "${header.name}: ${header.value}\r\n"
  }
  return text
}

function parseHeaders(headerText: string): readonly HttpHeader[] {
  headers: HttpHeader[] := []
  lines := headerText.split("\r\n")
  for line of lines {
    if line == "" {
      continue
    }

    separator := line.indexOf(":")
    if separator <= 0 {
      continue
    }

    headers.push(HttpHeader {
      name: line.substring(0, separator).trim(),
      value: line.slice(separator + 1).trim(),
    })
  }
  return headers.buildReadonly()
}

function parseError(raw: string): HttpError {
  firstSeparator := raw.indexOf("|")
  if firstSeparator < 0 {
    return HttpError {
      kind: "transport",
      code: "0",
      message: raw,
    }
  }

  remainder := raw.slice(firstSeparator + 1)
  secondSeparator := remainder.indexOf("|")
  if secondSeparator < 0 {
    return HttpError {
      kind: raw.substring(0, firstSeparator),
      code: "0",
      message: remainder,
    }
  }

  kind := raw.substring(0, firstSeparator)
  codeText := remainder.substring(0, secondSeparator)
  message := remainder.slice(secondSeparator + 1)

  return HttpError {
    kind,
    code: codeText,
    message,
  }
}