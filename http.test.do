import { Assert } from "std/assert"

import {
  Cookie,
  SetCookie,
  WebSocketOptions,
  cookieValue,
  connectWebSocket,
  parseCookieHeader,
  parseSetCookieHeader,
  renderCookieHeader,
  renderSetCookieHeader,
} from "./index"

export function testParseCookieHeaderKeepsOrderAndDuplicates(): void {
  cookies := parseCookieHeader("session=abc; theme=dark; session=override")

  Assert.equal(cookies.length, 3)
  Assert.equal(cookies[0].name, "session")
  Assert.equal(cookies[0].value, "abc")
  Assert.equal(cookies[1].name, "theme")
  Assert.equal(cookies[1].value, "dark")
  Assert.equal(cookies[2].name, "session")
  Assert.equal(cookies[2].value, "override")
}

export function testParseCookieHeaderIsLenient(): void {
  cookies := parseCookieHeader(" ; missing ; =empty-name ; a=1 ; b = two = three ; c= ")

  Assert.equal(cookies.length, 3)
  Assert.equal(cookies[0].name, "a")
  Assert.equal(cookies[0].value, "1")
  Assert.equal(cookies[1].name, "b")
  Assert.equal(cookies[1].value, "two = three")
  Assert.equal(cookies[2].name, "c")
  Assert.equal(cookies[2].value, "")
}

export function testRenderCookieHeaderSkipsEmptyNames(): void {
  rendered := renderCookieHeader(readonly [
    Cookie { name: "session", value: "abc" },
    Cookie { name: "", value: "ignored" },
    Cookie { name: "theme", value: "dark" },
  ])

  Assert.equal(rendered, "session=abc; theme=dark")
}

export function testCookieValueReturnsFirstMatch(): void {
  cookies := readonly [
    Cookie { name: "session", value: "first" },
    Cookie { name: "theme", value: "dark" },
    Cookie { name: "session", value: "second" },
  ]

  Assert.equal(cookieValue(cookies, "session"), "first")
  Assert.equal(cookieValue(cookies, "missing"), null)
}

export function testParseSetCookieHeaderWithCommonAttributes(): void {
  cookie := parseSetCookieHeader(
    "sid=abc; Expires=Wed, 21 Oct 2026 07:28:00 GMT; Max-Age=3600; Domain=example.com; Path=/; SameSite=Lax; Secure; HttpOnly",
  )!

  Assert.equal(cookie.name, "sid")
  Assert.equal(cookie.value, "abc")
  Assert.equal(cookie.expires, "Wed, 21 Oct 2026 07:28:00 GMT")
  Assert.equal(cookie.maxAge, "3600")
  Assert.equal(cookie.domain, "example.com")
  Assert.equal(cookie.path, "/")
  Assert.equal(cookie.sameSite, "Lax")
  Assert.isTrue(cookie.secure)
  Assert.isTrue(cookie.httpOnly)
}

export function testParseSetCookieHeaderAttributesAreCaseInsensitive(): void {
  cookie := parseSetCookieHeader(
    "sid=abc; expires=soon; max-age=10; DOMAIN=example.com; path=/app; samesite=Strict; secure; HTTPONLY",
  )!

  Assert.equal(cookie.expires, "soon")
  Assert.equal(cookie.maxAge, "10")
  Assert.equal(cookie.domain, "example.com")
  Assert.equal(cookie.path, "/app")
  Assert.equal(cookie.sameSite, "Strict")
  Assert.isTrue(cookie.secure)
  Assert.isTrue(cookie.httpOnly)
}

export function testParseSetCookieHeaderIgnoresUnknownAttributes(): void {
  cookie := parseSetCookieHeader("sid=abc; Priority=High; Partitioned; Path=/")!

  Assert.equal(cookie.name, "sid")
  Assert.equal(cookie.value, "abc")
  Assert.equal(cookie.path, "/")
  Assert.isFalse(cookie.secure)
  Assert.isFalse(cookie.httpOnly)
}

export function testParseSetCookieHeaderRejectsInvalidRequiredPair(): void {
  Assert.equal(parseSetCookieHeader(""), null)
  Assert.equal(parseSetCookieHeader("Secure; Path=/"), null)
  Assert.equal(parseSetCookieHeader("=abc; Path=/"), null)
}

export function testRenderSetCookieHeaderUsesStableAttributeOrder(): void {
  rendered := renderSetCookieHeader(SetCookie {
    name: "sid",
    value: "abc",
    domain: "example.com",
    path: "/",
    expires: "Wed, 21 Oct 2026 07:28:00 GMT",
    maxAge: "3600",
    secure: true,
    httpOnly: true,
    sameSite: "Lax",
  })

  Assert.equal(
    rendered,
    "sid=abc; Expires=Wed, 21 Oct 2026 07:28:00 GMT; Max-Age=3600; Domain=example.com; Path=/; SameSite=Lax; Secure; HttpOnly",
  )
}

export function testConnectWebSocketReportsInvalidUrlAsHttpError(): void {
  result := connectWebSocket("not-a-websocket-url", WebSocketOptions {
    timeoutMs: 100,
  })

  case result {
    _: Success -> Assert.fail("expected websocket connection to fail")
    f: Failure -> {
      Assert.notEqual(f.error.kind, "")
      Assert.notEqual(f.error.message, "")
    }
  }
}
