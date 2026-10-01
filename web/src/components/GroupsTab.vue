<script setup>
import { computed, inject, onMounted, ref } from "vue";
import Pill from "./Pill.vue";
import { api, clone, errorMap, fieldError, STATE_KIND } from "../api.js";
import { useConfig } from "../useConfig.js";

const status = inject("status");
const { config, load } = useConfig();
const editing = ref(null); // {index, group}
const editErrors = ref({});
const message = ref("");
const creating = ref(null);
const createErrors = ref({});
const createDialog = ref(null);

const RATES = ["23.98", "24/1", "25/1", "29.97", "30/1", "50/1", "59.94", "60/1"];
const RATE_VALUES = { "23.98": "24000/1001", "29.97": "30000/1001", "59.94": "60000/1001" };
const rateValue = (r) => RATE_VALUES[r] || r;

const groups = computed(() => config.value?.file?.groups || []);
// errors without their own field in the create dialog
const otherCreateErrors = computed(() => Object.entries(createErrors.value).filter(([p]) => p !== "/label" && p !== ""));
const domains = computed(() => (config.value?.effective?.mxl?.domains || []).map((d) => d.name));
const runtime = (uid) => status.value?.groups?.find((g) => g.uid === uid);

onMounted(load);

function startCreate() {
  creating.value = { label: "", direction: "ingest", domain: domains.value[0] || "", redundancy: false, counts: { video: 1, audio: 2, anc: 1 } };
  createErrors.value = {};
  createDialog.value.showModal();
}

async function create() {
  try {
    await api.post("/api/groups", creating.value);
    createDialog.value.close();
    message.value = `Group ${creating.value.label} created.`;
    await load();
  } catch (e) {
    createErrors.value = errorMap(e);
    if (!e.details.length) createErrors.value[""] = e.message;
  }
}

function edit(index) {
  editing.value = { index, group: clone(groups.value[index]) };
  editErrors.value = {};
}

function prefix() {
  return `/groups/${editing.value.index}`;
}
function err(p) {
  return fieldError(editErrors.value, prefix() + p);
}

async function saveEdit() {
  const g = editing.value.group;
  try {
    const r = await api.put(`/api/groups/${g.uid}`, g);
    message.value = r.body.restart_required ? "Saved — restart required for some changes." : "Saved and applied live.";
    editing.value = null;
    await load();
  } catch (e) {
    editErrors.value = errorMap(e);
    message.value = e.message;
  }
}

async function remove(g) {
  const n = (g.video?.length || 0) + (g.audio?.length || 0) + (g.anc?.length || 0);
  if (!confirm(`Delete group "${g.label}"? Its ${n} Senders and ${n} Receivers are removed from NMOS and active connections are interrupted.`)) return;
  try {
    await api.del(`/api/groups/${g.uid}`);
    message.value = `Group ${g.label} deleted.`;
    await load();
  } catch (e) {
    message.value = e.message;
  }
}

async function duplicate(g) {
  try {
    await api.post("/api/groups", { duplicate_of: g.uid });
    message.value = `Group ${g.label} duplicated (new uids).`;
    await load();
  } catch (e) {
    message.value = e.message;
  }
}

function legsOf(e) {
  if (!e.defaults) e.defaults = { legs: [] };
  if (!e.defaults.legs) e.defaults.legs = [];
  const count = editing.value.group.redundancy ? 2 : 1;
  while (e.defaults.legs.length < count) e.defaults.legs.push({ multicast: "", port: 20000 });
  return e.defaults.legs.slice(0, count);
}

function cleanLegs(group) {
  for (const list of ["video", "audio", "anc"]) {
    for (const e of group[list] || []) {
      if (e.defaults?.legs) {
        e.defaults.legs = e.defaults.legs.filter((l) => l.multicast);
        for (const l of e.defaults.legs) {
          if (!l.source) delete l.source;
          l.port = Number(l.port);
        }
        if (!e.defaults.legs.length) delete e.defaults;
      }
    }
  }
}

function addEssence(type) {
  const g = editing.value.group;
  const base = { uid: crypto.randomUUID ? crypto.randomUUID() : undefined, label: `${g.label} ${type.toUpperCase()}${(g[type]?.length || 0) + 1}` };
  if (type === "video") Object.assign(base, { width: 1920, height: 1080, rate: "50/1", interlace: "progressive", payload_type: 96 });
  if (type === "audio") Object.assign(base, { channels: 8, bit_depth: 24, ptime_us: 1000, block_us: 1000, payload_type: 97 });
  if (type === "anc") Object.assign(base, { payload_type: 100 });
  (g[type] ||= []).push(base);
}

function num(e, key, v) {
  if (v === "" || v === null) delete e[key];
  else e[key] = Number(v);
}

async function submitEdit() {
  cleanLegs(editing.value.group);
  await saveEdit();
}
</script>

