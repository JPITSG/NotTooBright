import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";
import { viteSingleFile } from "vite-plugin-singlefile";
import packageMetadata from "./package.json";

export default defineConfig({
  plugins: [react(), viteSingleFile()],
  define: {
    __APP_VERSION__: JSON.stringify(packageMetadata.version),
  },
  build: {
    outDir: "dist",
    assetsInlineLimit: Infinity,
  },
});
