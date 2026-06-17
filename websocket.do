import { BlobBuilder } from "std/blob"
import { ChannelReceiver, ChannelSender, createChannel } from "std/event"

import { HttpError, HttpHeader } from "./types"

import class NativeHttpWebSocketConnection from "./native_http_client.hpp" {
  static connect(
    url: string,
    requestHeaders: string,
    timeoutMs: int,
    outboundCapacity: int,
    eventCapacity: int,
  ): Result<NativeHttpWebSocketConnection, string>

  sendText(text: string): Result<void, string>
  sendBinary(bytes: readonly byte[]): Result<void, string>
  ping(): Result<void, string>
  close(code: int, reason: string): Result<void, string>
  attachChannels(
    connection: WebSocketConnection,
    eventSender: ChannelSender<WebSocketEvent>,
    commandReceiver: ChannelReceiver<WebSocketCommand>,
  ): void
  start(): void
  resumeInboundReads(): void
  state(): int
}

export enum WebSocketState {
  Connecting,
  Open,
  Closing,
  Closed,
  Error,
}

export const WEBSOCKET_CLOSE_NORMAL = 1000
export const WEBSOCKET_CLOSE_GOING_AWAY = 1001
export const WEBSOCKET_CLOSE_PROTOCOL_ERROR = 1002
export const WEBSOCKET_CLOSE_UNSUPPORTED_DATA = 1003
export const WEBSOCKET_CLOSE_INVALID_PAYLOAD = 1007
export const WEBSOCKET_CLOSE_POLICY_VIOLATION = 1008
export const WEBSOCKET_CLOSE_MESSAGE_TOO_BIG = 1009
export const WEBSOCKET_CLOSE_INTERNAL_ERROR = 1011

export class WebSocketOptions {
  readonly eventCapacity: int = 1024
  readonly commandCapacity: int = 1024
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

export type WebSocketCommand =
  WebSocketSendText |
  WebSocketSendBinary |
  WebSocketPing |
  WebSocketCloseCommand

export class WebSocketOpen {
  readonly connection: WebSocketConnection
}

export class WebSocketText {
  readonly connection: WebSocketConnection
  readonly text: string
}

export class WebSocketBinary {
  readonly connection: WebSocketConnection
  readonly bytes: readonly byte[]
}

export class WebSocketWritable {
  readonly connection: WebSocketConnection
}

export class WebSocketClose {
  readonly connection: WebSocketConnection
  readonly code: int
  readonly reason: string
  readonly wasClean: bool
}

export class WebSocketError {
  readonly connection: WebSocketConnection
  readonly error: HttpError
}

export class WebSocketSendText {
  readonly text: string
  readonly coalesceKey: string | null = null
}

export class WebSocketSendBinary {
  readonly bytes: readonly byte[]
  readonly coalesceKey: string | null = null
}

export class WebSocketPing {
}

export class WebSocketCloseCommand {
  readonly code: int = 1000
  readonly reason: string = ""
}

export class WebSocketConnection {
  readonly url: string
  readonly events: ChannelReceiver<WebSocketEvent>
  readonly commands: ChannelSender<WebSocketCommand>
  readonly options: WebSocketOptions = WebSocketOptions {}
  private readonly eventSender: ChannelSender<WebSocketEvent>
  private readonly commandReceiver: ChannelReceiver<WebSocketCommand>
  private readonly native: NativeHttpWebSocketConnection

  state(): WebSocketState {
    return nativeStateToPublic(this.native.state())
  }

  close(): void {
    ignored := this.native.close(WEBSOCKET_CLOSE_NORMAL, "")
    this.commands.close()
    this.events.close()
  }
}

export function connectWebSocket(
  url: string,
  options: WebSocketOptions = WebSocketOptions {},
): Result<WebSocketConnection, HttpError> {
  if options.commandCapacity <= 0 || options.eventCapacity <= 0 {
    panic("WebSocket channel capacities must be positive")
  }

  (eventSender, events) := createChannel<WebSocketEvent>{
    capacity: options.eventCapacity,
    keepsAlive: true,
  }
  (commands, commandReceiver) := createChannel<WebSocketCommand>{
    capacity: options.commandCapacity,
    keepsAlive: true,
  }

  let connection: WebSocketConnection | null = null
  nativeResult := NativeHttpWebSocketConnection.connect(
    url,
    renderHeaders(options.headers),
    options.timeoutMs,
    1,
    options.eventCapacity,
  )

  let native: NativeHttpWebSocketConnection | null = null
  case nativeResult {
    s: Success -> {
      native = s.value
    }
    f: Failure -> {
      commands.close()
      events.close()
      return Failure { error: parseWebSocketError(f.error) }
    }
  }

  actualConnection := WebSocketConnection {
    url,
    events,
    commands,
    options,
    eventSender,
    commandReceiver,
    native: native!,
  }
  connection = actualConnection

  actualConnection.native.attachChannels(actualConnection, eventSender, commandReceiver)

  emitLocalWebSocketEvent(actualConnection, WebSocketOpen {
    connection: actualConnection,
  })
  actualConnection.native.start()

  return Success { value: actualConnection }
}

function emitLocalWebSocketEvent(
  connection: WebSocketConnection,
  event: WebSocketEvent,
): void {
  ignored := connection.eventSender.send(event)
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
