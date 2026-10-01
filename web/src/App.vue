<script setup>
import { onMounted, onUnmounted, provide, ref } from "vue";
import { api } from "./api.js";
import Dashboard from "./components/Dashboard.vue";
import GroupsTab from "./components/GroupsTab.vue";
import NmosTab from "./components/NmosTab.vue";
import NetworkTab from "./components/NetworkTab.vue";
import PtpTab from "./components/PtpTab.vue";
import MxlTab from "./components/MxlTab.vue";
import ConfigTab from "./components/ConfigTab.vue";

const tabs = [
  { id: "dashboard", label: "Dashboard", component: Dashboard },
  { id: "groups", label: "Groups", component: GroupsTab },
  { id: "nmos", label: "NMOS", component: NmosTab },
  { id: "network", label: "Network", component: NetworkTab },
  { id: "ptp", label: "PTP", component: PtpTab },
  { id: "mxl", label: "MXL", component: MxlTab },
  { id: "config", label: "Configuration", component: ConfigTab },
];

const current = ref("dashboard");
const status = ref(null);
const statusError = ref("");
// PTP and clock history for the sparklines (§11.2: last 5 minutes kept in memory).
const history = ref([]);
provide("status", status);
provide("history", history);

function onHash() {
  const t = location.hash.slice(1);
  if (tabs.some((x) => x.id === t)) current.value = t;
}
function go(id) {
  current.value = id;
  location.hash = id;
}

async function poll() {
  try {
    status.value = await api.get("/api/status");
    statusError.value = "";
    const ptp = status.value.ptp || {};
    const sel = (ptp.ports || []).find((p) => p.selected) || (ptp.ports || [])[0];
    history.value.push({
      t: Date.now(),
      offset: sel?.offset_ns?.last ?? null,
      pathDelay: sel?.path_delay_ns?.last ?? null,
      clock: ptp.clock?.mtl_minus_host_tai_ns ?? null,
    });
    if (history.value.length > 300) history.value.splice(0, history.value.length - 300);
  } catch (e) {
    statusError.value = `status unavailable: ${e.message}`;
  }
}

let timer = null;
onMounted(() => {
  onHash();
  window.addEventListener("hashchange", onHash);
  poll();
  timer = setInterval(poll, 1000);
});
onUnmounted(() => {
  window.removeEventListener("hashchange", onHash);
  clearInterval(timer);
});

async function restart() {
  if (!confirm("Restart the gateway now? Active streams are interrupted.")) return;
  await api.post("/api/restart", {});
}
</script>

<template>
  <header>
    <h1>mxl-st2110-gateway</h1>
    <span class="node" v-if="status">{{ status.node.label }} · {{ status.node.id }}</span>
    <span style="flex: 1"></span>
    <span class="muted" style="font-size: 0.74rem" v-if="status">
      v{{ status.versions.gateway }} · MTL {{ status.versions.mtl }} · DPDK {{ status.versions.dpdk }} · MXL {{ status.versions.mxl }} · nmos-cpp
      {{ status.versions.nmos_cpp.slice(0, 7) }}
    </span>
  </header>
  <div class="banner bad" v-if="statusError">{{ statusError }}</div>
  <div class="banner info" v-if="status?.setup_mode">
    Setup mode: the NIC is not configured. Configure the port pair on the Network tab, then restart.
  </div>
  <div class="banner warn" v-if="status?.config_changed_on_disk">
    Configuration changed on disk — restart required. UI saves are blocked until resolved on the Configuration tab.
    <button class="btn small secondary" @click="go('config')">Resolve</button>
  </div>
  <div class="banner warn" v-else-if="status?.restart_required">
    Restart required ({{ status.restart_reasons.join(", ") }}).
    <button class="btn small secondary" @click="restart">Restart now</button>
  </div>
  <div class="banner bad" v-if="status?.readiness?.reasons?.includes('clock_mismatch')">
    MTL PTP time and host CLOCK_TAI disagree by more than ptp.max_offset_ns — MXL readers on this host see wrong grains.
  </div>
  <div class="banner warn" v-if="status?.media?.test_backend">
    Test backend ({{ status.media.backend }}): no pacing guarantees and no hardware PTP.
  </div>
  <nav>
    <button v-for="t in tabs" :key="t.id" :class="{ active: current === t.id }" @click="go(t.id)">{{ t.label }}</button>
  </nav>
  <main>
    <component :is="tabs.find((t) => t.id === current).component" />
  </main>
</template>
