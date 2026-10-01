// Shared configuration state: file content + ETag + provenance (§11.3 GET/PUT /api/config).
import { ref } from "vue";
import { api, clone, errorMap } from "./api.js";

export function useConfig() {
  const config = ref(null); // GET /api/config body
  const draft = ref(null); // editable copy of config.file
  const etag = ref(null);
  const errors = ref({});
  const message = ref("");
  const busy = ref(false);

  async function load() {
    const r = await api.getWithEtag("/api/config");
    config.value = r.body;
    etag.value = r.etag || r.body.etag;
    draft.value = clone(r.body.file);
    errors.value = {};
  }

  /** PUT the draft (or a given file) with If-Match; returns the server result or null. */
  async function save(file = draft.value, { force = false } = {}) {
    busy.value = true;
    message.value = "";
    errors.value = {};
    try {
      const r = await api.put(`/api/config${force ? "?force=true" : ""}`, file, etag.value);
      message.value = r.body.restart_required ? "Saved — restart required to apply." : "Saved and applied.";
      await load();
      return r.body;
    } catch (e) {
      errors.value = errorMap(e);
      message.value = e.status === 412 ? "The configuration changed meanwhile — reloaded, please re-apply your edit." : e.message;
      if (e.status === 412) await load();
      return null;
    } finally {
      busy.value = false;
    }
  }

  return { config, draft, etag, errors, message, busy, load, save };
}
