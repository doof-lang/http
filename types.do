export class HttpHeader {
  readonly name: string
  readonly value: string
}

export class HttpError {
  readonly kind: string
  readonly code: string
  readonly message: string
}
