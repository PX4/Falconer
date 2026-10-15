import { svelte } from "@sveltejs/vite-plugin-svelte";
import { defineConfig } from "vite";
import { falconerBackend } from "./dev/backend-plugin.ts";

export const DEV_HOST = "127.0.0.1";
export const DEV_PORT = 5173;

export default defineConfig({
  plugins: [svelte(), falconerBackend({ origin: `http://${DEV_HOST}:${DEV_PORT}` })],
  // MapLibre loads its web worker relative to its own module; pre-bundling
  // moves the module and breaks that URL.
  optimizeDeps: { exclude: ["maplibre-gl"] },
  server: {
    host: DEV_HOST,
    port: DEV_PORT,
    strictPort: true,
    // The example missions live outside app/.
    fs: { allow: [".."] },
  },
});
