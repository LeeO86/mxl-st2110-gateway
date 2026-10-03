<script setup>
import { computed, inject, onMounted, ref } from "vue";
import Pill from "./Pill.vue";
import Field from "./Field.vue";
import { api, errorMap } from "../api.js";
import { useConfig } from "../useConfig.js";

const status = inject("status");
const { config, draft, errors, message, busy, load, save } = useConfig();
const preflight = ref([]);
const logs = ref([]);
const importText = ref("");
const keepIds = ref(true);
const importReport = ref(null);
const importMessage = ref("");
onMounted(async () => {
  await load();
  preflight.value = await api.get("/api/preflight");
});
const prov = computed(() => config.value?.provenance || {});
const levelKind = { ok: "ok", info: "neutral", warn: "warn", fail: "bad" };

async function exportFile() {
  const r = await fetch("/api/config/export");
  const blob = await r.blob();
  const name = (r.headers.get("content-disposition") || "").match(/filename=([^;]+)/)?.[1] || "gateway.json";
  const a = document.createElement("a");
  a.href = URL.createObjectURL(blob);
  a.download = name;
  a.click();
}

function onFile(ev) {
  const f = ev.target.files[0];
  if (f) f.text().then((t) => (importText.value = t));
}

async function validateImport() {
  importMessage.value = "";
  try {
    importReport.value = (await api.post("/api/config/validate", importText.value)).body;
  } catch (e) {
    importReport.value = { valid: false, errors: e.details.length ? e.details : [{ pointer: "", message: e.message }] };
  }
}

async function doImport() {
  if (!keepIds.value && !confirm("Regenerate node.id and all uids? The imported gateway gets new NMOS identities (use this to clone onto another host).")) return;
  try {
    await api.post(`/api/config/import?keep_ids=${keepIds.value}`, importText.value);
    importMessage.value = "Imported — restart to apply.";
    await load();
  } catch (e) {
    importReport.value = { valid: false, errors: e.details };
    importMessage.value = e.message;
  }
}

async function restart() {
  if (!confirm("Restart the gateway now? It re-reads the configuration from disk.")) return;
  await api.post("/api/restart", {});
}

async function overwrite() {
  if (!confirm("Overwrite the file on disk with the configuration the UI shows?")) return;
  await save(config.value.file, { force: true });
}

async function loadLogs() {
  logs.value = await api.get("/api/logs?lines=200");
}
</script>

