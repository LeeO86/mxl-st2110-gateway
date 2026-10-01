<script setup>
// One configuration setting bound to a JSON pointer of the file draft, with its provenance
// (default / file / env:VAR, §11.2). Settings set via environment variables are read-only.
import { computed } from "vue";
import { getPath, setPath, fieldError } from "../api.js";

const props = defineProps({
  draft: { type: Object, required: true },
  pointer: { type: String, required: true },
  label: { type: String, required: true },
  type: { type: String, default: "text" }, // text / number / bool / select / list
  options: { type: Array, default: () => [] },
  provenance: { type: Object, default: () => ({}) },
  errors: { type: Object, default: () => ({}) },
  placeholder: { type: String, default: "" },
  nullable: { type: Boolean, default: false },
});

const source = computed(() => props.provenance[props.pointer] || "default");
const envVar = computed(() => (source.value.startsWith("env:") ? source.value.slice(4) : ""));
const error = computed(() => fieldError(props.errors, props.pointer));

const value = computed({
  get() {
    const v = getPath(props.draft, props.pointer);
    if (props.type === "list") return Array.isArray(v) ? v.join(", ") : "";
    if (props.type === "bool") return v === true ? "true" : v === false ? "false" : "";
    return v ?? "";
  },
  set(v) {
    let out = v;
    if (props.type === "number") out = v === "" ? undefined : Number(v);
    else if (props.type === "bool") out = v === "" ? undefined : v === "true";
    else if (props.type === "list") out = v.split(",").map((s) => s.trim()).filter(Boolean);
    if (out === undefined && props.nullable) out = null;
    setPath(props.draft, props.pointer, out);
  },
});
</script>

<template>
  <div>
    <label :title="pointer">
      {{ label }}
      <span v-if="envVar" class="badge env" :title="`set via environment variable ${envVar}`">ENV {{ envVar }}</span>
      <span v-else-if="source === 'file'" class="badge file">FILE</span>
      <span v-else class="badge default">DEFAULT</span>
    </label>
    <select v-if="type === 'select' || type === 'bool'" v-model="value" :disabled="!!envVar" :class="{ invalid: error }">
      <option value="">(default)</option>
      <option v-for="o in type === 'bool' ? ['true', 'false'] : options" :key="o" :value="o">{{ o }}</option>
    </select>
    <input v-else v-model="value" :type="type === 'number' ? 'number' : 'text'" :placeholder="placeholder" :disabled="!!envVar" :class="{ invalid: error }" />
    <div v-if="error" class="err-text">{{ error }}</div>
  </div>
</template>
