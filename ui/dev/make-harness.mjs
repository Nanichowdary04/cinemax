// DEV ONLY — never shipped, never imported by src/.
// Produces dev-out/harness.html: the production bundle plus a stub
// window.__JUCE__ that serves STATIC sample state so the layout can be
// checked in a normal browser. A banner marks it "LAYOUT TEST — NOT REAL AUDIO".
//
// Query parameters (for checking states):
//   ?status=running|off|no-virtual-device|no-output-device|error|starting
//   ?nodevices=1   no output devices
//   ?quiet=1       engine running but no audio flowing
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const dist = join(root, 'dist', 'index.html');
const outDir = join(root, 'dev-out');
const stub = readFileSync(join(root, 'dev', 'harness-stub.js'), 'utf8');

let html = readFileSync(dist, 'utf8');
// Allow the inline stub (the bundle's CSP already permits inline scripts).
html = html.replace('<head>', `<head>\n<script>\n${stub}\n</script>`);
mkdirSync(outDir, { recursive: true });
const out = join(outDir, 'harness.html');
writeFileSync(out, html);
console.log(out);
