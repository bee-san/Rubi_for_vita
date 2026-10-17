// The promo's words and timing in one place (read by tools/build.mjs, which injects T and C into
// src/promo.js and places the sound effects). Everything on the console's screen is a capture of the
// code or is laid out from it; README.md says which and from where.
//
// 44.0 s at 120 BPM: a beat is 0.5 s, a bar 2 s (the music is 22 bars). The owner's note on the
// first cut (#134, 14:15Z: "the pressing is a little too fast, slow it down a tad"): every press is
// slower (tools: press() and the fingers in src/promo.js) and no two presses come closer than 0.5 s,
// the busiest scenes (the install, turning a game off) given time from the quieter ones.
import { readFileSync } from "node:fs";

// Scene starts (the owner's script, in his order; his line 9 was dropped on 2026-10-15).
export const T = {
  s1: 0, //    Learn Japanese with your PS Vita for free
  s2: 4.5, //  Look up words from the games you play, entirely offline
  s3: 10.0, // Create flashcards of words you don't know
  s4: 14.6, // With screenshots
  s5: 19.0, // It only reads text when it changes
  s6: 23.5, // Easy install
  s7: 31.0, // Now it's installed: go to your game and press L + R to look up words.
  s8: 36.5, // Turn it off for specific games, so it doesn't get in the way
  end: 41.7,
  total: 44.0,
};

// The cue sheet: the moment of every press, tap and change (seconds). promo.js times the pictures
// from it and build.mjs the sounds, so a click is on the frame of its press.
export const C = {
  // S1: the game types its first line; × shows the next.
  typeA: 0.35,
  pressX1: 3.45,
  typeB: 3.6,
  // S2: L + R opens the overlay on また明日; Right goes to 一緒に.
  lr1: 5.0,
  ovOpen1: 5.1,
  right1: 6.75,
  // S3: × saves 一緒に (and its screenshot); Right, × saves 見に行く. "Saving card…" about 0.5 s
  // (shell/anki.c; an estimate, not measured on a console).
  mine1: 10.7,
  saved1: 11.2,
  land1: 11.95,
  right2: 12.45,
  mine2: 13.25,
  saved2: 13.75,
  land2: 14.4,
  // S4: the card in Anki.
  whipA: 14.6,
  cardOpen: 15.1,
  whipB: 18.75,
  // S5: ○ closes the overlay, × shows a new line; the Overlay app's Read text.
  pressO1: 19.55,
  pressX2: 20.15,
  typeC: 20.6,
  appIn: 21.45,
  right3: 22.35,
  // S6: the install: the release, VitaShell, the app's Install page, a restart.
  laptopIn: 23.45,
  clickVpk: 24.05,
  fileFly: 24.45,
  shellIn: 25.05,
  vsX1: 25.45,
  vsX2: 26.05,
  vsX3: 26.65,
  vsDone: 27.3,
  ps1: 27.6,
  tapBubbleApp: 28.1,
  tapStartApp: 28.6,
  installX: 29.35,
  installDone: 30.0,
  restart: 30.45,
  // S7: the game, L + R, Right.
  boot: 31.1,
  tapBubbleGame: 31.7,
  tapStartGame: 32.2,
  typeD: 32.55,
  lr2: 33.9,
  ovOpen2: 34.0,
  right4: 35.2,
  // S8: ○, PS, the home screen, the app, Games: Down, Down, × turns Ember Garden OFF.
  pressO2: 36.65,
  ps2: 37.2,
  flick: 37.75,
  tapBubbleApp2: 38.3,
  tapStartApp2: 38.8,
  down1: 39.4,
  down2: 39.9,
  toggle: 40.45,
};

// A western console: × enters, ○ cancels (core/buttons.h).
export const ENTER = "×";
export const CANCEL = "○";

// The product's name in what the film draws itself (owner, 2026-10-16 13:04Z: "Rubi", "Rubi for Vita";
// docs/rename-rubi.md): the app's bubble and LiveArea title, the install step, the end card, the address.
export const NAME = "Rubi";
export const LONG_NAME = "Rubi for Vita";
export const REPO = "bee-san/Rubi_for_vita";

// Captions: the owner's script. <em> marks the accent words.
export const CAPTIONS = {
  cap1: { h: "Learn Japanese with your PS&nbsp;Vita <em>for free</em>", in: 0.6, out: 4.3 },
  cap2: { h: "Look up words from the games you play, <em>entirely offline</em>", in: 4.55, out: 9.8 },
  cap3: { h: "Create flashcards of <em>words you don't know</em>", in: 10.05, out: 14.4 },
  cap4: { h: "With <em>screenshots</em>", in: 14.7, out: 18.75 },
  cap5: { h: "It only reads text <em>when it changes</em>", p: "or only when you press L&nbsp;+&nbsp;R", pIn: 21.85, in: 19.1, out: 23.3 },
  cap6: { h: "<em>Easy install</em>", in: 23.6, out: 31.25 },
  cap7: { h: "Now it's installed: go to your game and press <em>L&nbsp;+&nbsp;R</em> to look up words.", in: 31.55, out: 36.45 },
  cap8: { h: "Turn it off for specific games, <em>so it doesn't get in the way</em>", in: 36.75, out: 41.5 },
};

// Scene 6's steps, shown under its caption (the one-VPK install: docs/one-vpk-install.md, the README's
// Install; promo-final-flow.txt).
export const STEPS = [
  "Download the <em>.vpk</em>",
  "Install it in <em>VitaShell</em>",
  `Open <em>${NAME}</em>: Install, restart`,
];

