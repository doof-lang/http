import { BlobBuilder } from "std/blob"

import { HttpError, HttpHeader } from "./types"
import { prepareTransportUrl } from "./transport_url"

import class NativeHttpWebSocketConnection from "./native_http_client.hpp" {
  isolated static connect(
    url: string,
    requestHeaders: string,
    timeoutMs: int,
    outboundCapacity: int,
    eventCapacity: int,
  ): Result<NativeHttpWebSocketConnection, string>

  isolated sendText(text: string): Result<none, string>
  isolated sendBinary(bytes: readonly byte[]): Result<none, string>
  isolated ping(): Result<none, string>
  isolated close(code: int, reason: string): Result<none, string>
  isolated attachHandler(
    connection: WebSocketConnection,
    onEvent: (event: WebSocketEvent): none,
    eventCapacity: int,
  ): none
  isolated start(): none
  isolated eventHandled(): none
  isolated resumeInboundReads(): none
  isolated state(): int
}

export enum WebSocketState {
  Connecting,
  Open,
  Closing,
  Closed,
  Error,
}

export readonly WEBSOCKET_CLOSE_NORMAL = 1000
export readonly WEBSOCKET_CLOSE_GOING_AWAY = 1001
export readonly WEBSOCKET_CLOSE_PROTOCOL_ERROR = 1002
export readonly WEBSOCKET_CLOSE_UNSUPPORTED_DATA = 1003
export readonly WEBSOCKET_CLOSE_INVALID_PAYLOAD = 1007
export readonly WEBSOCKET_CLOSE_POLICY_VIOLATION = 1008
export readonly WEBSOCKET_CLOSE_MESSAGE_TOO_BIG = 1009
export readonly WEBSOCKET_CLOSE_INTERNAL_ERROR = 1011

export class WebSocketOptions {
  // Inbound events posted to the handler but not yet handled before the
  // connection stops reading from the socket.
  readonly eventCapacity: int = 1024
  // Sends accepted but not yet written before `sendText` and friends fail
  // with a backpressure error; `WebSocketWritable` reports the queue drained.
  readonly outboundCapacity: int = 1024
  readonly headers: readonly HttpHeader[] = []
  readonly timeoutMs: int = 30000
}

export type WebSocketEvent =
  WebSocketOpen |
  WebSocketText |
  WebSocketBinary |
  WebSocketWritable |
  WebSocketClose |
  WebSocketError

export class WebSocketOpen {
  connection: WebSocketConnection
}

export class WebSocketText {
  connection: WebSocketConnection
  readonly text: string
}

export class WebSocketBinary {
  connection: WebSocketConnection
  readonly bytes: readonly byte[]
}

export class WebSocketWritable {
  connection: WebSocketConnection
}

export class WebSocketClose {
  connection: WebSocketConnection
  readonly code: int
  readonly reason: string
  readonly wasClean: bool
}

export class WebSocketError {
  connection: WebSocketConnection
  readonly error: HttpError
}

export class WebSocketConnection {
  readonly url: string
  readonly options: WebSocketOptions = WebSocketOptions {}
  private native: NativeHttpWebSocketConnection

  state(): WebSocketState {
    return nativeStateToPublic(this.native.state())
  }

  sendText(text: string): Result<none, HttpError> {
    return mapWebSocketResult(this.native.sendText(text))
  }

  sendBinary(bytes: readonly byte[]): Result<none, HttpError> {
    return mapWebSocketResult(this.native.sendBinary(bytes))
  }

  ping(): Result<none, HttpError> {
    return mapWebSocketResult(this.native.ping())
  }

  close(code: int = WEBSOCKET_CLOSE_NORMAL, reason: string = ""): Result<none, HttpError> {
    return mapWebSocketResult(this.native.close(code, reason))
  }
}

// Events are delivered to `onEvent` on the actor that created it, in order,
// starting with `WebSocketOpen`.
export function connectWebSocket(
  url: string,
  onEvent: (event: WebSocketEvent): none,
  options: WebSocketOptions = WebSocketOptions {},
): Result<WebSocketConnection, HttpError> {
  if options.outboundCapacity <= 0 || options.eventCapacity <= 0 {
    panic("WebSocket capacities must be positive")
  }

  transportUrl := prepareTransportUrl(url) else error {
    return Failure { error: error }
  }

  native := NativeHttpWebSocketConnection.connect(
    transportUrl,
    renderHeaders(options.headers),
    options.timeoutMs,
    options.outboundCapacity,
    options.eventCapacity,
  ) else error {
    return Failure { error: parseWebSocketError(error) }
  }

  connection := WebSocketConnection { url, options, native }
  native.attachHandler(
    connection,
    (event: WebSocketEvent): none => {
      onEvent(event)
      native.eventHandled()
    },
    options.eventCapacity,
  )
  native.start()

  return Success { value: connection }
}

function mapWebSocketResult(result: Result<none, string>): Result<none, HttpError> {
  return case result {
    _: Success -> Success {},
    f: Failure -> Failure { error: parseWebSocketError(f.error) },
  }
}

function nativeStateToPublic(state: int): WebSocketState {
  return case state {
    0 -> WebSocketState.Connecting,
    1 -> WebSocketState.Open,
    2 -> WebSocketState.Closing,
    3 -> WebSocketState.Closed,
    _ -> WebSocketState.Error,
  }
}

function parseWebSocketError(raw: string): HttpError {
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

  return HttpError {
    kind: raw.substring(0, firstSeparator),
    code: remainder.substring(0, secondSeparator),
    message: remainder.slice(secondSeparator + 1),
  }
}

function renderHeaders(headers: readonly HttpHeader[]): string {
  let text = ""
  for header of headers {
    text += "${header.name}: ${header.value}\r\n"
  }
  return text
}
