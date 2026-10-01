<script setup>
import { computed, inject } from "vue";
import Pill from "./Pill.vue";
import { STATE_KIND, fmtNs, fmtCount, legHealth } from "../api.js";

const status = inject("status");
const ready = computed(() => status.value?.readiness);
const ptpPorts = computed(() => status.value?.ptp?.ports || []);
const selected = computed(() => ptpPorts.value.find((p) => p.selected) || ptpPorts.value[0]);
const clock = computed(() => status.value?.ptp?.clock);
const clockKind = computed(() => {
  const c = clock.value;
  if (!c) return "neutral";
  const a = Math.abs(c.mtl_minus_host_tai_ns);
  return a > status.value.ptp.max_offset_ns ? "bad" : a > status.value.ptp.warn_offset_ns ? "warn" : "ok";
});
</script>

<template>
  <div v-if="!status" class="muted">Loading…</div>
  <template v-else>
    <div class="grid cards">
      <div class="panel">
        <h3>Readiness</h3>
        <Pill :text="ready.ready ? 'ready' : 'not ready'" :kind="ready.ready ? 'ok' : 'bad'" />
        <div class="note" v-if="ready.reasons.length">Reasons: {{ ready.reasons.join(", ") }}</div>
        <div class="note" v-if="ready.warnings.length">Warnings: {{ ready.warnings.join(", ") }}</div>
      </div>
      <div class="panel">
        <h3>PTP</h3>
        <dl class="kv">
          <dt>Mode</dt><dd>{{ status.ptp.mode }} (domain {{ status.ptp.domain }})</dd>
          <template v-if="selected">
            <dt>Lock</dt><dd><Pill :text="selected.locked ? 'locked' : 'unlocked'" :kind="selected.locked ? 'ok' : 'bad'" /> port {{ selected.leg }}</dd>
            <dt>GM</dt><dd>{{ selected.grandmaster?.identity || "–" }}</dd>
            <dt>Offset</dt><dd>{{ fmtNs(selected.offset_ns?.last) }}</dd>
          </template>
          <template v-else><dt>Status</dt><dd class="muted">external clock / no PTP status</dd></template>
        </dl>
      </div>
      <div class="panel">
        <h3>MTL − host TAI</h3>
        <Pill v-if="clock" :text="fmtNs(clock.mtl_minus_host_tai_ns)" :kind="clockKind" />
        <span v-else class="muted">–</span>
        <div class="note" v-if="clock">60 s: {{ fmtNs(clock.min_60s_ns) }} … {{ fmtNs(clock.max_60s_ns) }}</div>
        <div class="note">kernel TAI offset {{ status.ptp.kernel_tai_offset_s }} s</div>
      </div>
      <div class="panel" v-if="status.media">
        <h3>NIC ({{ status.media.backend }})</h3>
        <div v-for="p in status.media.ports" :key="p.name" style="margin-bottom: 0.3rem">
          <Pill :text="p.link_up ? 'up' : 'down'" :kind="p.link_up ? 'ok' : 'bad'" />
          {{ p.name }} <span class="muted">{{ p.link_speed_mbps ? p.link_speed_mbps / 1000 + "G" : "" }} {{ p.bind_mode }}</span>
        </div>
      </div>
      <div class="panel">
        <h3>NMOS</h3>
        <Pill :text="status.nmos?.registered ? 'registered' : 'not registered'" :kind="status.nmos?.registered ? 'ok' : 'warn'" />
        <div class="note">{{ status.nmos?.registration_uri || status.nmos?.registry_mode }}</div>
      </div>
    </div>

    <h2>Groups</h2>
    <div class="grid wide">
      <div class="panel" v-for="g in status.groups" :key="g.uid">
        <h3>
          {{ g.label }} <Pill :text="g.direction" kind="neutral" />
          <Pill v-if="!g.enabled" text="disabled" kind="neutral" />
          <span class="muted" v-if="g.direction === 'egress' && g.output_delay_ns">delay {{ fmtNs(g.output_delay_ns) }}</span>
        </h3>
        <div v-for="e in g.essences" :key="e.uid" class="essence">
          <div class="row" style="align-items: center">
            <strong style="flex: 2">{{ e.label }}</strong>
            <Pill :text="e.state" :kind="STATE_KIND[e.state] || 'neutral'" :title="e.reason" />
          </div>
          <div class="note" v-if="e.reason">{{ e.reason }}</div>
          <dl class="kv" v-if="e.direction === 'ingest'">
            <dt>Frames</dt><dd>{{ fmtCount(e.frames.complete) }} ok · {{ e.frames.incomplete }} incomplete · {{ e.frames.dropped }} dropped</dd>
            <dt>2022-7 legs</dt>
            <dd class="legs">
              <span v-for="(l, i) in e.legs" :key="i" class="leg" :class="legHealth(l.packets, l.lost)" :title="`${i ? 'R' : 'P'}: ${l.packets} pkts, ${l.lost} lost`"></span>
            </dd>
            <dt>MXL</dt><dd>{{ fmtCount(e.type === "audio" ? e.samples_written : e.grains_written) }} {{ e.type === "audio" ? "samples" : "grains" }} · {{ e.write_errors }} errors</dd>
            <dt>Origin age</dt><dd>{{ fmtNs(e.origin_age_ns) }}</dd>
          </dl>
          <dl class="kv" v-else>
            <dt>Domain</dt>
            <dd>
              <template v-if="e.resolved_domain">{{ e.resolved_domain.kind }} · {{ e.resolved_domain.path }}</template>
              <span v-else class="muted">–</span>
            </dd>
            <dt>Read</dt><dd>{{ fmtCount(e.grains_read) }} · {{ e.read_timeouts }} timeouts · {{ e.late_reads }} late · {{ e.grains_invalid }} invalid</dd>
            <dt>Sent</dt><dd>{{ fmtCount(e.frames_sent) }} frames · {{ e.late_frames }} late · lead {{ fmtNs(e.lead_ns) }}</dd>
            <dt>Lag</dt><dd>{{ e.read_lag_grains ?? "–" }} grains (offset {{ fmtNs(e.read_offset_ns) }})</dd>
          </dl>
        </div>
      </div>
    </div>
  </template>
</template>
