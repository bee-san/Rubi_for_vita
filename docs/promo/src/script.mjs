// The promo's words and timing in one place (read by tools/build.mjs, which injects T and C into
// src/promo.js and places the sound effects). Everything on the console's screen is a capture of the
// code or is laid out from it; README.md says which and from where.
//
// The owner's order (#134; 2026-10-17 06:46Z: "show the install right at the END, and don't repeat
// ourselves"): learn Japanese for free, the L + R lookup, a card mined, the card with its screenshot,
// it only reads text when it changes, turning it off for a game, and last the install, ending on one
// short beat (then press L + R in your game), then the end card. 42.0 s at 120 BPM: a beat is 0.5 s
// (the music, tools/make_bed.py: promo-assets' bed synthesised without its 13th bar, so its second lift
// is on 29.0 s, as the .vpk lands in VitaShell, and its last chord on 39.5 s, as the end card's logo lands). The owner's note on the first cut (14:15Z: "the pressing is a
// little too fast, slow it down a tad"): every press is slower (press() and the fingers in
// src/promo.js) and no two presses come closer than 0.5 s.
import { readFileSync } from "node:fs";

// Scene starts.
export const T = {
  s1: 0, //    Learn Japanese with your PS Vita for free
  s2: 4.5, //  Look up words from the games you play, entirely offline
  s3: 9.5, //  Create flashcards of words you don't know
  s4: 14.0, // With screenshots
  s5: 17.6, // It only reads text when it changes
  s6: 22.4, // Turn it off for specific games, so it doesn't get in the way
  s7: 27.65, // Easy install ... then press L + R in your game
  end: 39.0,
  total: 42.0,
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
  mine1: 10.2,
  saved1: 10.7,
  land1: 11.45,
  right2: 11.95,
  mine2: 12.7,
  saved2: 13.2,
  land2: 13.8,
  // S4: the card in Anki.
  whipA: 13.95,
  cardOpen: 14.4,
  whipB: 17.55,
  // S5: ○ closes the overlay, × shows a new line; the app's Read text.
  pressO1: 18.35,
  pressX2: 18.95,
  typeC: 19.4,
  appIn: 20.1,
  right3: 21.05,
  // S6: in the app, a tap on its Games tab; Down, Down, × turns Ember Garden OFF.
  tapGames: 23.0,
  down1: 23.7,
  down2: 24.3,
  toggle: 25.0,
  // S7: the install: the release, VitaShell, the app's Install page, the user's restart, then L + R.
  laptopIn: 27.6,
  clickVpk: 28.05,
  fileFly: 28.45,
  shellIn: 29.0,
  vsDown: 29.35, // VitaShell opens on "..": Down to the file
  vsX1: 29.85,
  vsX2: 30.4,
  vsX3: 30.95,
  vsDone: 31.55,
  ps1: 31.8, // PS: VitaShell's LiveArea (it stays open: Continue)
  flick1: 32.3, // a flick to the home screen, with the app's new bubble
  tapBubbleApp: 32.85,
  tapStartApp: 33.35,
  installX: 33.9,
  installDone: 34.5,
  // the user restarts the Vita (the app never does), as the console's guide says: hold the power button
  // (two seconds; drawn shorter), select [Power Off] on the screen that appears; then the film cuts to it on
  powerHold: 35.25,
  powerScreen: 36.05,
  tapPowerOff: 36.55,
  dark: 36.65,
  // the Vita on again (a cut, at the whip's fastest), in the game (its line already shown): L + R, the
  // overlay opens as the camera pulls out to the end card (one short beat; no second lookup)
  boot: 37.25,
  lrEnd: 38.0,
  ovEnd: 38.1,
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
  cap2: { h: "Look up words from the games you play, <em>entirely offline</em>", in: 4.55, out: 9.3 },
  cap3: { h: "Create flashcards of <em>words you don't know</em>", in: 9.6, out: 13.75 },
  cap4: { h: "With <em>screenshots</em>", in: 14.1, out: 17.5 },
  cap5: { h: "It only reads text <em>when it changes</em>", p: "or only when you press L&nbsp;+&nbsp;R", pIn: 19.85, in: 17.8, out: 22.2 },
  cap6: { h: "Turn it off for specific games, <em>so it doesn't get in the way</em>", in: 22.5, out: 27.4 },
  cap7: { h: "<em>Easy install</em>", steps: true, in: 27.75, out: 38.85 },
};

