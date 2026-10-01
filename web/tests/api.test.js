import { afterEach, describe, expect, it, vi } from "vitest";
import { ApiError, api, errorMap, fieldError, fmtBytes, fmtCount, fmtNs, getPath, legHealth, setPath } from "../src/api.js";

function response(status, body, headers = {}) {
  const h = new Map(Object.entries({ "content-type": typeof body === "string" ? "text/plain" : "application/json", ...headers }));
  return {
    ok: status >= 200 && status < 300,
    status,
    headers: { get: (k) => h.get(k.toLowerCase()) ?? null },
    json: async () => body,
    text: async () => body,
  };
}

afterEach(() => vi.unstubAllGlobals());

describe("request", () => {
  it("returns body and ETag and sends If-Match on PUT", async () => {
    const fetch = vi.fn(async () => response(200, { ok: true }, { etag: '"abc"' }));
    vi.stubGlobal("fetch", fetch);
    const r = await api.put("/api/config", { a: 1 }, '"abc"');
    expect(r.body).toEqual({ ok: true });
    expect(r.etag).toBe('"abc"');
    const [, opts] = fetch.mock.calls[0];
    expect(opts.method).toBe("PUT");
    expect(opts.headers["If-Match"]).toBe('"abc"');
    expect(opts.headers["Content-Type"]).toBe("application/json");
    expect(JSON.parse(opts.body)).toEqual({ a: 1 });
  });

  it("raises ApiError with validation details", async () => {
    const details = [{ pointer: "/groups/0/video/0/width", message: "must be 1920" }];
    vi.stubGlobal("fetch", async () => response(400, { error: "invalid configuration", details }));
    const err = await api.post("/api/config/validate", {}).catch((e) => e);
    expect(err).toBeInstanceOf(ApiError);
    expect(err.status).toBe(400);
    expect(err.message).toBe("invalid configuration");
    expect(err.details).toEqual(details);
  });

  it("wraps non-JSON error bodies", async () => {
    vi.stubGlobal("fetch", async () => response(502, "Bad Gateway"));
    const err = await api.get("/api/status").catch((e) => e);
    expect(err.status).toBe(502);
    expect(err.message).toBe("Bad Gateway");
    expect(err.details).toEqual([]);
  });
});

describe("validation error mapping", () => {
  const map = errorMap({ details: [{ pointer: "/node/label", message: "too long" }, { pointer: "/groups/1/audio/0/channels", message: "1..64" }] });

  it("maps pointers to messages", () => {
    expect(map["/node/label"]).toBe("too long");
  });

  it("finds exact and child pointers", () => {
    expect(fieldError(map, "/node/label")).toBe("too long");
    expect(fieldError(map, "/groups/1")).toBe("audio/0/channels: 1..64");
    expect(fieldError(map, "/groups/10")).toBe("");
    expect(fieldError(map, "/nic")).toBe("");
  });

  it("tolerates errors without details", () => {
    expect(errorMap(new Error("x"))).toEqual({});
    expect(errorMap(undefined)).toEqual({});
  });
});

describe("JSON pointer helpers", () => {
  it("reads nested members and escaped keys", () => {
    const obj = { a: { "b/c": [1, { "d~e": 2 }] } };
    expect(getPath(obj, "/a/b~1c/1/d~0e")).toBe(2);
    expect(getPath(obj, "/a/missing/x")).toBeUndefined();
    expect(getPath(obj, "")).toBe(obj);
  });

  it("creates intermediate objects and arrays", () => {
    const obj = {};
    setPath(obj, "/groups/0/label", "cam 1");
    setPath(obj, "/nic/ptp/domain", 127);
    expect(obj).toEqual({ groups: [{ label: "cam 1" }], nic: { ptp: { domain: 127 } } });
  });

  it("removes members for empty values", () => {
    const obj = { node: { label: "x", description: "y" }, list: [1, 2, 3] };
    setPath(obj, "/node/label", "");
    setPath(obj, "/node/description", undefined);
    setPath(obj, "/list/1", undefined);
    expect(obj).toEqual({ node: {}, list: [1, 3] });
  });

  it("writes escaped keys", () => {
    const obj = {};
    setPath(obj, "/a~1b", 1);
    expect(obj).toEqual({ "a/b": 1 });
  });
});

describe("formatting", () => {
  it("formats nanoseconds", () => {
    expect(fmtNs(null)).toBe("–");
    expect(fmtNs(250)).toBe("250 ns");
    expect(fmtNs(-1500)).toBe("-1.5 µs");
    expect(fmtNs(2_000_000)).toBe("2.00 ms");
    expect(fmtNs(1_500_000_000)).toBe("1.500 s");
  });

  it("formats counts and bytes", () => {
    expect(fmtCount(undefined)).toBe("–");
    expect(fmtCount(9999)).toBe("9999");
    expect(fmtCount(12345)).toBe("12.3 k");
    expect(fmtCount(3_500_000)).toBe("3.50 M");
    expect(fmtBytes(0)).toBe("0 B");
    expect(fmtBytes(512)).toBe("512 B");
    expect(fmtBytes(2 * 1024 * 1024 * 1024)).toBe("2.0 GiB");
  });

  it("classifies leg health", () => {
    expect(legHealth(0, 0)).toBe("off");
    expect(legHealth(1000, 0)).toBe("ok");
    expect(legHealth(100000, 5)).toBe("warn");
    expect(legHealth(1000, 5)).toBe("bad");
  });
});
