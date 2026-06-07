import { BlobBuilder } from "std/blob"
import { Backpressure, ChannelReceiver, ChannelSender, SendError, createChannel } from "std/event"

import { HttpError, HttpHeader } from "./types"

import class NativeHttpWebSocketEvent from "./native_http_client.hpp" {
  kind(): int
  text(): string
  bytes(): readonly byte[]
  code(): int
  reason(): string
  wasClean(): bool
  error(): string
}

import class NativeHttpWebSocketConnection from "./native_http_client.hpp" {
  static connect(
    url: string,
    requestHeaders: string,
    timeoutMs: int,
    outboundCapacity: int,
    callback: (event: NativeHttpWebSocketEvent): int,
  ): Result<NativeHttpWebSocketConnection, string>

  sendText(text: string): Result<void, string>
  sendBinary(bytes: readonly byte[]): Result<void, string>
  ping(): Result<void, string>
  close(code: int, reason: string): Result<void, string>
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
}

export class WebSocketSendBinary {
  readonly bytes: readonly byte[]
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
    options.commandCapacity,
    (event: NativeHttpWebSocketEvent): int => emitNativeWebSocketEvent(connection!, event),
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

  commandReceiver.onMessage((command: WebSocketCommand): void => handleWebSocketCommand(actualConnection, command))
  commandReceiver.onClosed((): void => {
    ignored := actualConnection.native.close(WEBSOCKET_CLOSE_NORMAL, "")
  })
  eventSender.onReady((): void => actualConnection.native.resumeInboundReads())
  eventSender.onClosed((): void => {
    ignored := actualConnection.native.close(WEBSOCKET_CLOSE_NORMAL, "")
  })

  emitLocalWebSocketEvent(actualConnection, WebSocketOpen {
    connection: actualConnection,
  })
  actualConnection.native.start()

  return Success { value: actualConnection }
}

function handleWebSocketCommand(
  connection: WebSocketConnection,
  command: WebSocketCommand,
): void {
  textCommand := command as WebSocketSendText
  case textCommand {
    s: Success -> {
      reportCommandResult(connection, connection.native.sendText(s.value.text))
      return
    }
    _: Failure -> {}
  }

  binaryCommand := command as WebSocketSendBinary
  case binaryCommand {
    s: Success -> {
      reportCommandResult(connection, connection.native.sendBinary(s.value.bytes))
      return
    }
    _: Failure -> {}
  }

  pingCommand := command as WebSocketPing
  case pingCommand {
    _: Success -> {
      reportCommandResult(connection, connection.native.ping())
      return
    }
    _: Failure -> {}
  }

  closeCommand := command as WebSocketCloseCommand
  case closeCommand {
    s: Success -> {
      reportCommandResult(connection, connection.native.close(s.value.code, s.value.reason))
      return
    }
    _: Failure -> {}
  }
}

function reportCommandResult(
  connection: WebSocketConnection,
  result: Result<void, string>,
): void {
  case result {
    _: Success -> {}
    f: Failure -> {
      emitLocalWebSocketEvent(connection, WebSocketError {
        connection,
        error: parseWebSocketError(f.error),
      })
    }
  }
}

function emitLocalWebSocketEvent(
  connection: WebSocketConnection,
  event: WebSocketEvent,
): void {
  ignored := connection.eventSender.send(event)
}

function emitNativeWebSocketEvent(
  connection: WebSocketConnection,
  event: NativeHttpWebSocketEvent,
): int {
  publicEvent := nativeWebSocketEventToPublic(connection, event)
  sent := connection.eventSender.send(publicEvent)
  code := channelSendResultToNativeCode(sent)

  if event.kind() == 4 || event.kind() == 5 {
    connection.commands.close()
    connection.events.close()
  }

  return code
}

function channelSendResultToNativeCode(
  sent: Result<Backpressure, SendError>,
): int {
  return case sent {
    s: Success -> case s.value {
      Backpressure.None -> 0,
      Backpressure.High -> 1,
    },
    f: Failure -> case f.error {
      SendError.Full -> 2,
      SendError.Closed -> 3,
    },
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

function nativeWebSocketEventToPublic(
  connection: WebSocketConnection,
  event: NativeHttpWebSocketEvent,
): WebSocketEvent {
  return case event.kind() {
    1 -> WebSocketText {
      connection,
      text: event.text(),
    },
    2 -> WebSocketBinary {
      connection,
      bytes: event.bytes(),
    },
    3 -> WebSocketWritable {
      connection,
    },
    4 -> WebSocketClose {
      connection,
      code: event.code(),
      reason: event.reason(),
      wasClean: event.wasClean(),
    },
    5 -> WebSocketError {
      connection,
      error: parseWebSocketError(event.error()),
    },
    _ -> WebSocketOpen {
      connection,
    },
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
