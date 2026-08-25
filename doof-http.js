const encoder = new TextEncoder();
const decoder = new TextDecoder();

const STATUS_TEXT = 0;
const HEADERS = 1;
const BODY = 2;
const ERROR = 3;

function asError(value) {
  return value instanceof Error ? value : new Error(String(value));
}

function createHttpBridge(options = {}) {
  const wasm = options.WebAssembly ?? globalThis.WebAssembly;
  if (typeof wasm?.Suspending !== "function" || typeof wasm?.promising !== "function") {
    throw new Error("The std/http WebAssembly backend requires JavaScript Promise Integration (JSPI)");
  }

  const fetchImplementation = options.fetch ?? globalThis.fetch?.bind(globalThis);
  const wrapSuspending = options.wrapSuspending ?? ((callable) => new wasm.Suspending(callable));
  const maxResponseBytes = options.maxResponseBytes ?? Number.POSITIVE_INFINITY;
  if (typeof maxResponseBytes !== "number" || Number.isNaN(maxResponseBytes) || maxResponseBytes < 0) {
    throw new TypeError("maxResponseBytes must be a non-negative number");
  }
  let instance;
  let nextResponseId = 1;
  const responses = new Map();

  const memoryBytes = () => {
    if (!instance?.exports?.memory) {
      throw new Error("The std/http WebAssembly bridge has not been attached to an instance");
    }
    return new Uint8Array(instance.exports.memory.buffer);
  };
  const readString = (pointer) => {
    const bytes = memoryBytes();
    if (pointer <= 0 || pointer >= bytes.length) throw new Error("Invalid WebAssembly string pointer");
    let end = pointer;
    while (end < bytes.length && bytes[end] !== 0) end += 1;
    if (end === bytes.length) throw new Error("Unterminated WebAssembly string");
    return decoder.decode(bytes.subarray(pointer, end));
  };
  const readBytes = (pointer, length) => {
    if (length < 0) throw new Error("Invalid WebAssembly byte length");
    if (length === 0) return new Uint8Array();
    const bytes = memoryBytes();
    if (pointer <= 0 || pointer + length > bytes.length) {
      throw new Error("Invalid WebAssembly byte range");
    }
    return bytes.slice(pointer, pointer + length);
  };
  const parseHeaders = (value) => {
    const headers = new Headers();
    for (const line of value.split("\r\n")) {
      const separator = line.indexOf(":");
      if (separator <= 0) continue;
      headers.append(line.slice(0, separator).trim(), line.slice(separator + 1).trim());
    }
    return headers;
  };
  const renderHeaders = (headers) => {
    let value = "";
    headers.forEach((headerValue, name) => {
      value += `${name}: ${headerValue}\r\n`;
    });
    return value;
  };
  const encodeRecord = ({ status = 0, statusText = "", headers = "", body, error = "" }) => ({
    status,
    fields: [
      encoder.encode(statusText),
      encoder.encode(headers),
      body ?? new Uint8Array(),
      encoder.encode(error),
    ],
  });
  const saveResponse = (record) => {
    const responseId = nextResponseId++;
    responses.set(responseId, encodeRecord(record));
    return responseId;
  };
  const classifyError = (error, timedOut) => {
    if (timedOut) return `timeout|0|${error.message || "The request timed out"}`;
    return `transport|0|${error.message || String(error)}`;
  };
  const responseField = (responseId, field) => {
    const response = responses.get(responseId);
    if (!response || field < STATUS_TEXT || field > ERROR) return undefined;
    return response.fields[field];
  };
  const readResponseBody = async (response) => {
    if (!Number.isFinite(maxResponseBytes)) {
      return new Uint8Array(await response.arrayBuffer());
    }

    const contentLength = Number(response.headers.get("content-length"));
    if (Number.isFinite(contentLength) && contentLength > maxResponseBytes) {
      throw new Error(`The response body exceeded the ${maxResponseBytes} byte limit`);
    }

    const reader = response.body?.getReader?.();
    if (!reader) {
      const body = new Uint8Array(await response.arrayBuffer());
      if (body.length > maxResponseBytes) {
        throw new Error(`The response body exceeded the ${maxResponseBytes} byte limit`);
      }
      return body;
    }

    const chunks = [];
    let length = 0;
    try {
      while (true) {
        const { done, value } = await reader.read();
        if (done) break;
        if (length + value.length > maxResponseBytes) {
          await reader.cancel();
          throw new Error(`The response body exceeded the ${maxResponseBytes} byte limit`);
        }
        chunks.push(value);
        length += value.length;
      }
    } finally {
      reader.releaseLock();
    }

    const body = new Uint8Array(length);
    let offset = 0;
    for (const chunk of chunks) {
      body.set(chunk, offset);
      offset += chunk.length;
    }
    return body;
  };

  async function performRequest(
    methodPointer,
    urlPointer,
    headersPointer,
    bodyPointer,
    bodySize,
    hasBody,
    timeoutMs,
    followRedirects,
  ) {
    let timedOut = false;
    let timer;
    try {
      if (!fetchImplementation) throw new Error("This host does not provide fetch()");

      const request = {
        method: readString(methodPointer),
        headers: parseHeaders(readString(headersPointer)),
        redirect: followRedirects !== 0 ? "follow" : "manual",
      };
      if (hasBody !== 0) request.body = readBytes(bodyPointer, bodySize);

      const controller = new AbortController();
      request.signal = controller.signal;
      if (timeoutMs > 0) {
        timer = setTimeout(() => {
          timedOut = true;
          controller.abort();
        }, timeoutMs);
      }

      const response = await fetchImplementation(readString(urlPointer), request);
      const body = await readResponseBody(response);
      return saveResponse({
        status: response.status,
        statusText: response.statusText,
        headers: renderHeaders(response.headers),
        body,
      });
    } catch (error) {
      return saveResponse({ error: classifyError(asError(error), timedOut) });
    } finally {
      if (timer !== undefined) clearTimeout(timer);
    }
  }

  const imports = {
    perform_request: wrapSuspending(performRequest),
    response_status(responseId) {
      return responses.get(responseId)?.status ?? -1;
    },
    response_size(responseId, field) {
      return responseField(responseId, field)?.length ?? -1;
    },
    response_copy(responseId, field, destination, capacity) {
      const value = responseField(responseId, field);
      if (!value || capacity < value.length || (value.length > 0 && destination <= 0)) return -1;
      if (value.length > 0) memoryBytes().set(value, destination);
      return value.length;
    },
    release_response(responseId) {
      responses.delete(responseId);
    },
  };

  return {
    imports,
    attach(value) { instance = value; },
  };
}

export { createHttpBridge };
