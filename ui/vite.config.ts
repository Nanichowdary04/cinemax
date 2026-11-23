import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';
import { viteSingleFile } from 'vite-plugin-singlefile';

// Produces ONE self-contained dist/index.html (JS + CSS inlined). The C++ host
// embeds that file with juce_add_binary_data and serves it from the WebView2
// resource provider, so the app never touches the network.
export default defineConfig({
  base: './',
  plugins: [react(), viteSingleFile({ removeViteModuleLoader: true })],
  build: {
    target: 'es2022',
    outDir: 'dist',
    emptyOutDir: true,
    cssCodeSplit: false,
    assetsInlineLimit: 100_000_000,
    chunkSizeWarningLimit: 4000,
    sourcemap: false,
  },
});
