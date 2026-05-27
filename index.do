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

  next(): bool {
    if this.consumed {
      return false
    }

    this.consumed = true
    return true
  }

  value(): readonly byte[] => this.chunk
}

export class HttpHeader {
  readonly name: string
  readonly value: string
}

export class Cookie {
  readonly name: string
  readonly value: string
}

export class SetCookie {
  readonly name: string
  readonly value: string
  readonly domain: string | null = null
  readonly path: string | null = null
  readonly expires: string | null = null
  readonly maxAge: string | null = null
  readonly secure: bool = false
  readonly httpOnly: bool = false
  readonly sameSite: string | null = null
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

export function parseCookieHeader(header: string): readonly Cookie[] {
  cookies: Cookie[] := []
  parts := header.split(";")
  for part of parts {
    separator := part.indexOf("=")
    if separator <= 0 {
      continue
    }

    name := part.substring(0, separator).trim()
    if name == "" {
      continue
    }

    cookies.push(Cookie {
      name,
      value: part.slice(separator + 1).trim(),
    })
  }
  return cookies.buildReadonly()
}

export function renderCookieHeader(cookies: readonly Cookie[]): string {
  let text = ""
  let first = true
  for cookie of cookies {
    if cookie.name == "" {
      continue
    }

    if first {
      first = false
    } else {
      text += "; "
    }
    text += "${cookie.name}=${cookie.value}"
  }
  return text
}

export function parseSetCookieHeader(header: string): SetCookie | null {
  parts := header.split(";")
  if parts.length == 0 {
    return null
  }

  firstPart := parts[0].trim()
  firstSeparator := firstPart.indexOf("=")
  if firstSeparator <= 0 {
    return null
  }

  name := firstPart.substring(0, firstSeparator).trim()
  if name == "" {
    return null
  }

  let domain: string | null = null
  let path: string | null = null
  let expires: string | null = null
  let maxAge: string | null = null
  let secure = false
  let httpOnly = false
  let sameSite: string | null = null

  let index = 1
  while index < parts.length {
    attribute := parts[index].trim()
    index += 1
    if attribute == "" {
      continue
    }

    separator := attribute.indexOf("=")
    let attributeName = attribute
    let attributeValue = ""
    if separator >= 0 {
      attributeName = attribute.substring(0, separator).trim()
      attributeValue = attribute.slice(separator + 1).trim()
    }

    lowerName := attributeName.toLowerCase()
    if lowerName == "domain" {
      domain = attributeValue
    } else if lowerName == "path" {
      path = attributeValue
    } else if lowerName == "expires" {
      expires = attributeValue
    } else if lowerName == "max-age" {
      maxAge = attributeValue
    } else if lowerName == "secure" {
      secure = true
    } else if lowerName == "httponly" {
      httpOnly = true
    } else if lowerName == "samesite" {
      sameSite = attributeValue
    }
  }

  return SetCookie {
    name,
    value: firstPart.slice(firstSeparator + 1).trim(),
    domain,
    path,
    expires,
    maxAge,
    secure,
    httpOnly,
    sameSite,
  }
}

export function renderSetCookieHeader(cookie: SetCookie): string {
  let text = "${cookie.name}=${cookie.value}"
  expires := cookie.expires
  if expires != null {
    text += "; Expires=${expires!}"
  }
  maxAge := cookie.maxAge
  if maxAge != null {
    text += "; Max-Age=${maxAge!}"
  }
  domain := cookie.domain
  if domain != null {
    text += "; Domain=${domain!}"
  }
  path := cookie.path
  if path != null {
    text += "; Path=${path!}"
  }
  sameSite := cookie.sameSite
  if sameSite != null {
    text += "; SameSite=${sameSite!}"
  }
  if cookie.secure {
    text += "; Secure"
  }
  if cookie.httpOnly {
    text += "; HttpOnly"
  }
  return text
}

export function cookieValue(cookies: readonly Cookie[], name: string): string | null {
  for cookie of cookies {
    if cookie.name == name {
      return cookie.value
    }
  }
  return null
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
    s: Success -> Success {
      value: HttpResponse {
        status: s.value,
        statusText: client.native.responseStatusText(),
        headers: parseHeaders(client.native.responseHeadersText()),
        body: client.native.responseBody(),
      }
    },
    f: Failure -> Failure {
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
