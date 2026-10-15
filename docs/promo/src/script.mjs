// The promo's words and timing in one place (read by tools/build.mjs).
// Everything shown on the console's screen is checked against the code on
// main; see README.md, "What the video claims".

// Scene starts in seconds; the music is 120 BPM (a bar is 2 s). Scene 9 (the
// OCR engine choice) was dropped by the owner (2026-10-15 15:29Z): the film
// ends with scene 8 and the end card.
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
  end: 40.6,
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

// install-vita.sh's output for one game, in order (tools/install-vita.sh,
// tools/install_ftp.sh, tools/package_release.py --verify, which both scripts
// run, and the diff tools/patch_tai_config.py makes of a stock HENkaku
// config.txt). Long lines wrap at the terminal's 80 columns; "…" stands for
// the other models and language models.
export const TERMINAL = {
  command: "bash install-vita.sh --title HSDY00001 192.168.1.42",
  lines: [
    ["ok", "Verified 62 package files"],
    ["ok", "Verified 62 package files"],
    ["dim", "backup: build/device/backup/20261015-142112-Xk3t9Q"],
    ["", "--- ur0:/tai/config.txt changes (OCR worker registered for HSDY00001):"],
    ["dim", "5a6"],
    ["hi", "> ur0:tai/VitaJPOverlay_Kernel.skprx"],
    ["dim", "7a9"],
    ["hi", "> ur0:tai/VitaJPOverlay_Shell.suprx"],
    ["dim", "15a18,20"],
    ["hi", "> "],
    ["hi", "> *HSDY00001"],
    ["hi", "> ur0:tai/VitaJPOverlay_OCR.suprx"],
    ["", "verified ur0:/tai/config.txt.vjo-bak"],
    ["", "verified ur0:/tai/VitaJPOverlay_Kernel.skprx"],
    ["", "verified ur0:/data/VitaJPOverlay/boot.txt"],
    ["", "verified ur0:/tai/VitaJPOverlay_Shell.suprx"],
    ["", "verified ur0:/tai/VitaJPOverlay_OCR.suprx"],
    ["", "verified ur0:/data/VitaJPOverlay/vitajpoverlay.rco"],
    ["", "verified ux0:/data/VitaJPOverlay/dictionaries/plaintext.vjdict"],
    ["", "verified ux0:/data/JPOverlaySettings.vpk"],
    ["hi", "In VitaShell install ux0:data/JPOverlaySettings.vpk to add or update the"],
    ["hi", "Overlay settings app."],
    ["", "verified ux0:/data/VitaJPOverlay/ocr/VOCR3_FL10_w8.vocr"],
    ["", "uploaded ux0:data/VitaJPOverlay/ocr/VOCR3_FL10_w8.vocr"],
    ["dim", "…"],
    ["", "verified ux0:/data/VitaJPOverlay/config.ini"],
    ["ok", "config.ini created"],
    ["", "verified ur0:/tai/config.txt"],
    ["ok", "Installed. Reboot the Vita, then leave it on for a minute. A boot that does"],
    ["ok", "not complete makes the next one a safe boot (no hooks, no overlay; see"],
    ["ok", "ux0:data/VitaJPOverlay/status.txt). If it fails to boot at all, hold L while"],
    ["ok", "powering on and run: tools/install_ftp.sh --uninstall 192.168.1.42"],
  ],
};

export const ENDCARD_FINE =
  "Not affiliated with or endorsed by Sony Interactive Entertainment. The game shown is an original mock-up made for this video. " +
  "Dictionary entries: JMdict (EDRDG, CC BY-SA 4.0). Card design: the Lapis note type (GPL-3.0).";

// Sound cues: [file, time, volume]; the music is under them (assets/audio).
const t = T;
export const SFX = [
  ["click", t.s2 + 0.25, 0.4],
  ["tap", t.s2 + 2.75, 0.45],
  ["click", t.s3 + 1.0, 0.4],
  ["pop", t.s3 + 1.15, 0.4],
  ["chime", t.s3 + 2.3, 0.4],
  ["whoosh", t.s4, 0.35],
  ["pop", t.s4 + 0.45, 0.4],
  ["click", t.s4b + 0.55, 0.4],
  ["chime", t.s4b + 1.5, 0.4],
  ["pop", t.s4b + 1.6, 0.4],
  ["whoosh", t.s6, 0.35],
  ["click", t.s6 + 0.3, 0.4],
  ["click", t.s6 + 1.0, 0.4],
  ["click", t.s6 + 3.4, 0.4],
  ["click", t.s6 + 3.85, 0.4],
  ["click", t.s6 + 4.4, 0.4],
  ["chime", t.s6 + 5.0, 0.4],
  ["click", t.s7 + 1.55, 0.4],
  ["click", t.s8 + 0.25, 0.4],
  ["home", t.s8 + 0.75, 0.45],
  ["whoosh", t.s8 + 1.4, 0.35],
  ["tap", t.s8 + 2.15, 0.45],
  ["tap", t.s8 + 2.85, 0.45],
  ["click", t.s8 + 3.55, 0.4],
  ["click", t.s8 + 4.05, 0.4],
  ["home", t.s8 + 4.95, 0.45],
  ["whoosh", t.s8 + 5.5, 0.35],
  ["tap", t.s8 + 6.2, 0.45],
  ["whoosh", t.end - 0.15, 0.35],
];
