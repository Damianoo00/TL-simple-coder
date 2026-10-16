import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";
export default defineConfig({
  plugins: [react()],
  server: {
    proxy: {
      "/library": "http://127.0.0.1:8080",
      "/graphs": "http://127.0.0.1:8080",
    },
  },
  preview: {
    proxy: {
      "/library": "http://127.0.0.1:8080",
      "/graphs": "http://127.0.0.1:8080",
    },
  },
});
