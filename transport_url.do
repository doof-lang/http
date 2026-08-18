import { hostnameToAscii } from "std/url"

import { HttpError } from "./types"

export function prepareTransportUrl(url: string): Result<string, HttpError> {
  schemeEnd := url.indexOf("://")
  if schemeEnd < 0 {
    return Success { value: url }
  }

  scheme := url.substring(0, schemeEnd).toLowerCase()
  if scheme != "http" && scheme != "https" && scheme != "ws" && scheme != "wss" {
    return Success { value: url }
  }

  authorityStart := schemeEnd + 3
  authorityEnd := findAuthorityEnd(url, authorityStart)
  lastAt := findLastByteInRange(url, int('@'), authorityStart, authorityEnd)
  hostStart := if lastAt >= 0 then lastAt + 1 else authorityStart
  if hostStart >= authorityEnd {
    return Success { value: url }
  }

  if url.charAt(hostStart) == '[' {
    return Success { value: url }
  }

  let hostEnd = authorityEnd
  let colon = -1
  let colonCount = 0
  let index = hostStart
  while index < authorityEnd {
    if url.charAt(index) == ':' {
      colon = index
      colonCount += 1
    }
    index += 1
  }
  if colonCount == 1 {
    hostEnd = colon
  } else if colonCount > 1 {
    return Success { value: url }
  }

  host := url.substring(hostStart, hostEnd)
  asciiHost := hostnameToAscii(host)
  case asciiHost {
    s: Success -> return Success {
      value: url.substring(0, hostStart) + s.value + url.slice(hostEnd)
    }
    f: Failure -> return Failure {
      error: HttpError {
        kind: "invalid-url",
        code: "0",
        message: "Invalid internationalized hostname at byte ${hostStart + f.error.index}: ${f.error.message}",
      }
    }
  }
}

function findAuthorityEnd(url: string, start: int): int {
  let index = start
  while index < url.length {
    value := url.charAt(index)
    if value == '/' || value == '?' || value == '#' {
      return index
    }
    index += 1
  }
  return url.length
}

function findLastByteInRange(text: string, target: int, start: int, end: int): int {
  let found = -1
  let index = start
  while index < end {
    if int(text.charAt(index)) == target {
      found = index
    }
    index += 1
  }
  return found
}
