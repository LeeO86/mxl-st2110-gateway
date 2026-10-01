// @vitest-environment happy-dom
import { afterEach, describe, expect, it, vi } from "vitest";
import { flushPromises, mount } from "@vue/test-utils";
import { ref } from "vue";
import GroupsTab from "../src/components/GroupsTab.vue";
import statusFixture from "./fixtures/status.json";
import configFixture from "./fixtures/config.json";

function jsonResponse(status, body, headers = {}) {
  const h = new Map(Object.entries({ "content-type": "application/json", ...headers }));
  return { ok: status >= 200 && status < 300, status, headers: { get: (k) => h.get(k.toLowerCase()) ?? null }, json: async () => body, text: async () => JSON.stringify(body) };
}

function backend(overrides = {}) {
  const calls = [];
  vi.stubGlobal(
    "fetch",
    vi.fn(async (path, opts = {}) => {
      const key = `${opts.method || "GET"} ${path}`;
      calls.push({ key, body: opts.body ? JSON.parse(opts.body) : undefined });
      if (overrides[key]) return overrides[key];
      if (key === "GET /api/config") return jsonResponse(200, configFixture, { etag: configFixture.etag });
      return jsonResponse(200, { restart_required: false });
    }),
  );
  return calls;
}

let wrapper = null;
async function mountTab() {
  if (!HTMLDialogElement.prototype.showModal) {
    HTMLDialogElement.prototype.showModal = function () { this.open = true; };
    HTMLDialogElement.prototype.close = function () { this.open = false; };
  }
  wrapper = mount(GroupsTab, { attachTo: document.body, global: { provide: { status: ref(structuredClone(statusFixture)) } } });
  await flushPromises();
  return wrapper;
}
const button = (w, text) => w.findAll("button").find((b) => b.text() === text);

afterEach(() => {
  wrapper?.unmount();
  wrapper = null;
  vi.unstubAllGlobals();
  vi.restoreAllMocks();
});

describe("Groups tab", () => {
  it("lists the configured groups with their runtime states", async () => {
    backend();
    const w = await mountTab();
    const rows = w.findAll("tbody tr");
    expect(rows.map((r) => r.find("strong").text())).toEqual(["CAM 1", "PGM"]);
    expect(rows[0].text()).toContain("1 V · 1 A · 0 ANC");
    expect(rows[0].text()).toContain("2022-7");
  });

  it("creates a group from counts", async () => {
    const calls = backend({ "POST /api/groups": jsonResponse(201, { group: {} }) });
    const w = await mountTab();
    await button(w, "Create group").trigger("click");
    await w.find("dialog input").setValue("CAM 2");
    const audioPlus = w.findAll(".stepper")[1].findAll("button")[1];
    await audioPlus.trigger("click");
    await button(w, "Create").trigger("click");
    await flushPromises();
    const post = calls.find((c) => c.key === "POST /api/groups");
    expect(post.body).toEqual({ label: "CAM 2", direction: "ingest", domain: "main", redundancy: false, counts: { video: 1, audio: 3, anc: 1 } });
    expect(w.text()).toContain("Group CAM 2 created.");
    expect(calls.filter((c) => c.key === "GET /api/config").length).toBe(2);
  });

  it("shows server validation errors in the create dialog", async () => {
    backend({ "POST /api/groups": jsonResponse(400, { error: "validation failed", details: [{ pointer: "/label", message: "duplicate group label 'PGM'" }, { pointer: "/counts/video", message: "must be an integer 0..32" }] }) });
    const w = await mountTab();
    await button(w, "Create group").trigger("click");
    await w.find("dialog input").setValue("PGM");
    await button(w, "Create").trigger("click");
    await flushPromises();
    const text = w.find("dialog").text();
    expect(text.match(/duplicate group label/g)).toHaveLength(1);
    expect(text).toContain("/counts/video must be an integer 0..32");
  });

  it("edits a group and keeps its uid", async () => {
    const calls = backend();
    const w = await mountTab();
    await w.findAll("tbody tr")[0].findAll("button")[0].trigger("click"); // Edit
    await w.find(".panel input").setValue("CAM 1 edited");
    await button(w, "Save group").trigger("click");
    await flushPromises();
    const uid = configFixture.file.groups[0].uid;
    const put = calls.find((c) => c.key === `PUT /api/groups/${uid}`);
    expect(put.body.label).toBe("CAM 1 edited");
    expect(put.body.uid).toBe(uid);
    expect(w.text()).toContain("Saved and applied live.");
  });

  it("deletes a group after confirmation", async () => {
    const calls = backend();
    vi.stubGlobal("confirm", vi.fn(() => true));
    const w = await mountTab();
    await w.findAll("tbody tr")[1].findAll("button")[2].trigger("click"); // Delete
    await flushPromises();
    expect(calls.some((c) => c.key === `DELETE /api/groups/${configFixture.file.groups[1].uid}`)).toBe(true);
    expect(w.text()).toContain("Group PGM deleted.");
  });
});
