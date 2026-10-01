import { defineConfig } from "vite";
import vue from "@vitejs/plugin-vue";
import { viteSingleFile } from "vite-plugin-singlefile";

// Everything is inlined into dist/index.html, which the binary embeds (cmake/WebUi.cmake, §11.1).
export default defineConfig({
  base: "/admin/",
  plugins: [vue(), viteSingleFile()],
  server: {
    // `npm run dev` against a gateway on localhost:8080.
    proxy: {
      "/api": "http://127.0.0.1:8080",
      "/metrics": "http://127.0.0.1:8080",
      "/readyz": "http://127.0.0.1:8080",
      "/x-nmos": "http://127.0.0.1:8080",
    },
  },
  test: {
    environment: "node",
  },
});