<template>
  <div class="actions" style="justify-content: space-between">
    <span class="msg" :class="{ ok: message && !message.includes('fail') }">{{ message }}</span>
    <button class="btn" @click="startCreate" :disabled="!config">Create group</button>
  </div>

  <div class="panel" v-if="config && !editing">
    <table>
      <thead>
        <tr><th>Label</th><th>Direction</th><th>Domain</th><th>Redundancy</th><th>Essences</th><th>State</th><th></th></tr>
      </thead>
      <tbody>
        <tr v-for="(g, i) in groups" :key="g.uid">
          <td><strong>{{ g.label }}</strong><div class="note">{{ g.uid }}</div></td>
          <td>{{ g.direction }}</td>
          <td>{{ g.domain }}</td>
          <td>{{ g.redundancy ? "2022-7" : "single" }}</td>
          <td>{{ g.video?.length || 0 }} V · {{ g.audio?.length || 0 }} A · {{ g.anc?.length || 0 }} ANC</td>
          <td>
            <Pill v-if="g.enabled === false" text="disabled" kind="neutral" />
            <template v-else>
              <Pill v-for="e in runtime(g.uid)?.essences || []" :key="e.uid" :text="e.state" :kind="STATE_KIND[e.state]" :title="e.label" style="margin-right: 0.2rem" />
            </template>
          </td>
          <td style="white-space: nowrap">
            <button class="btn small secondary" @click="edit(i)">Edit</button>
            <button class="btn small secondary" @click="duplicate(g)">Duplicate</button>
            <button class="btn small danger" @click="remove(g)">Delete</button>
          </td>
        </tr>
        <tr v-if="!groups.length"><td colspan="7" class="muted">No groups yet.</td></tr>
      </tbody>
    </table>
  </div>

  <div class="panel" v-if="editing">
    <h3>Edit group {{ editing.group.label }}</h3>
    <p class="note">
      Saving tears down and rebuilds only this group (§9.3); its NMOS resources are re-registered with the same IDs. Network defaults and read offsets apply live.
      A format change mints a new MXL flow id.
    </p>
    <div class="grid fields">
      <div><label>Label</label><input v-model="editing.group.label" /><div class="err-text">{{ err("/label") }}</div></div>
      <div><label>Direction</label><select v-model="editing.group.direction"><option>ingest</option><option>egress</option></select></div>
      <div><label>MXL domain</label><select v-model="editing.group.domain"><option v-for="d in domains" :key="d">{{ d }}</option></select><div class="err-text">{{ err("/domain") }}</div></div>
      <div><label>ST 2022-7 redundancy</label><select v-model="editing.group.redundancy"><option :value="false">off</option><option :value="true">on</option></select><div class="err-text">{{ err("/redundancy") }}</div></div>
      <div><label>Enabled</label><select v-model="editing.group.enabled"><option :value="true">yes</option><option :value="false">no</option></select></div>
      <template v-if="editing.group.direction === 'egress'">
        <div><label>Output delay (ns, empty = 2 grains)</label><input :value="editing.group.output_delay_ns ?? ''" @input="editing.group.output_delay_ns = $event.target.value === '' ? null : Number($event.target.value)" /><div class="err-text">{{ err("/output_delay_ns") }}</div></div>
        <div><label>Missing data</label><select v-model="editing.group.missing_data"><option>black</option><option>repeat</option></select></div>
      </template>
    </div>

    <template v-for="type in ['video', 'audio', 'anc']" :key="type">
      <h2>{{ type === "anc" ? "ANC" : type[0].toUpperCase() + type.slice(1) }} <button class="btn small secondary" @click="addEssence(type)">+ add</button></h2>
      <div v-for="(e, i) in editing.group[type] || []" :key="e.uid || i" class="essence">
        <div class="grid fields">
          <div><label>Label</label><input v-model="e.label" /><div class="err-text">{{ err(`/${type}/${i}/label`) }}</div></div>
          <template v-if="type === 'video'">
            <div><label>Size</label>
              <select :value="`${e.width}x${e.height}`" @change="[e.width, e.height] = $event.target.value.split('x').map(Number)">
                <option>1920x1080</option><option>3840x2160</option>
              </select><div class="err-text">{{ err(`/${type}/${i}/width`) || err(`/${type}/${i}/height`) }}</div>
            </div>
            <div><label>Rate</label><select v-model="e.rate"><option v-for="r in RATES" :key="r" :value="rateValue(r)">{{ r }}</option></select><div class="err-text">{{ err(`/${type}/${i}/rate`) }}</div></div>
            <div><label>Scan</label><select v-model="e.interlace"><option>progressive</option><option>interlaced_tff</option><option>interlaced_bff</option></select><div class="err-text">{{ err(`/${type}/${i}/interlace`) }}</div></div>
            <div><label>Colorimetry</label><select v-model="e.colorimetry"><option>BT709</option><option>BT2020</option></select></div>
            <div><label>TCS</label><select v-model="e.tcs"><option>SDR</option><option>PQ</option><option>HLG</option></select></div>
            <div><label>Packing</label><select v-model="e.packing"><option>BPM</option><option>GPM</option><option>GPM_SL</option></select></div>
            <div v-if="editing.group.direction === 'egress'"><label>Pacing</label><select v-model="e.pacing"><option>narrow</option><option>linear</option><option>wide</option></select></div>
          </template>
          <template v-if="type === 'audio'">
            <div><label>Channels</label><input type="number" :value="e.channels" @input="num(e, 'channels', $event.target.value)" /><div class="err-text">{{ err(`/${type}/${i}/channels`) }}</div></div>
            <div><label>Bit depth</label><select v-model.number="e.bit_depth"><option :value="24">L24</option><option :value="16">L16</option></select></div>
            <div><label>Packet time</label><select v-model.number="e.ptime_us"><option :value="1000">1 ms</option><option :value="125">125 µs</option></select><div class="err-text">{{ err(`/${type}/${i}/ptime_us`) }}</div></div>
            <div><label>Block (µs)</label><input type="number" :value="e.block_us" @input="num(e, 'block_us', $event.target.value)" /><div class="err-text">{{ err(`/${type}/${i}/block_us`) }}</div></div>
          </template>
          <div><label>Payload type</label><input type="number" :value="e.payload_type" @input="num(e, 'payload_type', $event.target.value)" /><div class="err-text">{{ err(`/${type}/${i}/payload_type`) }}</div></div>
          <div v-if="editing.group.direction === 'egress'"><label>Read offset (grains)</label><input type="number" :value="e.read_offset_grains ?? ''" @input="num(e, 'read_offset_grains', $event.target.value)" /><div class="err-text">{{ err(`/${type}/${i}/read_offset_grains`) }}</div></div>
        </div>
        <div class="row">
          <div v-for="(leg, li) in legsOf(e)" :key="li">
            <label>Leg {{ li ? "R" : "P" }} {{ editing.group.direction === "ingest" ? "multicast / SSM source / port" : "destination / port" }}</label>
            <div class="row">
              <input v-model="leg.multicast" placeholder="239.x.x.x" />
              <input v-if="editing.group.direction === 'ingest'" v-model="leg.source" placeholder="source (optional)" />
              <input v-model="leg.port" type="number" style="max-width: 6rem" />
            </div>
            <div class="err-text">{{ err(`/${type}/${i}/defaults`) }}</div>
          </div>
        </div>
        <div class="actions"><button class="btn small danger" @click="editing.group[type].splice(i, 1)">Remove essence</button></div>
      </div>
    </template>
    <div class="err-text" v-if="editErrors[''] || editErrors[prefix()]">{{ editErrors[""] || editErrors[prefix()] }}</div>
    <div class="actions">
      <button class="btn secondary" @click="editing = null">Cancel</button>
      <button class="btn" @click="submitEdit">Save group</button>
    </div>
  </div>

  <dialog ref="createDialog">
    <template v-if="creating">
      <h3>Create group</h3>
      <label>Label</label><input v-model="creating.label" placeholder="CAM 1" />
      <div class="err-text">{{ createErrors["/label"] || createErrors[""] }}</div>
      <div class="row">
        <div><label>Direction</label><select v-model="creating.direction"><option value="ingest">ingest (2110 → MXL)</option><option value="egress">egress (MXL → 2110)</option></select></div>
        <div><label>MXL domain</label><select v-model="creating.domain"><option v-for="d in domains" :key="d">{{ d }}</option></select></div>
      </div>
      <label><input type="checkbox" v-model="creating.redundancy" /> ST 2022-7 redundancy</label>
      <div class="row">
        <div v-for="t in ['video', 'audio', 'anc']" :key="t">
          <label>{{ t === "anc" ? "ANC" : t }}</label>
          <div class="stepper">
            <button class="btn small secondary" @click="creating.counts[t] = Math.max(0, creating.counts[t] - 1)">−</button>
            <span>{{ creating.counts[t] }}</span>
            <button class="btn small secondary" @click="creating.counts[t]++">+</button>
          </div>
        </div>
      </div>
      <p class="note">Essences get the default profile (1080p50 · 8 ch L24 1 ms · ANC) and fresh uids; edit them afterwards.</p>
      <div class="err-text" v-for="[p, m] in otherCreateErrors" :key="p">{{ p }} {{ m }}</div>
      <div class="actions">
        <button class="btn secondary" @click="createDialog.close()">Cancel</button>
        <button class="btn" @click="create">Create</button>
      </div>
    </template>
  </dialog>
</template>
