#!/usr/bin/env node
// Builds the HyperFrames composition index.html in one of two layouts:
//   --layout landscape  1920 x 1080 (the default; the committed file)
//   --layout portrait   1080 x 1920 (the vertical cut, built for its render)
// from src/promo.template.html, src/promo.css, src/promo.js, src/script.mjs, the overlay states
// and the device drawing. `node tools/build.mjs --check` fails when index.html is stale.
//
// What is drawn here, and from where:
// - the overlay: src/overlay-states.json (the checkout's own render order, strings and lookups:
//   capture/overlay_states.py), laid out as shell/rco/vitajpoverlay.xml and shell/overlay.cpp place it;
// - nothing of the Overlay app: its pages are Vita3K captures (assets/shots/, capture/capture_app.sh);
// - VitaShell's screens: VitaShell 81af709's layout, Default theme and strings, with the release's file;
// - the Releases page: the release's file names and sizes (src/release.json).
import { readFileSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import * as S from "../src/script.mjs";

const ROOT = join(dirname(fileURLToPath(import.meta.url)), "..");
const read = (p) => readFileSync(join(ROOT, p), "utf8");
const esc = (s) =>
  String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;");
const cps = (s) => Array.from(s);

// ---------------------------------------------------------------- the overlay
// shell/rco/vitajpoverlay.xml + shell/overlay.cpp, as promo-assets' src/stage.html lays it out:
// header from y 12 (38 px lines, at most 3), separator 19 px below it, body 17 px below that,
// hint centred at y 522; colours from core/styled.h.
const RGB = { text: "#ffffff", highlight: "#ffd24a", reading: "#9fd0ff", rank: "#8a94a6", dim: "#b8c0cc", ok: "#7cd992" };
const OV = JSON.parse(read("src/overlay-states.json")).states;
function overlayState(key, id) {
  const o = OV[key];
  if (!o) throw new Error(`no overlay state ${key}`);
  const hc = cps(o.header);
  const [a, b] = o.hl || [-1, -1];
  const head = hc
    .map((c, i) => (c === "\n" ? "\n" : i >= a && i < b ? `<span class="hl">${esc(c)}</span>` : esc(c)))
    .join("");
  const lines = Math.min(o.header.split("\n").length, 3);
  const hh = lines * 38, sep = 12 + hh + 19, body = sep + 17;
  const spans = o.body
    .map((s) => `<span class="${s.px === 27 ? "ja" : "en"}" style="color:${RGB[s.rgb] || s.rgb}">${esc(s.text)}</span>`)
    .join("");
  return (
    `<div class="ov-state" id="${id}" data-layout-allow-overlap data-layout-allow-occlusion>` +
    `<div class="ov-head" style="height:${hh}px">${head}</div>` +
    `<div class="ov-band" style="top:${sep - 17}px;height:34px"></div>` +
    `<div class="ov-sep" style="top:${sep - 1}px"></div>` +
    `<div class="ov-body" style="top:${body}px;height:${496 - body}px">${spans}</div>` +
    `<div class="ov-hint">${esc(o.hint)}</div>` +
    `</div>`
  );
}
const OV_STATES = [
  ["s1-m2-ov01-open", "ov-a1"],
  ["s1-m2-ov02-right-inflight", "ov-a2"],
  ["s1-m2-ov03-issho", "ov-a3"],
  ["s1-m2-ov04-issho-saving", "ov-a4"],
  ["s1-m2-ov05-issho-saved", "ov-a5"],
  ["s1-m2-ov06-miniiku", "ov-a6"],
  ["s1-m2-ov06b-miniiku-saving", "ov-a7"],
  ["s1-m2-ov07-miniiku-saved", "ov-a8"],
  ["s3-m1-ov03-open", "ov-b1"], // the last beat: L + R in the game, right after the install (card mining off)
];

// ---------------------------------------------------------------- the game (夕凪ステップ)
// promo-assets' src/stage.html: the text box at (34, 384) 892 x 148, the text at (40, 30) in
// Zen Maru Gothic 500 26/40 px; a continuation row of a 「…」 line is indented by one letter.
function vnText(id, s) {
  const chars = cps(s);
  let out = "";
  chars.forEach((c, i) => {
    if (c === "\n") out += "\n";
    else {
      const indent = chars[0] === "「" && i > 0 && chars[i - 1] === "\n" ? " indent" : "";
      out += `<span class="c${indent}" data-layout-allow-occlusion>${esc(c)}</span>`;
    }
  });
  return `<div class="vn-text" id="${id}">${out}</div>`;
}

// ---------------------------------------------------------------- VitaShell (81af709, Default theme)
const VS = { folder: "rgb(0,255,255)", file: "rgb(255,255,255)", focus: "rgb(0,255,0)" };
const LAYER = "data-layout-allow-overlap data-layout-allow-occlusion";
function vsRows(rows, focus) {
  return rows
    .map(([name, info, date], i) => {
      const folder = info === "Folder";
      const color = i === focus ? VS.focus : folder ? VS.folder : VS.file;
      return (
        `<div class="vs-row" style="top:${i * 23}px;color:${color}"><i class="${folder ? "fold" : "file"}"></i>` +
        `<span class="vs-name" ${LAYER}>${esc(name)}</span><span class="vs-info" ${LAYER}>${esc(info)}</span>` +
        `<span class="vs-date" ${LAYER}>${esc(date)}</span></div>`
      );
    })
    .join("");
}
const VS_DATA = [
  ["..", "Folder", "2026/10/14 22:07"],
  [S.RELEASE.vpk.name, S.RELEASE.vpk.vitashell, "2026/10/16 09:41"],
];
// The Releases page's assets: the release's files, their sizes as GitHub shows them.
const ghAssets = () =>
  S.RELEASE.assets
    .map((a) => `<div class="gh-asset"${a.name === S.RELEASE.vpk.name ? ' id="gh-vpk"' : ""}><i class="${a.name.startsWith("Source code") ? "pk zip" : "pk"}"></i>` +
      `<span class="gh-name">${esc(a.name)}</span><span class="gh-size">${esc(a.github)}</span></div>`)
    .join("\n                      ");
const ghVpkRow = S.RELEASE.assets.findIndex((a) => a.name === S.RELEASE.vpk.name);

// ---------------------------------------------------------------- captions
const words = (html) =>
  html
    .split(/(<[^>]+>)/)
    .map((part) => (part.startsWith("<") ? part : part.replace(/(\S+)(\s*)/g, '<span class="w">$1</span>$2')))
    .join("")
    .replace(/<span class="w">([^<]*?)&nbsp;/g, '<span class="w">$1&nbsp;'); // keep non-breaking groups
function captions() {
  return Object.entries(S.CAPTIONS)
    .map(([id, c]) => {
      const p = c.p ? `<p class="sub" id="${id}-p">${c.p}</p>` : "";
      // the install's steps: one at a time, as large as a caption line, under "Easy install"; then its last beat
      const steps = c.steps
        ? `<div class="steps">${S.STEPS.map((s, i) => `<div class="step" id="step${i + 1}"><b>${i + 1}</b><span>${s}</span></div>`).join("")}` +
          `<div class="step" id="step-done"><b>✓</b><span>${S.LAST_STEP}</span></div></div>`
        : "";
      return `<div class="cap" id="${id}"><h1>${words(c.h)}</h1>${p}${steps}</div>`;
    })
    .join("\n");
}

// ---------------------------------------------------------------- sounds
// The cue names' sound files (assets/audio/sfx-*.wav, promo-assets' CC0 set) and their lengths
// in seconds (promo-assets' audio/loudness.json).
const SFX_FILE = {
  click: "click", lr: "lr", dpad: "dpad", tap: "click", confirm: "confirm", pop: "pop",
  whoosh: "whoosh-in", whooshOut: "whoosh-out", swish: "swish", chime: "chime",
};
const SFX_SECONDS = {
  click: 0.115, lr: 0.151, dpad: 0.075, "whoosh-in": 0.445, "whoosh-out": 0.445, swish: 0.675, pop: 0.185,
  chime: 1.625, confirm: 0.925, "text-blip-1": 0.07, "text-blip-2": 0.07, "text-blip-3": 0.07, "text-blip-4": 0.07,
  "text-blip-5": 0.07,
};
// The game's text blips: one every second character as a line types (src/script.mjs TYPING).
function blips() {
  const out = [];
  for (const [msg, t0, per] of S.TYPING) {
    const n = cps(S.VN[msg]).filter((c) => c !== "\n").length;
    for (let i = 0; i < n; i += 2) out.push([`text-blip-${1 + ((i / 2) % 5)}`, t0 + i * per, 0.22]);
  }
  return out;
}
function sfx() {
  const cues = S.SFX.map(([name, at, vol]) => [SFX_FILE[name], at, vol]).concat(blips());
  // each sound on the first track that is free by its start (tracks 11 and up; 10 is the music)
  const ends = [];
  const track = new Map();
  [...cues.keys()].sort((a, b) => cues[a][1] - cues[b][1]).forEach((i) => {
    const [f, at] = cues[i];
    if (!(f in SFX_SECONDS)) throw new Error(`no sound ${f}`);
    let k = ends.findIndex((e) => e <= at);
    if (k < 0) k = ends.push(0) - 1;
    ends[k] = at + SFX_SECONDS[f];
    track.set(i, 11 + k);
  });
  return cues
    .map(([f, at, vol], i) =>
      `      <audio id="sfx-${i}" src="assets/audio/sfx-${f}.wav" data-start="${at.toFixed(3)}" ` +
      `data-duration="${SFX_SECONDS[f]}" data-track-index="${track.get(i)}" data-volume="${vol}"></audio>`)
    .join("\n");
}

// ---------------------------------------------------------------- hands
// A fingertip seen from above, tip up, its base fading out (an original, stylised drawing): the
// box is 200 x 620, the pad's contact point (100, 40); a soft shadow falls on the console below it.
let fingerN = 0;
function finger() {
  const k = ++fingerN;
  return (
    `<svg viewBox="0 0 200 620" width="200" height="620" aria-hidden="true">` +
    `<defs>` +
    `<linearGradient id="fs${k}" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#c88866"/>` +
    `<stop offset="0.22" stop-color="#e9b493"/><stop offset="0.55" stop-color="#f6cfb2"/>` +
    `<stop offset="0.85" stop-color="#e2a988"/><stop offset="1" stop-color="#b9785a"/></linearGradient>` +
    `<linearGradient id="fn${k}" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#fbe6de"/>` +
    `<stop offset="1" stop-color="#eab6a6"/></linearGradient>` +
    `<linearGradient id="ff${k}" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#fff"/>` +
    `<stop offset="0.45" stop-color="#fff"/><stop offset="1" stop-color="#fff" stop-opacity="0"/></linearGradient>` +
    `<mask id="fm${k}"><rect width="200" height="620" fill="url(#ff${k})"/></mask>` +
    `<filter id="fb${k}" x="-50%" y="-50%" width="200%" height="200%"><feGaussianBlur stdDeviation="13"/></filter>` +
    `</defs>` +
    `<g mask="url(#fm${k})">` +
    `<path class="f-shadow" d="M44,620 V130 C44,64 70,30 104,30 C138,30 164,64 164,130 V620 Z" fill="#000" ` +
    `opacity="0.34" filter="url(#fb${k})" transform="translate(18 30)"/>` +
    `<path d="M32,620 V128 C32,58 62,22 100,22 C138,22 168,58 168,128 V620 Z" fill="url(#fs${k})"/>` +
    `<path d="M52,300 Q100,316 148,300" fill="none" stroke="#b9785a" stroke-width="3" opacity="0.45"/>` +
    `<path d="M58,318 Q100,332 142,318" fill="none" stroke="#b9785a" stroke-width="2" opacity="0.3"/>` +
    `<path d="M60,132 C60,74 76,52 100,52 C124,52 140,74 140,132 C140,168 124,182 100,182 C76,182 60,168 60,132 Z" ` +
    `fill="url(#fn${k})" stroke="#d29c8b" stroke-width="2"/>` +
    `<path d="M74,98 C84,74 110,66 126,84" fill="none" stroke="#fff" stroke-width="7" stroke-linecap="round" opacity="0.4"/>` +
    `</g></svg>`
  );
}

// ---------------------------------------------------------------- outputs
function device() {
  return read("assets/device/vita-white.svg").replace(/<svg /, '<svg id="device-art" data-layout-ignore ');
}

// scale < 1: a cheap draft (the conductor's 10:26Z rule while the host is busy): the frame is
// scale x the film's size, and the whole film is drawn into it, scaled.
function build(layout, scale = 1) {
  const portrait = layout === "portrait";
  const fw = portrait ? 1080 : 1920, fh = portrait ? 1920 : 1080;
  const draft =
    scale === 1
      ? ""
      : `\n      #root { width: ${fw}px !important; height: ${fh}px !important; transform: scale(${scale}); transform-origin: 0 0; }`;
  const subs = {
    W: String(Math.round(fw * scale)),
    H: String(Math.round(fh * scale)),
    LAYOUT: layout,
    TITLE: `${S.LONG_NAME} promo (${portrait ? "1080x1920" : "1920x1080"})`,
    DURATION: String(S.T.total),
    CSS: read("src/promo.css") + draft,
    JS: read("src/promo.js")
      .replace("/*{{TIMES}}*/ null", JSON.stringify(S.T))
      .replace("/*{{CUES}}*/ null", JSON.stringify(S.C))
      .replace("/*{{CAPS}}*/ null", JSON.stringify(S.CAPTIONS))
      .replace("/*{{GHROW}}*/ 0", String(ghVpkRow)),
    DEVICE_SVG: device(),
    CAPTIONS: captions(),
    VN_NAME: esc(S.VN.name),
    VN_M1: vnText("vn-m1", S.VN.m1),
    VN_M2: vnText("vn-m2", S.VN.m2),
    VN_M3: vnText("vn-m3", S.VN.m3),
    VN_M4: vnText("vn-m4", S.VN.m4),
    VN_TITLE: esc(S.VN.title),
    OVERLAY_STATES: OV_STATES.map(([k, id]) => overlayState(k, id)).join("\n"),
    VS_DATA_E: vsRows(VS_DATA.slice(0, 1), 0),
    VS_DATA_0: vsRows(VS_DATA, 0),
    VS_DATA_1: vsRows(VS_DATA, 1),
    VPK: esc(S.RELEASE.vpk.name),
    GH_ASSETS: ghAssets(),
    GH_COUNT: String(S.RELEASE.assets.length),
    VERSION: esc(S.RELEASE.version),
    NAME: esc(S.NAME),
    LONG_NAME: esc(S.LONG_NAME),
    REPO: esc(S.REPO),
    REPO_OWNER: esc(S.REPO.split("/")[0]),
    REPO_NAME: esc(S.REPO.split("/")[1]),
    ENDCARD_FINE: esc(S.ENDCARD_FINE),
    SFX: sfx(),
  };
  fingerN = 0;
  return read("src/promo.template.html").replace(/\{\{FINGER\}\}/g, () => finger()).replace(/\{\{([A-Z_0-9]+)\}\}/g, (m, k) => {
    if (!(k in subs)) throw new Error(`unknown placeholder ${m}`);
    return subs[k];
  });
}

// One project, one index.html: the landscape film is committed; the portrait cut is built into
// the same file for its render (npm run render:vertical) and the landscape one restored after.
const arg = (name) => {
  const i = process.argv.indexOf(name);
  return i > 0 ? process.argv[i + 1] : undefined;
};
const layout = arg("--layout") || "landscape";
if (!["landscape", "portrait"].includes(layout)) throw new Error(`--layout landscape|portrait, not ${layout}`);
const scale = Number(arg("--scale") || 1);
if (!(scale > 0 && scale <= 1)) throw new Error(`--scale between 0 and 1, not ${scale}`);
const html = build(layout, scale);
const path = join(ROOT, "index.html");
if (process.argv.includes("--check")) {
  let cur = "";
  try {
    cur = readFileSync(path, "utf8");
  } catch {}
  if (cur !== html) {
    console.error("index.html is out of date: run npm run build");
    process.exit(1);
  }
} else {
  writeFileSync(path, html);
  console.log(`wrote index.html (${layout}${scale === 1 ? "" : `, scale ${scale}`}, ${html.length} bytes)`);
}
