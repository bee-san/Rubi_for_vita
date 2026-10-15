#!/usr/bin/env node
// Builds the HyperFrames composition index.html in one of two layouts:
//   --layout landscape  1920 x 1080 (the default; this is the committed file)
//   --layout portrait   1080 x 1920 (the vertical cut, built for its render)
// from src/promo.template.html, src/promo.css, src/promo.js, the device SVG and
// the Overlay app pages below. The outputs are committed; edit the sources and
// run `npm run build` (node tools/build.mjs). `node tools/build.mjs --check`
// fails when a committed output is out of date.
//
// The Overlay app pages are drawn here as SVG with the same geometry, colours
// and strings as settings/main.c and settings/anki_page.c draw them with vita2d
// (text y is the baseline, as vita2d_pvf_draw_text takes it).
import { readFileSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import * as S from "../src/script.mjs";

const ROOT = join(dirname(fileURLToPath(import.meta.url)), "..");
const read = (p) => readFileSync(join(ROOT, p), "utf8");

// ---------------------------------------------------------------- the app
// settings/main.c colours
const C = {
  BG: "rgb(9,15,23)",
  CARD: "rgb(21,31,44)",
  SELECTED: "rgb(35,56,70)",
  WHITE: "rgb(238,244,255)",
  DIM: "rgb(193,207,222)",
  ACCENT: "rgb(91,220,207)",
  OFF: "rgb(244,160,167)",
};
// A western console: × enters, ○ cancels (core/buttons.h).
const ENTER = "×";
const CANCEL = "○";

const esc = (s) =>
  String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;");

class Page {
  constructor() {
    this.parts = [];
  }
  rect(x, y, w, h, fill, extra = "") {
    this.parts.push(`<rect x="${x}" y="${y}" width="${w}" height="${h}" fill="${fill}"${extra}/>`);
  }
  // vita2d_pvf_draw_text: x, baseline y, size in pixels (the em).
  text(x, y, color, px, s, anchor = "start", extra = "") {
    const a = anchor === "start" ? "" : ` text-anchor="${anchor}"`;
    this.parts.push(`<text x="${x}" y="${y}" fill="${color}" font-size="${px}"${a}${extra} xml:space="preserve">${esc(s)}</text>`);
  }
  centered(x, y, w, color, px, s, extra = "") {
    this.text(x + w / 2, y, color, px, s, "middle", extra);
  }
  raw(s) {
    this.parts.push(s);
  }
  // wrapped_text(): rows of `width` px, baselines `lh` apart from y, at most
  // `lines` rows. The browser wraps the words (vita2d's layout breaks at spaces
  // too, settings/text_layout.c). Noto Sans JP: ascent 1.16 em, descent 0.288 em.
  wrapped(x, y, width, color, px, lh, lines, s) {
    const top = y - ((lh - 1.448 * px) / 2 + 1.16 * px);
    this.parts.push(
      `<foreignObject x="${x}" y="${top.toFixed(2)}" width="${width}" height="${lh * lines}">` +
        `<div xmlns="http://www.w3.org/1999/xhtml" class="wrap" style="color:${color};font-size:${px}px;` +
        `line-height:${lh}px;max-height:${lh * lines}px">${esc(s)}</div></foreignObject>`,
    );
  }
  svg(id, cls = "") {
    return (
      `<svg id="${id}" class="scr app-page ${cls}" viewBox="0 0 960 544" width="960" height="544" ` +
      `xmlns="http://www.w3.org/2000/svg">` +
      `<rect width="960" height="544" fill="${C.BG}"/>` +
      this.parts.join("") +
      `</svg>`
    );
  }
}

const PAGES = ["Games", "OCR & lookup", "Anki"];
const TAB_X = (i) => 24 + i * 304;

function header(p, page) {
  p.rect(0, 0, 960, 4, C.ACCENT);
  p.text(24, 35, C.WHITE, 32, "Overlay");
  p.text(744, 34, C.DIM, 24, "Select: page");
  PAGES.forEach((t, i) => {
    p.rect(TAB_X(i), 52, 296, 48, page === i ? C.ACCENT : C.CARD);
    p.centered(TAB_X(i), 52 + 33, 296, page === i ? C.BG : C.WHITE, 28, t);
  });
}

function focusCard(p, y, h, focus) {
  p.rect(24, y, 912, h, focus ? C.SELECTED : C.CARD);
  if (focus) {
    p.rect(24, y, 4, h, C.ACCENT);
    p.rect(24, y, 912, 2, C.ACCENT);
    p.rect(24, y + h - 2, 912, 2, C.ACCENT);
    p.rect(932, y, 4, h, C.ACCENT);
  }
}

function badge(p, x, y, w, h, label, active) {
  p.rect(x, y, w, h, active ? C.ACCENT : C.SELECTED);
  p.centered(x, y + h / 2 + 9, w, active ? C.BG : C.WHITE, 26, label);
}

function statusBox(p, y, h, textY, message, error) {
  p.rect(24, y, 912, h, C.CARD);
  p.text(40, textY, error ? C.OFF : C.WHITE, 24, message);
}

// draw(): the Games page. games: [{title, id, on, icon}], three rows shown.
function gamesPage(id, { games, selected, offset = 0, defaultOn = true, filter = 0, status, rowExtra = {} }) {
  const p = new Page();
  header(p, 0);
  const filters = ["All games", "Enabled", "Disabled"];
  p.rect(24, 112, 536, 54, C.CARD);
  p.text(42, 112 + 35, C.WHITE, 24, "Default:");
  badge(p, 174, 112 + 8, 108, 38, defaultOn ? "ON" : "OFF", defaultOn);
  p.text(394, 112 + 35, C.DIM, 24, "Triangle");
  p.rect(572, 112, 364, 54, C.CARD);
  p.text(590, 112 + 35, C.WHITE, 24, filters[filter]);
  p.text(824, 112 + 35, C.DIM, 24, "Square");
  for (let r = 0; r < 3 && offset + r < games.length; r++) {
    const index = offset + r;
    const y = 174 + r * 88;
    const g = games[index];
    focusCard(p, y, 80, index === selected);
    p.raw(
      `<image href="${g.icon}" x="38" y="${y + 8}" width="64" height="64" preserveAspectRatio="xMidYMid slice"/>`,
    );
    p.text(120, y + 30, C.WHITE, 26, g.title);
    const extra = rowExtra[index] || "";
    p.raw(`<g class="game-badge" data-row="${index}"${extra}>`);
    badge(p, 788, y + 7, 128, 38, g.on ? "ON" : "OFF", g.on);
    p.raw(`</g>`);
    p.text(920, y + 70, C.DIM, 24, g.id, "end");
  }
  statusBox(p, 440, 64, 467, status, false);
  p.text(24, 535, C.DIM, 24, `${ENTER}: toggle   L/R: page   Left/Right: letter   ${CANCEL}: exit`);
  const counter = `${selected + 1}/${games.length}`;
  p.text(936, 535, C.DIM, 24, counter, "end");
  return p.svg(id);
}

// The installed titles the Games page lists (fictional games; the catalog's
// order: letters A-Z, then other characters, settings/letter.c).
const GAMES = [
  { title: "Ember Quest", id: "EMBR00001", on: true, icon: "assets/vn/icon-ember.svg" },
  { title: "Neon Drift", id: "NEON00001", on: true, icon: "assets/vn/icon-neon.svg" },
  { title: "Pocket Garden", id: "PCKG00001", on: true, icon: "assets/vn/icon-garden.svg" },
  { title: "Sky Courier", id: "SKYC00001", on: true, icon: "assets/vn/icon-courier.svg" },
  { title: "VitaShell", id: "VITASHELL", on: true, icon: "assets/vn/icon-files.svg" },
  { title: "星空ダイアリー", id: "HSDY00001", on: true, icon: "assets/vn/icon-hoshizora.svg" },
];

function appPages() {
  const before = gamesPage("app-games-a", {
    games: GAMES,
    selected: 0,
    status: "Choices save automatically. Turn OFF games that need L+R.",
  });
  const moved = gamesPage("app-games-b", {
    games: GAMES,
    selected: 1,
    status: "Choices save automatically. Turn OFF games that need L+R.",
  });
  const off = gamesPage("app-games-c", {
    games: GAMES.map((g, i) => (i === 1 ? { ...g, on: false } : g)),
    selected: 1,
    status: "Saved. Resume or restart your game to apply.",
  });
  return [before, moved, off].join("\n");
}

// The Anki page (settings/anki_page.c after the .apkg export, #118): card
// mining, Export cards, Export all cards and the status box.
function ankiPage(id, { selected = 1, busy = false, busyText = "", queued = 3, total = 3, footer }) {
  const p = new Page();
  header(p, 2);
  const MINING_Y = 112, EXPORT_Y = 188, ALL_Y = 296, CARD_H = 68, ACTION_H = 100;
  focusCard(p, MINING_Y, CARD_H, selected === 0);
  p.text(42, MINING_Y + 30, C.WHITE, 28, "Card mining");
  badge(p, 812, MINING_Y + 6, 108, 34, "ON", true);
  p.text(42, MINING_Y + 60, C.DIM, 24, `${ENTER} in the overlay saves the word to a queue on the memory card.`);
  const count = (n, what) => `${n} card${n === 1 ? "" : "s"} ${what}`;
  focusCard(p, EXPORT_Y, ACTION_H, selected === 1);
  p.text(42, EXPORT_Y + 30, C.WHITE, 28, "Export cards");
  p.text(500, EXPORT_Y + 30, C.DIM, 24, count(queued, "queued"));
  badge(p, 760, EXPORT_Y + 6, 160, 34, busy ? "Exporting…" : "Export", busy);
  p.wrapped(42, EXPORT_Y + 62, 870, C.DIM, 22, 24, 2, busy
    ? busyText
    : `A new .apkg of Lapis cards in ${S.APKG.dir.replace(/\/$/, "")}/. The queue is cleared once the file is written and checked.`);
  focusCard(p, ALL_Y, ACTION_H, selected === 2);
  p.text(42, ALL_Y + 30, C.WHITE, 28, "Export all cards");
  p.text(500, ALL_Y + 30, C.DIM, 24, count(total, "in all"));
  badge(p, 760, ALL_Y + 6, 160, 34, "Export all", false);
  p.wrapped(42, ALL_Y + 62, 870, C.DIM, 22, 24, 2,
    "Every card again, the exported ones too (a new device, a lost file): Anki adds only the notes it does not have.");
  p.rect(24, 408, 912, 92, C.CARD);
  p.wrapped(40, 435, 880, C.WHITE, 24, 28, 3, footer);
  p.text(24, 535, C.DIM, 24, busy ? `${CANCEL}: stop   Select: page` : `Up/Down: choose   ${ENTER}: select   Select: page   ${CANCEL}: exit`);
  return p.svg(id);
}
function ankiPages() {
  const copy = `Copy the .apkg from ${S.APKG.dir.replace(/\/$/, "")}/ to a computer or phone (VitaShell: FTP with Select, or USB with Start), then open it in Anki: File > Import.`;
  return [
    ankiPage("app-anki-a", { footer: copy }),
    ankiPage("app-anki-b", { busy: true, busyText: S.APKG.progress, footer: copy }),
    ankiPage("app-anki-c", { queued: 0, footer: S.APKG.result }),
  ].join("\n");
}

// The Lapis card's back (donkuri/lapis src/back.html, light theme), with the
// fields the export writes (#118).
function lapisCard() {
  return `<div class="lapis">
            <div class="lp-top">&#160;</div>
            <div class="lp-head">
              <div class="lp-vocab"><ruby>一緒<rt>いっしょ</rt></ruby>に</div>
              <div class="lp-info">いっしょに</div>
            </div>
            <div class="lp-sentence">明日も、ここで<b>一緒に</b>見ようよ。</div>
            <div class="lp-def"><small>JMdict Plain</small>together (with); at the same time; in one lump | in the same category</div>
            <div class="lp-misc">星空ダイアリー · HSDY00001</div>
            <div class="lp-tags"><span>VitaJPOverlay</span></div>
          </div>`;
}

const ICONS = {
  desktop: `<svg viewBox="0 0 120 96" width="120" height="96" fill="none" stroke="#c1cfde" stroke-width="5" stroke-linejoin="round"><rect x="6" y="4" width="108" height="66" rx="8"/><path d="M48 70 L44 88 H76 L72 70"/><rect x="40" y="18" width="40" height="38" rx="5" stroke="#ffd24a"/></svg>`,
  phone: `<svg viewBox="0 0 60 96" width="60" height="96" fill="none" stroke="#c1cfde" stroke-width="5" stroke-linejoin="round"><rect x="6" y="4" width="48" height="88" rx="10"/><rect x="16" y="26" width="28" height="34" rx="4" stroke="#ffd24a"/></svg>`,
  tablet: `<svg viewBox="0 0 96 96" width="96" height="96" fill="none" stroke="#c1cfde" stroke-width="5" stroke-linejoin="round"><rect x="6" y="6" width="84" height="84" rx="10"/><rect x="30" y="28" width="36" height="40" rx="5" stroke="#ffd24a"/></svg>`,
};

// Typewriter characters: one span per character (the game's are covered by the
// overlay's panel on purpose: data-layout-allow-occlusion).
const chars = (s, attr = "") => [...s].map((c) => `<span class="c"${attr}>${esc(c)}</span>`).join("");
const gameChars = (s) => chars(s, " data-layout-allow-occlusion");
// Caption words: each word in a span, tags kept.
const words = (html) =>
  html
    .split(/(<[^>]+>)/)
    .map((part) => (part.startsWith("<") ? part : part.replace(/(\S+)(\s*)/g, '<span class="w">$1</span>$2')))
    .join("");

function terminal() {
  const cmd = `<span class="l cmd"><span class="p">$ </span>${chars(S.TERMINAL.command)}</span>`;
  const out = S.TERMINAL.lines.map(([cls, text]) => `<span class="l out ${cls}">${esc(text)}</span>`).join("");
  return cmd + out;
}

// Sound effect lengths in seconds (assets/audio/sfx-*.wav, tools/make_audio.py).
const SFX_SECONDS = { tap: 0.07, click: 0.045, whoosh: 0.45, pop: 0.16, chime: 0.6, home: 0.38 };
function sfx() {
  return S.SFX.map(
    ([name, at, vol], i) =>
      `      <audio id="sfx-${i}" src="assets/audio/sfx-${name}.wav" data-start="${at.toFixed(2)}" ` +
      `data-duration="${SFX_SECONDS[name]}" data-track-index="${11 + (i % 3)}" data-volume="${vol}"></audio>`,
  ).join("\n");
}

// ---------------------------------------------------------------- outputs
function device() {
  // The device SVG inline (the timeline presses its buttons by id); its
  // comments are kept, the XML prolog (none) is not needed.
  // data-layout-ignore: the drawing is decoration (its only text, SELECT and
  // START, is embossed grey on white plastic, not text to read).
  return read("assets/device/vita-white.svg").replace(/<svg /, '<svg id="device-art" data-layout-ignore ');
}

function build(layout) {
  const portrait = layout === "portrait";
  const subs = {
    W: portrait ? "1080" : "1920",
    H: portrait ? "1920" : "1080",
    LAYOUT: layout,
    TITLE: portrait ? "Vita JP Overlay promo (1080x1920)" : "Vita JP Overlay promo (1920x1080)",
    CSS: read("src/promo.css"),
    JS: read("src/promo.js").replace("/*{{TIMES}}*/ null", JSON.stringify(S.T)),
    DEVICE_SVG: device(),
    APP_PAGES: appPages(),
    ANKI_PAGES: ankiPages(),
    ENTER_GLYPH: esc(S.ENTER),
    CAP4_H: S.CAP4.h,
    CAP4_P: esc(S.CAP4.p),
    LAPIS_CARD: lapisCard(),
    APKG_NAME: `${esc(S.APKG.name)}<small>${esc(S.APKG.dir)}</small>`,
    ICON_DESKTOP: ICONS.desktop,
    ICON_PHONE: ICONS.phone,
    ICON_TABLET: ICONS.tablet,
    TERMINAL: terminal(),
    VN_LINE_1: gameChars(S.VN.line1),
    VN_LINE_2: gameChars(S.VN.line2),
    VN_LINE_3: gameChars(S.VN.line3),
    OV_QUEUE_LINE_2: `<div class="en">${esc(S.OVERLAY.queueBefore)}</div>`,
    OV_QUEUE_LINE_3: `<div class="en">${esc(S.OVERLAY.queueAfter)}</div>`,
    OV_HINT: esc(S.OVERLAY.hint),
    ENDCARD_FINE: esc(S.ENDCARD_FINE),
    SFX: sfx(),
  };
  let html = read("src/promo.template.html").replace(/\{\{([A-Z_0-9]+)\}\}/g, (m, k) => {
    if (!(k in subs)) throw new Error(`unknown placeholder ${m}`);
    return subs[k];
  });
  // caption headlines: one span per word
  html = html.replace(/(<div class="cap"[^>]*>[\s\S]*?<h1>)([\s\S]*?)(<\/h1>)/g, (m, a, b, c) => a + words(b) + c);
  return html;
}

// One project, one index.html: the landscape film is committed; the portrait
// cut is built into the same file for its render (npm run render:vertical)
// and the landscape one is restored after it. HyperFrames treats every
// root-level HTML file with a composition as an entry point, so the two
// cannot sit side by side.
const arg = (name) => {
  const i = process.argv.indexOf(name);
  return i > 0 ? process.argv[i + 1] : undefined;
};
const layout = arg("--layout") || "landscape";
if (!["landscape", "portrait"].includes(layout)) throw new Error(`--layout landscape|portrait, not ${layout}`);
const outputs = { "index.html": build(layout) };
const check = process.argv.includes("--check");
let stale = 0;
for (const [name, html] of Object.entries(outputs)) {
  const path = join(ROOT, name);
  if (check) {
    let cur = "";
    try {
      cur = readFileSync(path, "utf8");
    } catch {}
    if (cur !== html) {
      console.error(`${name} is out of date: run npm run build`);
      stale++;
    }
  } else {
    writeFileSync(path, html);
    console.log(`wrote ${name} (${html.length} bytes)`);
  }
}
process.exit(stale ? 1 : 0);
