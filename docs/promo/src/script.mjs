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
  s7: 30.5,
  s8: 34,
  end: 41.1,
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

// Scene 4: what a mined card is, as the merged .apkg export (#125) writes it.
export const CAP4 = {
  h: "Export them as <em>Lapis</em> cards for Anki",
  p: "with the word, its reading, the sentence and the definition",
};

// The overlay's body lines that depend on Anki mining (shell/overlay.cpp, #125).
export const OVERLAY = {
  queueBefore: "Anki queue: 2 · export in the Overlay app",
  queueAfter: "Anki queue: 3 · export in the Overlay app",
  hint: `Tap / ◀▶▲▼ word · ${ENTER} queue · stick scroll · □ region · ○ close`,
  // right after a first install card mining is off (docs/config.example.ini): no queue
  hintFresh: "Tap / ◀▶▲▼ word · stick scroll · □ region · ○ close",
};

// The Overlay app's export (settings/anki_page.c, settings/anki_export.c, #125, merged as e53d9fb).
// The size is what that export wrote for three text-only cards in its host
// test on main 6020c0b (host/tests/test_apkg.c: 86,234 B, shown as 85 KB; the export is unchanged on 1ed95ca);
// measured on the host, not on a console.
// VitaShell's size for the app's VPK (getSizeString(): 2 decimals, 1024-based): 491,554 B in
// CI's release zip of main 1ed95ca.
export const VPK_SIZE = "480.03 KB";

export const APKG = {
  dir: "ux0:data/VitaJPOverlay/anki/",
  name: "2026-10-15_142530-3cards.apkg",
  // settings/anki_export.c progress(): "notes" counts only every 8th card and the last
  progress: "Writing the cards… 3 of 3",
  result:
    "Exported 3 cards (85 KB) to 2026-10-15_142530-3cards.apkg. Copy it from ux0:data/VitaJPOverlay/anki/ " +
    "by VitaShell's FTP or USB (or the SD2Vita card, when it is ux0:) and open it in Anki.",
  copy:
    "Copy the .apkg from ux0:data/VitaJPOverlay/anki/ with VitaShell's FTP or USB (Select in VitaShell), " +
    "or with an SD2Vita as ux0: its card in a reader, then open it in Anki.",
};

// Scene 6's terminal shows src/install-transcript.txt: the real output of main's
// install-vita.sh during a first install (tools/capture_install.sh).

export const ENDCARD_FINE =
  "Not affiliated with or endorsed by Sony Interactive Entertainment. The game shown is an original mock-up made for this video. " +
  "Dictionary entries: JMdict (EDRDG, CC BY-SA 4.0). Frequency ranks: the Jiten frequency list (CC BY-SA 4.0). " +
  "Card design: the Lapis note type (GPL-3.0).";

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
  ["click", t.s6 + 0.7, 0.4],
  ["click", t.s6 + 0.9, 0.4],
  ["click", t.s6 + 1.15, 0.4],
  ["click", t.s6 + 1.45, 0.4],
  ["click", t.s6 + 1.8, 0.4],
  ["click", t.s6 + 4.05, 0.4],
  ["click", t.s6 + 4.3, 0.4],
  ["click", t.s6 + 4.5, 0.4],
  ["click", t.s6 + 4.65, 0.4],
  ["click", t.s6 + 4.85, 0.4],
  ["click", t.s6 + 5.15, 0.4],
  ["click", t.s6 + 5.8, 0.4],
  ["chime", t.s6 + 6.2, 0.4],
  ["click", t.s7 + 1.55, 0.4],
  ["click", t.s8 + 0.25, 0.4],
  ["home", t.s8 + 0.75, 0.45],
  ["whoosh", t.s8 + 1.4, 0.35],
  ["tap", t.s8 + 2.15, 0.45],
  ["tap", t.s8 + 2.85, 0.45],
  ["click", t.s8 + 3.45, 0.4],
  ["click", t.s8 + 3.8, 0.4],
  ["click", t.s8 + 4.25, 0.4],
  ["home", t.s8 + 5.05, 0.45],
  ["whoosh", t.s8 + 5.55, 0.35],
  ["tap", t.s8 + 6.25, 0.45],
  ["whoosh", t.end - 0.15, 0.35],
];
