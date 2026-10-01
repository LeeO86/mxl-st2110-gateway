<script setup>
import { computed, inject, onMounted, ref } from "vue";
import Field from "./Field.vue";
import Pill from "./Pill.vue";
import { api, fmtBytes, STATE_KIND } from "../api.js";
import { useConfig } from "../useConfig.js";

const status = inject("status");
const domains = ref(null);
const flows = ref(null);
const flowDomain = ref(null);
const flowError = ref("");
const { config, draft, errors, message, busy, load, save } = useConfig();
const prov = computed(() => config.value?.provenance || {});

async function refresh() {
  domains.value = await api.get("/api/domains");
}
onMounted(() => {
  refresh();
  load();
});

async function browse(d) {
  flowDomain.value = d;
  flowError.value = "";
  try {
    flows.value = (await api.get(`/api/flows?domain=${encodeURIComponent(d.id)}`)).flows;
  } catch (e) {
    flowError.value = e.message;
    flows.value = [];
  }
}

const receivers = computed(() =>
  (status.value?.groups || []).flatMap((g) => g.essences.filter((e) => e.direction === "egress").map((e) => ({ ...e, group: g.label }))),
);
const kindPill = (k) => (k === "mirror" ? "warn" : k === "configured" ? "ok" : "neutral");
</script>

<template>
  <div v-if="!domains" class="muted">Loading…</div>
  <template v-else>
    <div class="panel">
      <h3>Accessible domains <span class="muted">scan path {{ domains.scan_path ?? "off" }}</span> <button class="btn small secondary" @click="refresh">rescan view</button></h3>
      <table>
        <thead><tr><th>Kind</th><th>Label</th><th>Id (domain_def.json)</th><th>Path</th><th>tmpfs</th><th>Usage</th><th>Flows</th><th></th></tr></thead>
        <tbody>
          <tr v-for="d in domains.domains" :key="d.id + d.path">
            <td><Pill :text="d.kind" :kind="kindPill(d.kind)" /><div class="note" v-if="d.source_host_id">from {{ d.source_host_id }}</div></td>
            <td>{{ d.label }}<div class="note" v-if="d.name">config: {{ d.name }}</div></td>
            <td class="note">{{ d.id }}</td>
            <td>{{ d.path }}</td>
            <td><Pill :text="d.tmpfs ? d.fs_type : d.fs_type || 'no'" :kind="d.tmpfs ? 'ok' : 'bad'" /></td>
            <td>{{ fmtBytes(d.total_bytes - d.free_bytes) }} / {{ fmtBytes(d.total_bytes) }}</td>
            <td class="num">{{ d.flows }}</td>
            <td><button class="btn small secondary" @click="browse(d)">flows</button></td>
          </tr>
        </tbody>
      </table>
      <div class="note" v-for="c in domains.conflicts" :key="c.path">Conflict: id {{ c.id }} at {{ c.path }} (excluded)</div>
      <div class="note" v-for="s in domains.skipped" :key="s.path">Skipped: {{ s.path }} — {{ s.reason }}</div>
    </div>

    <div class="panel" v-if="flowDomain">
      <h3>Flows in {{ flowDomain.label }} <span class="muted">{{ flowDomain.path }}</span> <button class="btn small secondary" @click="flowDomain = null">close</button></h3>
      <div class="msg err" v-if="flowError">{{ flowError }}</div>
      <table>
        <thead><tr><th>Id</th><th>Label</th><th>Media type</th><th>Active</th><th class="num">Head index</th><th>Last write (TAI)</th></tr></thead>
        <tbody>
          <tr v-for="f in flows" :key="f.id">
            <td class="note">{{ f.id }}</td>
            <td>{{ f.label }}</td>
            <td>{{ f.media_type }}</td>
            <td><Pill :text="f.active ? 'active' : 'inactive'" :kind="f.active ? 'ok' : 'neutral'" /></td>
            <td class="num">{{ f.head_index ?? "–" }}</td>
            <td class="note">{{ f.last_write_tai_ns ? new Date(f.last_write_tai_ns / 1e6 - 37000).toISOString() : "–" }}</td>
          </tr>
        </tbody>
      </table>
    </div>

    <div class="panel">
      <h3>MXL Receivers (egress)</h3>
      <table>
        <thead><tr><th>Group</th><th>Essence</th><th>State</th><th>Resolved domain</th><th class="num">Not found</th><th class="num">Lag</th><th class="num">Read offset</th></tr></thead>
        <tbody>
          <tr v-for="e in receivers" :key="e.uid">
            <td>{{ e.group }}</td>
            <td>{{ e.label }}</td>
            <td><Pill :text="e.state" :kind="STATE_KIND[e.state]" /><div class="note">{{ e.reason }}</div></td>
            <td>
              <template v-if="e.resolved_domain"><Pill :text="e.resolved_domain.kind" :kind="kindPill(e.resolved_domain.kind)" /> {{ e.resolved_domain.path }}</template>
              <span v-else class="muted">–</span>
            </td>
            <td class="num">{{ e.flow_not_found }}</td>
            <td class="num">{{ e.read_lag_grains ?? "–" }}</td>
            <td class="num">{{ (e.read_offset_ns / 1e6).toFixed(1) }} ms</td>
          </tr>
          <tr v-if="!receivers.length"><td colspan="7" class="muted">No egress groups.</td></tr>
        </tbody>
      </table>
    </div>

    <div class="panel" v-if="draft">
      <h3>MXL configuration <span class="muted">(restart required)</span></h3>
      <div class="grid fields">
        <Field :draft="draft" pointer="/mxl/scan_path" label="Scan path (MXL root)" :provenance="prov" :errors="errors" nullable />
        <Field :draft="draft" pointer="/mxl/default_read_offset_grains" label="Default read offset (grains)" type="number" :provenance="prov" :errors="errors" />
      </div>
      <h2>Configured domains</h2>
      <div v-for="(d, i) in draft.mxl.domains" :key="i" class="grid fields essence">
        <Field :draft="draft" :pointer="`/mxl/domains/${i}/name`" label="Name" :provenance="prov" :errors="errors" />
        <Field :draft="draft" :pointer="`/mxl/domains/${i}/path`" label="Path" :provenance="prov" :errors="errors" />
        <Field :draft="draft" :pointer="`/mxl/domains/${i}/id`" label="Id (written by the gateway)" :provenance="prov" :errors="errors" nullable />
        <Field :draft="draft" :pointer="`/mxl/domains/${i}/label`" label="Label" :provenance="prov" :errors="errors" />
        <Field :draft="draft" :pointer="`/mxl/domains/${i}/history_duration_ns`" label="History (ns)" type="number" :provenance="prov" :errors="errors" />
        <Field :draft="draft" :pointer="`/mxl/domains/${i}/gc_on_start`" label="Domain-wide GC on start" type="bool" :provenance="prov" :errors="errors" />
      </div>
      <div class="actions">
        <span class="msg" :class="Object.keys(errors).length ? 'err' : 'ok'">{{ message }}</span>
        <button class="btn secondary" @click="draft.mxl.domains.push({ name: 'domain' + (draft.mxl.domains.length + 1), path: '' })">Add domain</button>
        <button class="btn secondary" @click="load">Discard</button>
        <button class="btn" :disabled="busy" @click="save()">Save</button>
      </div>
    </div>
  </template>
</template>
