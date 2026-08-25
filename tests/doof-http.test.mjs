import test from "node:test";
import assert from "node:assert/strict";

import { createHttpBridge } from "../doof-http.js";

const encoder = new TextEncoder();
const decoder = new TextDecoder();

function harness(fetchImplementation, options = {}) {
  const memory = new WebAssembly.Memory({ initial: 2 });
  let nextPointer = 1024;
  const bridge = createHttpBridge({
    fetch: fetchImplementation,
    wrapSuspending: (callable) => callable,
    ...options,
  });
  bridge.attach({ exports: { memory } });

  const allocate = (length) => {
    const pointer = nextPointer;
    nextPointer += Math.max(length, 1);
    return pointer;
  };
  const writeString = (value) => {
    const bytes = encoder.encode(value);
    const pointer = allocate(bytes.length + 1);
    const memoryBytes = new Uint8Array(memory.buffer);
    memoryBytes.set(bytes, pointer);
    memoryBytes[pointer + bytes.length] = 0;
    return pointer;
  };
  const writeBytes = (value) => {
    const pointer = allocate(value.length);
    new Uint8Array(memory.buffer).set(value, pointer);
    return pointer;
  };
  const readField = (responseId, field) => {
    const size = bridge.imports.response_size(responseId, field);
    assert.ok(size >= 0);
    if (size === 0) return new Uint8Array();
    const pointer = allocate(size);
    assert.equal(bridge.imports.response_copy(responseId, field, pointer, size), size);
    return new Uint8Array(memory.buffer).slice(pointer, pointer + size);
  };

  return {
    imports: bridge.imports,
    writeString,
    writeBytes,
    readField,
    readFieldText(responseId, field) {
      return decoder.decode(readField(responseId, field));
    },
  };
}

test("marshals fetch requests and exposes a buffered response record", async () => {
  let received;
  const host = harness(async (url, request) => {
    received = { url, request };
    return new Response(new Uint8Array([4, 5, 6]), {
      status: 201,
      statusText: "Created",
      headers: { "X-Reply": "yes" },
    });
  });
  const body = new Uint8Array([1, 2, 3]);

  const responseId = await host.imports.perform_request(
    host.writeString("POST"),
    host.writeString("https://example.com/items"),
    host.writeString("Content-Type: application/octet-stream\r\nX-Test: value\r\n"),
    host.writeBytes(body),
    body.length,
    1,
    1000,
    0,
  );

  assert.equal(received.url, "https://example.com/items");
  assert.equal(received.request.method, "POST");
  assert.equal(received.request.redirect, "manual");
  assert.equal(received.request.headers.get("x-test"), "value");
  assert.deepEqual(Array.from(received.request.body), [1, 2, 3]);
  assert.equal(host.imports.response_status(responseId), 201);
  assert.equal(host.readFieldText(responseId, 0), "Created");
  assert.match(host.readFieldText(responseId, 1), /x-reply: yes/i);
  assert.deepEqual(Array.from(host.readField(responseId, 2)), [4, 5, 6]);
  assert.equal(host.readFieldText(responseId, 3), "");

  host.imports.release_response(responseId);
  assert.equal(host.imports.response_status(responseId), -1);
  assert.equal(host.imports.response_size(responseId, 2), -1);
});

test("timeout aborts fetch and resolves to an HttpError record", async () => {
  const host = harness((_url, request) => new Promise((_resolve, reject) => {
    request.signal.addEventListener("abort", () => {
      reject(new DOMException("Aborted", "AbortError"));
    });
  }));

  const responseId = await host.imports.perform_request(
    host.writeString("GET"),
    host.writeString("https://example.com"),
    host.writeString(""),
    0,
    0,
    0,
    1,
    1,
  );

  assert.equal(host.imports.response_status(responseId), 0);
  assert.match(host.readFieldText(responseId, 3), /^timeout\|0\|/);
});

test("invalid browser request options resolve to a transport error record", async () => {
  let fetchCalls = 0;
  const host = harness(async () => {
    fetchCalls += 1;
    return new Response();
  });

  const responseId = await host.imports.perform_request(
    host.writeString("GET"),
    host.writeString("https://example.com"),
    host.writeString("Bad Header: value\r\n"),
    0,
    0,
    0,
    1000,
    1,
  );

  assert.equal(fetchCalls, 0);
  assert.match(host.readFieldText(responseId, 3), /^transport\|0\|/);
});

test("optional response limit rejects oversized buffered bodies", async () => {
  const host = harness(
    async () => new Response(new Uint8Array([1, 2, 3])),
    { maxResponseBytes: 2 },
  );

  const responseId = await host.imports.perform_request(
    host.writeString("GET"),
    host.writeString("https://example.com"),
    host.writeString(""),
    0,
    0,
    0,
    1000,
    1,
  );

  assert.match(host.readFieldText(responseId, 3), /exceeded the 2 byte limit/);
});

test("requires JSPI support when constructing the bridge", () => {
  assert.throws(
    () => createHttpBridge({ WebAssembly: {} }),
    /requires JavaScript Promise Integration/,
  );
});

test("uses a real WebAssembly.Suspending import by default", () => {
  const bridge = createHttpBridge({ fetch: async () => new Response() });
  assert.equal(
    Object.prototype.toString.call(bridge.imports.perform_request),
    "[object WebAssembly.Suspending]",
  );
});
