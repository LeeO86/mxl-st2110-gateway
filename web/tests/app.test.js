// @vitest-environment happy-dom
import { afterEach, describe, expect, it, vi } from "vitest";
import { flushPromises, mount } from "@vue/test-utils";
import App from "../src/App.vue";
import { useConfig } from "../src/useConfig.js";
import statusFixture from "./fixtures/status.json";
import configFixture from "./fixtures/config.json";

function jsonResponse(status, body, headers = {}) {
  const h = new Map(Object.entries({ "content-type": "application/json", ...headers }));
  return { ok: status >= 200 && status < 300, status, headers: { get: (k) => h.get(k.toLowerCase()) ?? null }, json: async () => body, text: async () => JSON.stringify(body) };
}

function stubStatus(patch = {}) {
  const status = { ...structuredClone(statusFixture), ...patch };
  const fetch = vi.fn(async (path) =>
    path.startsWith("/api/config") ? jsonResponse(200, configFixture, { etag: configFixture.etag }) : jsonResponse(200, status),
  );
  vi.stubGlobal("fetch", fetch);
  return fetch;
}

let wrapper = null;
async function mountApp() {
  wrapper = mount(App, { attachTo: document.body });
  await flushPromises();
  return wrapper;
}

afterEach(() => {
  wrapper?.unmount();
  wrapper = null;
  vi.unstubAllGlobals();
  location.hash = "";
});

describe("App shell", () => {
  it("renders the dashboard with the groups of /api/status", async () => {
    stubStatus();
    const w = await mountApp();
    expect(w.findAll("nav button").map((b) => b.text())).toEqual(["Dashboard", "Groups", "NMOS", "Network", "PTP", "MXL", "Configuration"]);
    expect(w.text()).toContain("CAM 1");
    expect(w.text()).toContain("PGM");
    expect(w.text()).not.toMatch(/undefined|NaN|\[object Object\]/);
  });

  it("shows the changed-on-disk banner instead of restart-required", async () => {
    stubStatus({ config_changed_on_disk: true, restart_required: true, restart_reasons: ["ptp"] });
    const w = await mountApp();
    expect(w.text()).toContain("Configuration changed on disk");
    expect(w.text()).not.toContain("Restart required (ptp)");
  });

  it("shows restart-required with its reasons", async () => {
    stubStatus({ config_changed_on_disk: false, restart_required: true, restart_reasons: ["nic", "ptp"] });
    const w = await mountApp();
    expect(w.text()).toContain("Restart required (nic, ptp)");
  });

  it("shows setup mode and the test-backend warning", async () => {
    stubStatus({ setup_mode: true });
    const w = await mountApp();
    expect(w.text()).toContain("Setup mode");
    expect(w.text()).toContain("Test backend (mock)");
  });

  it("reports an unreachable status endpoint", async () => {
    vi.stubGlobal("fetch", vi.fn(async () => jsonResponse(503, { error: "unavailable" })));
    const w = await mountApp();
    expect(w.text()).toContain("status unavailable: unavailable");
  });

  it("switches tabs by hash", async () => {
    stubStatus();
    const w = await mountApp();
    await w.findAll("nav button")[1].trigger("click");
    expect(location.hash).toBe("#groups");
    expect(w.find("nav button.active").text()).toBe("Groups");
  });
});

describe("useConfig", () => {
  function configFetch(handlers) {
    const calls = [];
    vi.stubGlobal(
      "fetch",
      vi.fn(async (path, opts = {}) => {
        calls.push({ path, opts });
        const h = handlers[`${opts.method || "GET"} ${path}`];
        return typeof h === "function" ? h(opts) : h;
      }),
    );
    return calls;
  }

  it("loads the file with its ETag and saves with If-Match", async () => {
    const calls = configFetch({
      "GET /api/config": jsonResponse(200, configFixture, { etag: configFixture.etag }),
      "PUT /api/config": jsonResponse(200, { restart_required: false, groups: {}, etag: '"next"' }, { etag: '"next"' }),
    });
    const c = useConfig();
    await c.load();
    expect(c.etag.value).toBe(configFixture.etag);
    expect(c.draft.value.node.label).toBe("GW-UI-TEST");
    c.draft.value.groups[0].label = "CAM 1 renamed";
    const result = await c.save();
    expect(result.restart_required).toBe(false);
    const put = calls.find((x) => x.opts.method === "PUT");
    expect(put.opts.headers["If-Match"]).toBe(configFixture.etag);
    expect(JSON.parse(put.opts.body).groups[0].label).toBe("CAM 1 renamed");
    expect(c.message.value).toBe("Saved and applied.");
  });

  it("reloads after a 412 conflict and keeps per-field errors from a 400", async () => {
    let reloads = 0;
    configFetch({
      "GET /api/config": () => {
        reloads += 1;
        return jsonResponse(200, configFixture, { etag: configFixture.etag });
      },
      "PUT /api/config": jsonResponse(412, { error: "ETag does not match (If-Match)" }),
      "PUT /api/config?force=true": jsonResponse(400, { error: "validation failed", details: [{ pointer: "/groups/0/label", message: "duplicate group label 'PGM'" }] }),
    });
    const c = useConfig();
    await c.load();
    expect(await c.save()).toBeNull();
    expect(reloads).toBe(2);
    expect(c.message.value).toContain("changed meanwhile");
    expect(await c.save(c.draft.value, { force: true })).toBeNull();
    expect(c.errors.value["/groups/0/label"]).toContain("duplicate group label");
  });

  it("reports restart-required saves", async () => {
    configFetch({
      "GET /api/config": jsonResponse(200, configFixture, { etag: configFixture.etag }),
      "PUT /api/config": jsonResponse(200, { restart_required: true, restart_reasons: ["ptp"] }),
    });
    const c = useConfig();
    await c.load();
    await c.save();
    expect(c.message.value).toBe("Saved — restart required to apply.");
  });
});
