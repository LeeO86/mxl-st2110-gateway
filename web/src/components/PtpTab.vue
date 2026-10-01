<script setup>
import { computed, inject, onMounted } from "vue";
import Field from "./Field.vue";
import Pill from "./Pill.vue";
import Sparkline from "./Sparkline.vue";
import { fmtNs } from "../api.js";
import { useConfig } from "../useConfig.js";

const status = inject("status");
const history = inject("history");
const ptp = computed(() => status.value?.ptp);
const { config, draft, errors, message, busy, load, save } = useConfig();
onMounted(load);
const prov = computed(() => config.value?.provenance || {});
const series = (k) => history.value.map((h) => h[k]);
</script>

<template>
  <div v-if="!ptp" class="muted">Loading…</div>
  <template v-else>
    <div class="grid cards">
      <div class="panel">
        <h3>Mode</h3>
        <dl class="kv">
          <dt>Mode</dt><dd>{{ ptp.mode }}</dd>
          <dt>Domain</dt><dd>{{ ptp.domain }}</dd>
          <dt>Require lock</dt><dd>{{ ptp.require_lock }}</dd>
          <dt>Status API</dt><dd>{{ ptp.available ? "MTL patch 0001" : "unavailable" }}</dd>
          <dt>Selection changes</dt><dd>{{ ptp.selection_changes ?? "–" }}</dd>
          <dt>PHC2SYS</dt><dd>{{ ptp.phc2sys_locked ? "locked" : "–" }}</dd>
        </dl>
      </div>
      <div class="panel">
        <h3>MTL − host CLOCK_TAI</h3>
        <dl class="kv" v-if="ptp.clock">
          <dt>Now</dt><dd>{{ fmtNs(ptp.clock.mtl_minus_host_tai_ns) }}</dd>
          <dt>60 s min/max</dt><dd>{{ fmtNs(ptp.clock.min_60s_ns) }} / {{ fmtNs(ptp.clock.max_60s_ns) }}</dd>
          <dt>Thresholds</dt><dd>warn {{ fmtNs(ptp.warn_offset_ns) }} · max {{ fmtNs(ptp.max_offset_ns) }}</dd>
          <dt>Kernel TAI offset</dt><dd>{{ ptp.kernel_tai_offset_s }} s</dd>
        </dl>
        <Sparkline :values="series('clock')" />
      </div>
    </div>
    <div class="grid wide">
      <div class="panel" v-for="p in ptp.ports" :key="p.leg">
        <h3>
          Port {{ p.leg.toUpperCase() }}
          <Pill :text="p.locked ? 'locked' : p.active ? 'unlocked' : 'inactive'" :kind="p.locked ? 'ok' : p.active ? 'bad' : 'neutral'" />
          <Pill v-if="p.selected" text="steers PHC" kind="ok" />
        </h3>
        <dl class="kv">
          <dt>Grandmaster</dt><dd>{{ p.grandmaster?.identity || "–" }}</dd>
          <dt>GM priority1/2</dt><dd>{{ p.grandmaster ? `${p.grandmaster.priority1} / ${p.grandmaster.priority2}` : "–" }}</dd>
          <dt>Clock class/accuracy</dt><dd>{{ p.grandmaster ? `${p.grandmaster.clock_class} / 0x${p.grandmaster.clock_accuracy.toString(16)}` : "–" }}</dd>
          <dt>Steps removed</dt><dd>{{ p.grandmaster?.steps_removed ?? "–" }}</dd>
          <dt>Parent</dt><dd>{{ p.parent || "–" }}</dd>
          <dt>UTC offset</dt><dd>{{ p.utc_offset }} s (display only)</dd>
          <dt>Offset</dt><dd>{{ fmtNs(p.offset_ns.last) }} ({{ fmtNs(p.offset_ns.min) }} … {{ fmtNs(p.offset_ns.max) }})</dd>
          <dt>Path delay</dt><dd>{{ fmtNs(p.path_delay_ns.last) }} ({{ fmtNs(p.path_delay_ns.min) }} … {{ fmtNs(p.path_delay_ns.max) }})</dd>
          <dt>Syncs</dt><dd>{{ p.sync_count }}</dd>
          <dt>GM changes</dt><dd>{{ p.gm_changes }}</dd>
        </dl>
        <template v-if="p.selected">
          <label>Offset (last 5 min)</label><Sparkline :values="series('offset')" />
          <label>Path delay (last 5 min)</label><Sparkline :values="series('pathDelay')" />
        </template>
      </div>
    </div>
    <div class="panel" v-if="draft">
      <h3>PTP configuration <span class="muted">(restart required)</span></h3>
      <div class="grid fields">
        <Field :draft="draft" pointer="/ptp/mode" label="Mode" type="select" :options="['builtin', 'builtin_phc2sys', 'external']" :provenance="prov" :errors="errors" />
        <Field :draft="draft" pointer="/ptp/domain" label="Domain" type="number" :provenance="prov" :errors="errors" />
        <Field :draft="draft" pointer="/ptp/require_lock" label="Require lock" type="bool" :provenance="prov" :errors="errors" />
        <Field :draft="draft" pointer="/ptp/warn_offset_ns" label="Warn offset (ns)" type="number" :provenance="prov" :errors="errors" />
        <Field :draft="draft" pointer="/ptp/max_offset_ns" label="Max offset (ns)" type="number" :provenance="prov" :errors="errors" />
      </div>
      <p class="note" v-if="draft.ptp?.mode === 'builtin_phc2sys'">
        builtin_phc2sys steers CLOCK_REALTIME to TAI (the wall clock runs 37 s ahead of UTC) and needs a kernel TAI offset of 0 — dedicated appliances only.
      </p>
      <div class="actions">
        <span class="msg" :class="Object.keys(errors).length ? 'err' : 'ok'">{{ message }}</span>
        <button class="btn secondary" @click="load">Discard</button>
        <button class="btn" :disabled="busy" @click="save()">Save</button>
      </div>
    </div>
  </template>
</template>
