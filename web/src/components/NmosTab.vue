<script setup>
import { inject, onMounted, onUnmounted, ref } from "vue";
import Pill from "./Pill.vue";
import { api } from "../api.js";

const status = inject("status");
const nmos = ref(null);
const sdp = ref(null);
let timer = null;

async function refresh() {
  try {
    nmos.value = await api.get("/api/nmos");
  } catch {
    /* keep last */
  }
}
onMounted(() => {
  refresh();
  timer = setInterval(refresh, 3000);
});
onUnmounted(() => clearInterval(timer));

function params(e) {
  const tp = e.active?.transport_params || [];
  return tp
    .map((l, i) => {
      if ("mxl_flow_id" in l) return `domain ${l.mxl_domain_id ?? "null"} · flow ${l.mxl_flow_id ?? "null"}`;
      const dst = l.multicast_ip ?? l.destination_ip ?? "–";
      return `${i ? "R" : "P"}: ${dst}:${l.destination_port ?? "–"}${l.rtp_enabled === false ? " (off)" : ""}`;
    })
    .join("\n");
}
const hint = (e) => (e.tags?.["urn:x-nmos:tag:grouphint/v1.0"] || []).join(", ");
</script>

<template>
  <div v-if="!nmos" class="muted">Loading…</div>
  <div v-else-if="!nmos.enabled" class="panel muted">NMOS is not running ({{ nmos.setup_mode ? "setup mode" : "disabled" }}).</div>
  <template v-else>
    <div class="grid cards">
      <div class="panel">
        <h3>Node</h3>
        <dl class="kv">
          <dt>Node id</dt><dd>{{ nmos.node_id }}</dd>
          <dt>Device id</dt><dd>{{ nmos.device_id }}</dd>
          <dt>Registry</dt><dd>{{ nmos.registry_mode }}</dd>
          <dt>Registration</dt>
          <dd><Pill :text="nmos.registered ? 'registered' : 'not registered'" :kind="nmos.registered ? 'ok' : 'warn'" /> {{ nmos.registration_uri }}</dd>
        </dl>
        <div class="note">
          Raw resources: <a href="/x-nmos/node/v1.3/self" target="_blank">self</a> ·
          <a href="/x-nmos/node/v1.3/senders" target="_blank">senders</a> ·
          <a href="/x-nmos/node/v1.3/receivers" target="_blank">receivers</a> ·
          <a href="/x-nmos/connection/v1.2/single/" target="_blank">connection</a>
        </div>
      </div>
    </div>
    <div class="panel" v-for="kind in ['senders', 'receivers']" :key="kind">
      <h3>{{ kind }}</h3>
      <table>
        <thead><tr><th>Label</th><th>Transport</th><th>Group hint</th><th>master_enable</th><th>Active</th><th>Id</th></tr></thead>
        <tbody>
          <tr v-for="e in nmos[kind]" :key="e.id">
            <td>{{ e.label }}</td>
            <td>{{ e.transport.replace("urn:x-nmos:transport:", "") }}</td>
            <td>{{ hint(e) }}</td>
            <td><Pill :text="e.active?.master_enable ? 'on' : 'off'" :kind="e.active?.master_enable ? 'ok' : 'neutral'" /></td>
            <td style="white-space: pre-line; font-size: 0.75rem">{{ params(e) }}
              <button v-if="e.sdp" class="btn small secondary" @click="sdp = e">SDP</button>
            </td>
            <td class="note">{{ e.id }}</td>
          </tr>
        </tbody>
      </table>
    </div>
    <div class="panel" v-if="sdp">
      <h3>SDP — {{ sdp.label }} <button class="btn small secondary" @click="sdp = null">close</button></h3>
      <pre>{{ sdp.sdp }}</pre>
    </div>
  </template>
</template>
