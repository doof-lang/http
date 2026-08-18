import { BlobReader } from "std/blob"
import { Assert } from "std/assert"

import { prepareTransportUrl } from "./transport_url"

export function testPrepareTransportUrlEncodesOnlyHostname(): none {
  url := "HTTPS://usér:pass@例え.テスト:8443/a/b?next=例#場所"
  Assert.equal(
    try! prepareTransportUrl(url),
    "HTTPS://usér:pass@xn--r8jz45g.xn--zckzah:8443/a/b?next=例#場所",
  )
}

export function testPrepareTransportUrlHandlesAllSupportedSchemes(): none {
  Assert.equal(try! prepareTransportUrl("http://bücher.example"), "http://xn--bcher-kva.example")
  Assert.equal(try! prepareTransportUrl("https://bücher.example"), "https://xn--bcher-kva.example")
  Assert.equal(try! prepareTransportUrl("ws://bücher.example"), "ws://xn--bcher-kva.example")
  Assert.equal(try! prepareTransportUrl("wss://bücher.example"), "wss://xn--bcher-kva.example")
}

export function testPrepareTransportUrlPreservesAsciiAndIpHosts(): none {
  Assert.equal(try! prepareTransportUrl("https://Example.COM:443/path"), "https://Example.COM:443/path")
  Assert.equal(try! prepareTransportUrl("http://127.0.0.1:8080/"), "http://127.0.0.1:8080/")
  Assert.equal(try! prepareTransportUrl("http://[2001:db8::1]:8080/"), "http://[2001:db8::1]:8080/")
  Assert.equal(try! prepareTransportUrl("http://2001:db8::1/"), "http://2001:db8::1/")
}

export function testPrepareTransportUrlLeavesUnsupportedAndMalformedUrlsAlone(): none {
  Assert.equal(try! prepareTransportUrl("ftp://例え.テスト/file"), "ftp://例え.テスト/file")
  Assert.equal(try! prepareTransportUrl("not-a-url"), "not-a-url")
  Assert.equal(try! prepareTransportUrl("http:///path"), "http:///path")
  Assert.equal(try! prepareTransportUrl("http://例え:bad:port/path"), "http://例え:bad:port/path")
}

export function testPrepareTransportUrlMapsPunycodeFailuresToInvalidUrl(): none {
  malformedUtf8 := BlobReader([255]).readString(1L)
  result := prepareTransportUrl("https://${malformedUtf8}.example/path")

  case result {
    _: Success -> Assert.fail("expected malformed hostname to fail")
    f: Failure -> {
      Assert.equal(f.error.kind, "invalid-url")
      Assert.equal(f.error.code, "0")
      Assert.stringContains(f.error.message, "Invalid UTF-8")
    }
  }
}
