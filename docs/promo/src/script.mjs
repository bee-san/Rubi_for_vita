// The promo's words and timing in one place (read by tools/build.mjs).
// Everything shown on the console's screen is checked against the code on
// main; see README.md, "What the video claims".

// Scene starts in seconds; the music is 120 BPM (a bar is 2 s).
export const T = {
  s1: 0,
  s2: 4,
  s3: 9,
  s4: 13.5,
  s4b: 16.5,
  s5: 20,
  s6: 24,
  s7: 29.5,
  s8: 33.5,
  s9: 39,
  end: 42,
  total: 44,
};

// A western console: × enters, ○ cancels (core/buttons.h).
export const ENTER = "×";

// The mock visual novel's dialogue (original text). Lines 1-2 are what the
// overlay reads; the looked-up words resolve to JMdict entries in the bundled
// plaintext.vjdict (checked with the real dictionary and core/local_dict.c).
export const VN = {
  line1: "「キラキラ光る星、本当にきれいだね。",
  line2: "　明日も、ここで一緒に見ようよ。」",
  line3: "「流れ星が見えたら、願い事しようね。」",
};

// Scene 4: what a mined card is (set from the merged .apkg export, #118).
export const CAP4 = {
  h: "Export them as <em>Lapis</em> cards for Anki",
  p: "with the word, its reading, the sentence and the definition",
};

// The overlay's body lines that depend on Anki mining (shell/overlay.cpp).
export const OVERLAY = {
  queueBefore: "Anki queue: 2 · △ send",
  queueAfter: "Anki queue: 3 · △ send",
  hint: `Tap / ◀▶▲▼ word · ${ENTER} queue · △ send · stick scroll · □ region · ○ close`,
};

export const APKG = {
  dir: "ux0:data/VitaJPOverlay/anki/",
  name: "2026-10-15_142530-3cards.apkg",
  progress: "Writing 3 cards…",
  result: "Saved 2026-10-15_142530-3cards.apkg (3 cards). Copy it from ux0:data/VitaJPOverlay/anki/ to a computer or phone (VitaShell: FTP or USB), then open it in Anki.",
};

// install-vita.sh's output for one game (tools/install-vita.sh,
// tools/install_ftp.sh, tools/patch_tai_config.py), shortened with "…".
export const TERMINAL = {
  command: "bash install-vita.sh --title HSDY00001 192.168.1.42",
  lines: [
    ["ok", "Verified 62 package files"],
    ["dim", "backup: build/device/backup/20261015-142112"],
    ["", "--- ur0:/tai/config.txt changes (OCR worker"],
    ["", "    registered for HSDY00001):"],
    ["dim", "> *HSDY00001"],
    ["dim", "> ur0:tai/VitaJPOverlay_OCR.suprx"],
    ["", "verified ur0:/tai/VitaJPOverlay_Kernel.skprx"],
    ["", "verified ur0:/tai/VitaJPOverlay_Shell.suprx"],
    ["", "verified ur0:/tai/VitaJPOverlay_OCR.suprx"],
    ["", "verified ux0:/data/VitaJPOverlay/dictionaries/plaintext.vjdict"],
    ["", "verified ux0:/data/JPOverlaySettings.vpk"],
    ["hi", "In VitaShell install ux0:data/JPOverlaySettings.vpk"],
    ["hi", "to add or update the Overlay settings app."],
    ["dim", "uploaded ux0:data/VitaJPOverlay/ocr/VOCR3_H15_w8.vocr …"],
    ["ok", "config.ini created"],
    ["ok", "Installed. Reboot the Vita, then leave it on for a minute."],
  ],
};

export const ENDCARD_FINE =
  "Not affiliated with or endorsed by Sony Interactive Entertainment. The game shown is an original mock-up made for this video. " +
  "Dictionary entries: JMdict (EDRDG, CC BY-SA 4.0). Card design: the Lapis note type (GPL-3.0).";

// Sound cues: [file, time, volume]; the music is under them (assets/audio).
const t = T;
export const SFX = [
  ["click", t.s2 + 0.25, 1.2],
  ["tap", t.s2 + 2.75, 1.1],
  ["click", t.s3 + 1.0, 1.2],
  ["pop", t.s3 + 1.15, 0.5],
  ["chime", t.s3 + 2.3, 0.5],
  ["whoosh", t.s4, 0.45],
  ["pop", t.s4 + 0.45, 0.5],
  ["click", t.s4b + 0.55, 1.2],
  ["chime", t.s4b + 1.5, 0.5],
  ["pop", t.s4b + 1.6, 0.5],
  ["whoosh", t.s6, 0.45],
  ["click", t.s6 + 3.75, 1.2],
  ["chime", t.s6 + 4.55, 0.5],
  ["click", t.s7 + 1.55, 1.2],
  ["home", t.s8 + 0.3, 0.55],
  ["tap", t.s8 + 1.2, 1.1],
  ["tap", t.s8 + 2.0, 1.1],
  ["click", t.s8 + 2.65, 1.2],
  ["click", t.s8 + 3.15, 1.2],
  ["home", t.s8 + 4.0, 0.55],
  ["tap", t.s8 + 4.65, 1.1],
  ["tap", t.s9 + 1.05, 1.1],
  ["whoosh", t.end - 0.15, 0.45],
];