// The install's steps, shown under its caption (the one-VPK install: docs/one-vpk-install.md, the README's
// Install; promo-final-flow.txt), and its last beat (the owner, 06:46Z: "then press L + R in your game").
export const STEPS = [
  "Download the <em>.vpk</em>",
  "Install it in <em>VitaShell</em>",
  `Open <em>${NAME}</em>: Install, restart`,
];
export const LAST_STEP = "Then press <em>L&nbsp;+&nbsp;R</em> in your game";

// The mock visual novel 夕凪ステップ (original; promo-assets' vn/). Every line is read exactly by
// main's own OCR (promo-assets lookups/ocr-check.md); the looked-up words are JMdict entries.
export const VN = {
  title: "夕凪ステップ",
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
const FILES = [relName("0.8.0.vpk"), relName("0.8.0-debug.vpk"), relName("0.8.0.zip"), relName("0.8.0-debug.zip"),
  { name: "SHA256SUMS", ...REL.SHA256SUMS }];
export const RELEASE = {
  version: "0.8.0",
  vpk: FILES[0], // the main download (the README's Install)
  // the Releases page as GitHub lists a release's assets: its files by name (case-insensitive), then the
  // two source archives it adds (no size shown)
  assets: [...FILES]
    .sort((a, b) => (a.name.toLowerCase() < b.name.toLowerCase() ? -1 : 1))
    .concat([{ name: "Source code (zip)", github: "" }, { name: "Source code (tar.gz)", github: "" }]),
};

export const ENDCARD_FINE =
  "Not affiliated with or endorsed by Sony Interactive Entertainment. The game shown is an original mock-up made for this video. " +
  "Dictionary entries: JMdict (EDRDG, CC BY-SA 4.0); kanji lines: Bee's Ultimate Kanji Dictionary (Jiten, KANJIDIC2; CC BY-SA 4.0). " +
  "Frequency ranks: the Jiten frequency list (CC BY-SA 4.0). Card design: the Lapis note type (GPL-3.0).";

// The lines as they type: [message, start, seconds per character] (promo.js types them so).
export const TYPING = [
  ["m1", C.typeA, 0.045],
  ["m2", C.typeB, 0.045],
  ["m3", C.typeC, 0.05],
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
  ["tap", C.tapGames, 0.55],
  ["dpad", C.down1, 0.45],
  ["dpad", C.down2, 0.45],
  ["click", C.toggle, 0.5],
  ["pop", C.toggle + 0.05, 0.3],
  ["whoosh", C.laptopIn - 0.1, 0.35],
  ["tap", C.clickVpk, 0.55],
  ["whoosh", C.fileFly, 0.3],
  ["dpad", C.vsDown, 0.45],
  ["click", C.vsX1, 0.5],
  ["click", C.vsX2, 0.5],
  ["click", C.vsX3, 0.5],
  ["chime", C.vsDone, 0.3],
  ["click", C.ps1, 0.5],
  ["swish", C.flick1, 0.3],
  ["tap", C.tapBubbleApp, 0.55],
  ["tap", C.tapStartApp, 0.55],
  ["click", C.installX, 0.5],
  ["chime", C.installDone, 0.35],
  ["click", C.powerHold, 0.5],
  ["pop", C.powerScreen, 0.3],
  ["tap", C.tapPowerOff, 0.55],
  ["whoosh", C.boot - 0.3, 0.35],
  ["lr", C.lrEnd, 0.6],
  ["whooshOut", T.end - 0.2, 0.35],
];
