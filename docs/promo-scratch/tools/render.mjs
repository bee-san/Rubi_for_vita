// node tools/render.mjs --cut 16x9 [--from F] [--to F] [--fps 30] [--scale 1] [--out DIR] [--times t1,t2,...] [--meta]
// Renders frames of src/index.html in chrome-headless-shell (SwiftShader WebGL, no GPU, no network:
// every request is answered from this folder by request interception, so nothing listens on a port).
import puppeteer from 'puppeteer-core';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const args = Object.fromEntries(process.argv.slice(2).reduce((a, v, i, arr) => (v.startsWith('--') ? [...a, [v.slice(2), arr[i + 1] && !arr[i + 1].startsWith('--') ? arr[i + 1] : true]] : a), []));
const cut = args.cut || '16x9', fps = +(args.fps || 30);
const outDir = path.resolve(args.out || path.join(ROOT, 'out', cut));
fs.mkdirSync(outDir, { recursive: true });
const CHROME = process.env.CHROME || path.join(ROOT, '.cache/browsers/chrome-headless-shell/linux-152.0.7977.30/chrome-headless-shell-linux64/chrome-headless-shell');
const [W, H] = cut === '9x16' ? [1080, 1920] : [1920, 1080];

const browser = await puppeteer.launch({
  executablePath: CHROME, headless: 'shell', protocolTimeout: 0,
  args: ['--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist', '--no-sandbox',
    `--window-size=${W},${H}`, '--force-device-scale-factor=1', '--disable-background-timer-throttling', '--disable-renderer-backgrounding',
    '--font-render-hinting=none', '--disable-lcd-text', '--disable-features=Translate,MediaRouter', '--no-first-run', '--mute-audio'],
});
const page = await browser.newPage();
page.on('console', (m) => { if (m.type() === 'error' || m.type() === 'warn' || process.env.VERBOSE) console.log('[page]', m.type(), m.text()); });
page.on('pageerror', (e) => console.log('[pageerror]', e.message));
await page.setRequestInterception(true);
const TYPES = { '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript', '.png': 'image/png', '.jpg': 'image/jpeg', '.svg': 'image/svg+xml', '.ttf': 'font/ttf', '.json': 'application/json' };
page.on('request', (req) => {
  const u = new URL(req.url());
  if (u.host !== 'promo.local') return req.abort();
  const f = path.join(ROOT, decodeURIComponent(u.pathname));
  if (!f.startsWith(ROOT + path.sep) || !fs.existsSync(f) || fs.statSync(f).isDirectory()) return req.respond({ status: 404, body: '' });
  req.respond({ status: 200, contentType: TYPES[path.extname(f)] || 'application/octet-stream', body: fs.readFileSync(f) });
});
await page.setViewport({ width: W, height: H, deviceScaleFactor: 1 });
await page.goto('http://promo.local/src/index.html');
await page.waitForFunction('window.__ready === true', { timeout: 120000 });
const info = await page.evaluate((c, sc) => window.__setup(c, { scale: sc }), cut, +(args.scale || 1));
const duration = info.duration;
const nFrames = Math.round(duration * fps);
console.log(`cut ${cut}: ${duration} s, ${nFrames} frames; ${info.gl}`);
if (args.meta) {
  fs.writeFileSync(path.join(outDir, 'sfx.json'), JSON.stringify(await page.evaluate(() => window.__sfx()), null, 1));
  fs.writeFileSync(path.join(outDir, 'captions.json'), JSON.stringify(await page.evaluate(() => window.__captions()), null, 1));
}
let list;
if (args.times) list = String(args.times).split(',').map((s) => ({ t: +s, name: `t${(+s).toFixed(2).padStart(6, '0')}.png` }));
else {
  const from = +(args.from ?? 0), to = Math.min(+(args.to ?? nFrames), nFrames);
  list = []; for (let f = from; f < to; f++) list.push({ t: f / fps, name: `f${String(f).padStart(5, '0')}.png` });
}
const T0 = Date.now();
for (const [i, it] of list.entries()) {
  const b64 = await page.evaluate((t) => window.__frame(t), it.t);
  fs.writeFileSync(path.join(outDir, it.name), Buffer.from(b64, 'base64'));
  if (i % 30 === 0) console.log(`${it.name} ${((Date.now() - T0) / (i + 1)).toFixed(0)} ms/frame`);
}
console.log(`done: ${list.length} frames in ${((Date.now() - T0) / 1000).toFixed(1)} s`);
await browser.close();
