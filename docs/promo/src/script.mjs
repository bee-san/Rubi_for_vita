// The promo's words and timing in one place (read by tools/build.mjs, which injects T and C into
// src/promo.js and places the sound effects). Everything on the console's screen is checked
// against the code; README.md says what each shot shows and what is a mock-up.
//
// 44.0 s at 120 BPM: a beat is 0.5 s, a bar 2 s (the music is 22 bars).

// Scene starts (the owner's script, in his order; his line 9 was dropped on 2026-10-15).
export const T = {
  s1: 0, //    Learn Japanese with your PS Vita for free
  s2: 4.5, //  Look up words from the games you play, entirely offline
  s3: 10.0, // Create flashcards of words you don't know
  s4: 14.5, // With screenshots
  s5: 19.0, // It only reads text when it changes
  s6: 23.5, // Easy install
  s7: 31.0, // Now it's installed: go to your game and press L + R to look up words.
  s8: 37.0, // Turn it off for specific games, so it doesn't get in the way
  end: 41.5,
  total: 44.0,
};

// The cue sheet: the moment of every press, tap and change (seconds). promo.js times the pictures
// from it and build.mjs the sounds, so a click is on the frame of its press.
export const C = {
  // S1: the game types its first line; × shows the next.
  typeA: 0.35,
  pressX1: 3.5,
  typeB: 3.62,
  // S2: L + R opens the overlay on また明日; Right goes to 一緒に.
  lr1: 5.0,
  ovOpen1: 5.1,
  right1: 6.6,
  // S3: × saves 一緒に (and its screenshot); Right, × saves 見に行く.
  mine1: 10.75,
  saved1: 11.25,
  land1: 12.0,
  right2: 12.25,
  mine2: 12.9,
  saved2: 13.35,
  land2: 14.05,
  // S4: the card in Anki.
  whipA: 14.45,
  cardOpen: 14.95,
  whipB: 18.55,
  // S5: ○ closes the overlay, × shows a new line; the Overlay app's Read text.
  pressO1: 19.4,
  pressX2: 19.85,
  typeC: 20.35,
  appIn: 21.15,
  right3: 21.95,
  // S6: the install: the release, VitaShell, the app's Install page, a restart.
  laptopIn: 23.55,
  clickVpk: 24.75,
  fileFly: 25.3,
  shellIn: 25.95,
  vsX1: 26.3,
  vsX2: 26.65,
  vsX3: 27.15,
  vsDone: 27.65,
  ps1: 27.85,
  tapBubbleApp: 28.25,
  tapStartApp: 28.6,
  installX: 29.0,
  installDone: 29.6,
  restart: 30.45,
  // S7: the game, L + R, Right, Right.
  boot: 31.1,
  tapBubbleGame: 31.65,
  tapStartGame: 32.05,
  typeD: 32.4,
  lr2: 33.8,
  ovOpen2: 33.9,
  right4: 35.1,
  right5: 35.9,
  // S8: ○, PS, the home screen, Overlay, Games: Down, Down, × turns Neon Drift OFF.
  pressO2: 36.75,
  ps2: 37.15,
  flick: 37.6,
  tapBubbleApp2: 38.15,
  tapStartApp2: 38.55,
  down1: 39.0,
  down2: 39.35,
  toggle: 39.8,
};

// A western console: × enters, ○ cancels (core/buttons.h).
export const ENTER = "×";
export const CANCEL = "○";

// Captions: the owner's script. <em> marks the accent words.
export const CAPTIONS = {
  cap1: { h: "Learn Japanese with your PS&nbsp;Vita <em>for free</em>", in: 0.6, out: 4.3 },
  cap2: { h: "Look up words from the games you play, <em>entirely offline</em>", in: 4.55, out: 9.8 },
  cap3: { h: "Create flashcards of <em>words you don't know</em>", in: 10.05, out: 14.3 },
  cap4: { h: "With <em>screenshots</em>", in: 14.6, out: 18.75 },
  cap5: { h: "It only reads text <em>when it changes</em>", p: "Or only when you press L&nbsp;+&nbsp;R", pIn: 21.6, in: 19.1, out: 23.3 },
  cap6: { h: "<em>Easy install</em>", in: 23.6, out: 31.0 },
  cap7: { h: "Now it's installed: go to your game and press <em>L&nbsp;+&nbsp;R</em> to look up words.", in: 31.15, out: 36.6 },
  cap8: { h: "Turn it off for specific games, <em>so it doesn't get in the way</em>", in: 36.7, out: 41.35 },
};

// Scene 6's steps, shown under its caption (the one-VPK install, overlay-one-vpk-install's design).
export const STEPS = [
  "Download the <b>.vpk</b>",
  "Install it in <b>VitaShell</b>",
  "<b>Overlay</b>: Install, restart",
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

// The games on the home screen and in the Overlay app's list (promo-assets' Vita3K capture:
// src/app_capture/make_promo_games.py, original titles and icons).
export const GAMES = {
  PROMO0001: "夕凪ステップ",
  PROMO0002: "Neon Drift",
  PROMO0003: "Courier Run",
  PROMO0004: "Ember Garden",
  PROMO0005: "Hoshizora Diary",
};

// The release (overlay-one-vpk-install: the normal build's VPK is the main download, about 53 MB,
// an estimate from the release zip's 53,005,464 B). Version: the one its design shows.
export const RELEASE = {
  version: "0.8.0",
  vpk: "VitaJPOverlay-0.8.0.vpk",
  vpkSize: "53 MB",
  // VitaShell's getSizeString() for 53,005,464 B (1024-based, two decimals); an estimate too
  vpkSizeShell: "50.55 MB",
};

export const ENDCARD_FINE =
  "Not affiliated with or endorsed by Sony Interactive Entertainment. The game shown is an original mock-up made for this video. " +
  "Dictionary entries: JMdict (EDRDG, CC BY-SA 4.0). Frequency ranks: the Jiten frequency list (CC BY-SA 4.0). " +
  "Card design: the Lapis note type (GPL-3.0).";

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
  ["whoosh", C.laptopIn - 0.15, 0.35],
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
  ["dpad", C.right5, 0.45],
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