// The mock visual novel 夕凪ステップ (original; promo-assets' vn/). Every line is read exactly by
// main's own OCR (promo-assets lookups/ocr-check.md); the looked-up words are JMdict entries.
export const VN = {
  title: "夕凪ステップ",
  titleEn: "Yuunagi Step",
  name: "ひなた",
  m1: "「ねえ、聞いて！明日の夜、丘の上で\n流れ星が見えるんだって！」",
  m2: "「また明日、一緒に見に行こう！」",
  m3: "「ドキドキ、ワクワク……！」",
  m4: "「キラキラ……。\n子供の頃みたい。」",
};

// The games on the home screen and in the app's list (capture/make_promo_games.py: original titles and
// icons, installed on the capture's card).
export const GAMES = {
  PROMO0001: "夕凪ステップ",
  PROMO0002: "Neon Drift",
  PROMO0003: "Courier Run",
  PROMO0004: "Ember Garden",
  PROMO0005: "Hoshizora Diary",
};

// The release: the files package_variants.py makes of the checkout the film shows (src/release.json,
// written by tools/prep_assets.py from capture/run.sh), in the order the README's Install lists them.
// Their sizes are what the Releases page and VitaShell show for them (both 1024-based: GitHub one
// decimal, VitaShell's getSizeString() two). Named as after the rename (docs/rename-rubi.md:
// Rubi-<version>.vpk); a checkout from before it names them VitaJPOverlay-<version>.
const REL = JSON.parse(readFileSync(new URL("./release.json", import.meta.url), "utf8")).files;
const relName = (suffix) => {
  const k = Object.keys(REL).find((n) => n.endsWith(suffix) && (suffix.includes("debug") || !n.includes("-debug")));
  if (!k) throw new Error(`release.json has no *${suffix}`);
  return { name: k.replace(/^VitaJPOverlay-/, `${NAME}-`), ...REL[k] };
};
export const RELEASE = {
  version: "0.8.0",
  assets: [relName("0.8.0.vpk"), relName("0.8.0-debug.vpk"), relName("0.8.0.zip"), relName("0.8.0-debug.zip"),
    { name: "SHA256SUMS", ...REL.SHA256SUMS }],
};
RELEASE.vpk = RELEASE.assets[0];

export const ENDCARD_FINE =
  "Not affiliated with or endorsed by Sony Interactive Entertainment. The game shown is an original mock-up made for this video. " +
  "Dictionary entries: JMdict (EDRDG, CC BY-SA 4.0); kanji lines: Bee's Ultimate Kanji Dictionary (Jiten, KANJIDIC2; CC BY-SA 4.0). " +
  "Frequency ranks: the Jiten frequency list (CC BY-SA 4.0). Card design: the Lapis note type (GPL-3.0).";

// The lines as they type: [message, start, seconds per character] (promo.js types them so).
export const TYPING = [
  ["m1", C.typeA, 0.045],
  ["m2", C.typeB, 0.045],
  ["m3", C.typeC, 0.05],
  ["m4", C.typeD, 0.045],
];

// Sound cues: [name, time, volume]. The times are the cue sheet's; the music runs under them.
export const SFX = [
  ["click", C.pressX1, 0.5],
  ["lr", C.lr1, 0.6],
  ["dpad", C.right1, 0.45],
  ["click", C.mine1, 0.5],
  ["confirm", C.saved1, 0.4],
  ["pop", C.land1, 0.45],
  ["dpad", C.right2, 0.45],
  ["click", C.mine2, 0.5],
  ["confirm", C.saved2, 0.4],
  ["pop", C.land2, 0.45],
  ["whoosh", C.whipA - 0.1, 0.4],
  ["chime", C.cardOpen + 0.1, 0.35],
  ["whooshOut", C.whipB - 0.1, 0.4],
  ["click", C.pressO1, 0.5],
  ["click", C.pressX2, 0.5],
  ["swish", C.appIn - 0.05, 0.35],
  ["dpad", C.right3, 0.45],
  ["whoosh", C.laptopIn - 0.1, 0.35],
  ["tap", C.clickVpk, 0.55],
  ["whoosh", C.fileFly, 0.3],
  ["click", C.vsX1, 0.5],
  ["click", C.vsX2, 0.5],
  ["click", C.vsX3, 0.5],
  ["chime", C.vsDone, 0.3],
  ["click", C.ps1, 0.5],
  ["tap", C.tapBubbleApp, 0.55],
  ["tap", C.tapStartApp, 0.55],
  ["click", C.installX, 0.5],
  ["chime", C.installDone, 0.35],
  ["tap", C.tapBubbleGame, 0.55],
  ["tap", C.tapStartGame, 0.55],
  ["lr", C.lr2, 0.6],
  ["dpad", C.right4, 0.45],
  ["click", C.pressO2, 0.5],
  ["click", C.ps2, 0.5],
  ["swish", C.flick, 0.3],
  ["tap", C.tapBubbleApp2, 0.55],
  ["tap", C.tapStartApp2, 0.55],
  ["dpad", C.down1, 0.45],
  ["dpad", C.down2, 0.45],
  ["click", C.toggle, 0.5],
  ["pop", C.toggle + 0.05, 0.3],
  ["whooshOut", T.end - 0.2, 0.35],
];
