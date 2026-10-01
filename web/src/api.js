// REST client for the gateway API (SPECIFICATION.md §11.3).

export class ApiError extends Error {
  constructor(status, body) {
    super(body?.error || `HTTP ${status}`);
    this.status = status;
    this.body = body;
    /** Per-field validation errors: [{pointer, message}] */
    this.details = Array.isArray(body?.details) ? body.details : [];
  }
}

async function request(path, options = {}) {
  const headers = { ...(options.headers || {}) };
  if (options.body != null && !headers["Content-Type"]) headers["Content-Type"] = "application/json";
  const resp = await fetch(path, { ...options, headers });
  const ct = resp.headers.get("content-type") || "";
  const body = ct.includes("json") ? await resp.json() : await resp.text();
  if (!resp.ok) throw new ApiError(resp.status, typeof body === "object" ? body : { error: String(body).slice(0, 300) });
  return { body, etag: resp.headers.get("etag"), headers: resp.headers };
}

export const api = {
  async get(path) {
    return (await request(path)).body;
  },
  getWithEtag: (path) => request(path),
  put: (path, body, etag) =>
    request(path, { method: "PUT", body: JSON.stringify(body), headers: etag ? { "If-Match": etag } : {} }),
  post: (path, body) => request(path, { method: "POST", body: typeof body === "string" ? body : JSON.stringify(body ?? {}) }),
  del: (path) => request(path, { method: "DELETE" }),
};

/** Maps validation errors to a {pointer: message} lookup. */
export function errorMap(error) {
  const map = {};
  for (const d of error?.details || []) map[d.pointer] = d.message;
  return map;
}

/** Error message for a field, matching the pointer itself or any child pointer. */
export function fieldError(map, pointer) {
  if (map[pointer]) return map[pointer];
  const prefix = pointer + "/";
  const hit = Object.keys(map).find((k) => k.startsWith(prefix));
  return hit ? `${hit.slice(pointer.length + 1)}: ${map[hit]}` : "";
}

export const STATE_KIND = {
  running: "ok",
  degraded: "warn",
  waiting_for_flow: "warn",
  no_signal: "warn",
  idle: "neutral",
  error: "bad",
};

export function fmtNs(ns) {
  if (ns === null || ns === undefined) return "–";
  const a = Math.abs(ns);
  if (a >= 1e9) return `${(ns / 1e9).toFixed(3)} s`;
  if (a >= 1e6) return `${(ns / 1e6).toFixed(2)} ms`;
  if (a >= 1e3) return `${(ns / 1e3).toFixed(1)} µs`;
  return `${ns} ns`;
}

export function fmtCount(n) {
  if (n === null || n === undefined) return "–";
  if (n >= 1e9) return `${(n / 1e9).toFixed(2)} G`;
  if (n >= 1e6) return `${(n / 1e6).toFixed(2)} M`;
  if (n >= 1e4) return `${(n / 1e3).toFixed(1)} k`;
  return String(n);
}

export function fmtBytes(n) {
  if (!n) return "0 B";
  const u = ["B", "KiB", "MiB", "GiB", "TiB"];
  let i = 0;
  while (n >= 1024 && i < u.length - 1) {
    n /= 1024;
    ++i;
  }
  return `${n.toFixed(i ? 1 : 0)} ${u[i]}`;
}

/** Leg health (§11.2): green/amber/red from received vs total packets. */
export function legHealth(packets, lost) {
  if (!packets) return "off";
  if (!lost) return "ok";
  return lost / packets < 0.001 ? "warn" : "bad";
}

/** JSON pointer helpers for the configuration file. */
function pointerParts(pointer) {
  return pointer
    .split("/")
    .slice(1)
    .map((k) => k.replace(/~1/g, "/").replace(/~0/g, "~"));
}

export function getPath(obj, pointer) {
  return pointerParts(pointer).reduce((o, k) => (o == null ? undefined : o[k]), obj);
}

/** Sets a value; undefined or "" removes the member (array elements are spliced out). */
export function setPath(obj, pointer, value) {
  const parts = pointerParts(pointer);
  let o = obj;
  for (let i = 0; i < parts.length - 1; ++i) {
    if (o[parts[i]] == null) o[parts[i]] = /^\d+$/.test(parts[i + 1]) ? [] : {};
    o = o[parts[i]];
  }
  const last = parts.at(-1);
  if (value !== undefined && value !== "") o[last] = value;
  else if (Array.isArray(o)) o.splice(Number(last), 1);
  else delete o[last];
}

export function clone(v) {
  return v === undefined ? undefined : JSON.parse(JSON.stringify(v));
}