<template>
  <div class="panel" v-if="config?.changed_on_disk">
    <h3>Configuration changed on disk</h3>
    <p class="note">The file was edited outside the UI. Hand edits apply after a restart; UI saves are blocked until you choose.</p>
    <div class="actions">
      <button class="btn" @click="restart">Reload from disk (restart)</button>
      <button class="btn secondary" @click="overwrite">Overwrite with UI state</button>
    </div>
  </div>

  <div class="grid cards">
    <div class="panel">
      <h3>File</h3>
      <dl class="kv" v-if="config">
        <dt>Path</dt><dd>{{ config.path }}</dd>
        <dt>ETag</dt><dd>{{ config.etag }}</dd>
        <dt>Restart required</dt><dd>{{ config.restart_required ? config.restart_reasons.join(", ") : "no" }}</dd>
      </dl>
      <div class="actions">
        <button class="btn secondary" @click="exportFile">Export</button>
        <button class="btn danger" @click="restart">Restart</button>
      </div>
    </div>
    <div class="panel">
      <h3>Environment overrides</h3>
      <table v-if="config?.environment?.length">
        <tr v-for="b in config.environment" :key="b.pointer"><td><code>{{ b.variable }}</code></td><td class="note">{{ b.pointer }}</td></tr>
      </table>
      <div v-else class="muted">none</div>
    </div>
  </div>

  <div class="panel" v-if="draft">
    <h3>Node <span class="muted">(restart required)</span></h3>
    <div class="grid fields">
      <Field :draft="draft" pointer="/node/label" label="Label" :provenance="prov" :errors="errors" />
      <Field :draft="draft" pointer="/node/description" label="Description" :provenance="prov" :errors="errors" />
      <Field :draft="draft" pointer="/node/seed" label="Id seed" placeholder="(node id)" :provenance="prov" :errors="errors" nullable />
      <Field :draft="draft" pointer="/node/http_port" label="NMOS port (MXL node)" type="number" :provenance="prov" :errors="errors" />
      <Field :draft="draft" pointer="/node/web_port" label="Web port" type="number" placeholder="(NMOS port)" :provenance="prov" :errors="errors" nullable />
      <Field :draft="draft" pointer="/node/host_address" label="Host address (IPv4)" placeholder="(detected)" :provenance="prov" :errors="errors" nullable />
      <Field :draft="draft" pointer="/node/public_port" label="Public port" type="number" :provenance="prov" :errors="errors" nullable />
      <Field :draft="draft" pointer="/node/registry/dns_sd" label="DNS-SD" type="bool" :provenance="prov" :errors="errors" nullable />
      <Field :draft="draft" pointer="/node/registry/address" label="Registry address" :provenance="prov" :errors="errors" nullable />
      <Field :draft="draft" pointer="/node/registry/port" label="Registry port" type="number" placeholder="3210" :provenance="prov" :errors="errors" nullable />
      <Field :draft="draft" pointer="/node/registry/query_address" label="Query address" placeholder="(registry address)" :provenance="prov" :errors="errors" nullable />
      <Field :draft="draft" pointer="/node/registry/query_port" label="Query port" type="number" placeholder="(registry port + 1)" :provenance="prov" :errors="errors" nullable />
      <Field :draft="draft" pointer="/node/st2110/enabled" label="ST 2110 node" type="bool" :provenance="prov" :errors="errors" />
      <Field :draft="draft" pointer="/node/st2110/http_port" label="ST 2110 node port" type="number" placeholder="(NMOS port + 1)" :provenance="prov" :errors="errors" nullable />
      <Field :draft="draft" pointer="/node/st2110/registry/dns_sd" label="ST 2110 DNS-SD" type="bool" :provenance="prov" :errors="errors" nullable />
      <Field :draft="draft" pointer="/node/st2110/registry/address" label="ST 2110 registry address" :provenance="prov" :errors="errors" nullable />
      <Field :draft="draft" pointer="/node/st2110/registry/port" label="ST 2110 registry port" type="number" placeholder="3210" :provenance="prov" :errors="errors" nullable />
      <Field :draft="draft" pointer="/node/resume_connections" label="Resume connections" type="bool" :provenance="prov" :errors="errors" />
      <Field :draft="draft" pointer="/node/shutdown_timeout_s" label="Shutdown timeout (s)" type="number" placeholder="10" :provenance="prov" :errors="errors" />
      <Field :draft="draft" pointer="/node/log_level" label="Log level" type="select" :options="['trace', 'debug', 'info', 'warn', 'error']" :provenance="prov" :errors="errors" />
    </div>
    <div class="actions">
      <span class="msg" :class="Object.keys(errors).length ? 'err' : 'ok'">{{ message }}</span>
      <button class="btn secondary" @click="load">Discard</button>
      <button class="btn" :disabled="busy" @click="save()">Save</button>
    </div>
  </div>

  <div class="panel">
    <h3>Import</h3>
    <input type="file" accept="application/json,.json" @change="onFile" />
    <textarea v-model="importText" placeholder="paste a gateway.json"></textarea>
    <label><input type="checkbox" v-model="keepIds" /> keep node.id and uids (uncheck to clone onto another host)</label>
    <div v-if="importReport" class="msg" :class="importReport.valid ? 'ok' : 'err'">
      {{ importReport.valid ? "Valid." : "Invalid:" }}
      <div v-for="e in importReport.errors" :key="e.pointer + e.message">{{ e.pointer || "/" }}: {{ e.message }}</div>
    </div>
    <div class="actions">
      <span class="msg ok">{{ importMessage }}</span>
      <button class="btn secondary" :disabled="!importText" @click="validateImport">Validate</button>
      <button class="btn" :disabled="!importText" @click="doImport">Import (restart required)</button>
    </div>
  </div>

  <div class="panel">
    <h3>Preflight</h3>
    <table>
      <tr v-for="c in preflight" :key="c.id">
        <td><Pill :text="c.level" :kind="levelKind[c.level]" /></td>
        <td>{{ c.id }}</td>
        <td>{{ c.message }}</td>
      </tr>
    </table>
  </div>

  <div class="panel" v-if="config">
    <h3>Raw file (read-only)</h3>
    <pre>{{ JSON.stringify(config.file, null, 2) }}</pre>
  </div>

  <div class="panel">
    <h3>Log <button class="btn small secondary" @click="loadLogs">load last 200 lines</button></h3>
    <pre v-if="logs.length">{{ logs.join("\n") }}</pre>
  </div>
</template>
