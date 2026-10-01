<script setup>
import { computed } from "vue";

const props = defineProps({ values: { type: Array, default: () => [] } });
const points = computed(() => {
  const v = props.values.filter((x) => x !== null && x !== undefined);
  if (v.length < 2) return "";
  const min = Math.min(...v);
  const max = Math.max(...v);
  const span = max - min || 1;
  return v.map((x, i) => `${(i / (v.length - 1)) * 100},${40 - ((x - min) / span) * 38}`).join(" ");
});
</script>

<template>
  <svg class="spark" viewBox="0 0 100 42" preserveAspectRatio="none"><polyline :points="points" /></svg>
</template>
