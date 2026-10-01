<script setup>
import { computed, inject, onMounted } from "vue";
import Field from "./Field.vue";
import Pill from "./Pill.vue";
import { fmtCount, fmtBytes } from "../api.js";
import { useConfig } from "../useConfig.js";

const status = inject("status");
const { config, draft, errors, message, busy, load, save } = useConfig();
onMounted(load);
const prov = computed(() => config.value?.provenance || {});
const media = computed(() => status.value?.media);
const hasRedundant = computed(() => !!draft.value?.nic?.port_pairs?.[0]?.redundant);

function ensurePair() {
  draft.value.nic ||= {};
  draft.value.nic.port_pairs ||= [{ name: "media", primary: { name: "media-p" } }];
}
function toggleRedundant() {
  ensurePair();
  const pair = draft.value.nic.port_pairs[0];
  if (pair.redundant) delete pair.redundant;
  else pair.redundant = { name: "media-r" };
}
</script>

<template>
  <div class="grid wide" v-if="media">
    <div class="panel" v-for="p in media.ports" :key="p.name">
      <h3>{{ p.name }} <Pill :text="p.link_up ? 'link up' : 'link down'" :kind="p.link_up ? 'ok' : 'bad'" /></h3>
      <dl class="kv">
        <dt>PCI / ifname</dt><dd>{{ p.pci || p.ifname }}</dd>
        <dt>MAC</dt><dd>{{ p.mac || "–" }}</dd>
        <dt>IP</dt><dd>{{ p.ip }}</dd>
        <dt>Speed</dt><dd>{{ p.link_speed_mbps ? p.link_speed_mbps + " Mb/s" : "–" }}</dd>
        <dt>Bind mode</dt><dd>{{ p.bind_mode }}</dd>
        <dt>Driver</dt><dd>{{ p.driver }}</dd>
        <dt>DDP package</dt><dd>{{ p.ddp_package || "–" }}</dd>
        <dt>RX</dt><dd>{{ fmtCount(p.rx_packets) }} pkts · {{ fmtBytes(p.rx_bytes) }} · {{ p.rx_errors }} errors · {{ p.rx_missed }} missed</dd>
        <dt>TX</dt><dd>{{ fmtCount(p.tx_packets) }} pkts · {{ fmtBytes(p.tx_bytes) }}</dd>
      </dl>
    </div>
  </div>
  <div class="panel" v-if="draft">
    <h3>Port pair configuration <span class="muted">(restart required)</span></h3>
    <div class="grid fields">
      <Field :draft="draft" pointer="/nic/backend" label="Backend" type="select" :options="['dpdk', 'kernel', 'mock']" :provenance="prov" :errors="errors" />
      <Field :draft="draft" pointer="/nic/lcores" label="MTL lcores" placeholder="4-9" :provenance="prov" :errors="errors" />
      <Field :draft="draft" pointer="/nic/app_cpus" label="Worker CPUs" placeholder="10-15" :provenance="prov" :errors="errors" />
    </div>
    <template v-for="leg in hasRedundant ? ['primary', 'redundant'] : ['primary']" :key="leg">
      <h2>{{ leg }}</h2>
      <div class="grid fields">
        <Field :draft="draft" :pointer="`/nic/port_pairs/0/${leg}/name`" label="Name" :provenance="prov" :errors="errors" />
        <Field :draft="draft" :pointer="`/nic/port_pairs/0/${leg}/pci`" label="PCI address (dpdk)" placeholder="0000:31:00.0 or env:VAR" :provenance="prov" :errors="errors" />
        <Field :draft="draft" :pointer="`/nic/port_pairs/0/${leg}/ifname`" label="Interface (kernel)" :provenance="prov" :errors="errors" />
        <Field :draft="draft" :pointer="`/nic/port_pairs/0/${leg}/ip`" label="IP" :provenance="prov" :errors="errors" />
        <Field :draft="draft" :pointer="`/nic/port_pairs/0/${leg}/netmask`" label="Netmask" :provenance="prov" :errors="errors" />
        <Field :draft="draft" :pointer="`/nic/port_pairs/0/${leg}/gateway`" label="Gateway" :provenance="prov" :errors="errors" />
      </div>
    </template>
    <div class="actions">
      <span class="msg" :class="Object.keys(errors).length ? 'err' : 'ok'">{{ message }}</span>
      <button class="btn secondary" @click="toggleRedundant">{{ hasRedundant ? "Remove" : "Add" }} redundant port</button>
      <button class="btn secondary" @click="load">Discard</button>
      <button class="btn" :disabled="busy" @click="ensurePair(); save()">Save</button>
    </div>
  </div>
</template>
