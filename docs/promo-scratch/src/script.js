// The owner's script (PR #134, 2026-10-16 06:59Z), as the conductor set it on 09:07Z: "the script as
// written". Lines 4-6 show features that land for test.4 (mock-ups until they merge; see MOCKUPS).
// Each line is one caption over live action; swap a line here and re-render.
export const CAPTIONS = [
  { line: 1, t0: 0.15, t1: 5.0, text: 'Learn Japanese with your PS Vita for free', accent: ['free'] },
  { line: 2, t0: 5.15, t1: 10.15, text: 'Look up words from the games you play, entirely offline', accent: ['offline'] },
  { line: 3, t0: 10.25, t1: 13.95, text: "Create flashcards of words you don't know", accent: ['flashcards'] },
  // line 4: true once PR #150 (overlay-anki-screenshot) merges: the card keeps a picture of the game screen
  { line: 4, t0: 14.05, t1: 17.0, text: 'With screenshots', accent: ['screenshots'], size: 84 },
  // line 5 per the Auto setting (#146 "Read text": Automatically / Only when I press L + R); no battery claim
  { line: 5, t0: 17.1, t1: 21.15, text: 'It only reads the text when it changes', accent: ['changes'] },
  // line 6: the one-VPK install of feat/one-vpk-install (copy, VitaShell, Install, restart)
  { line: 6, t0: 21.3, t1: 26.55, text: 'Easy install', accent: ['Easy'], size: 84 },
  { line: 7, t0: 26.65, t1: 30.45, text: "Now it's installed, go to your game", accent: ['installed,'] },
  { line: 7, t0: 30.55, t1: 34.65, text: 'and press L + R to look up words.', accent: ['L', '+', 'R'] },
  { line: 8, t0: 34.75, t1: 41.28, text: "Turn it off for specific games, so it doesn't get in the way", accent: ['off'] },
];

// line 6's steps: in 16:9 the current one, large, over the caption; in 9:16 a list under the console
export const STEPS = [
  { t: 21.3, done: 22.4, text: 'Download the .vpk from GitHub Releases' },
  { t: 22.4, done: 22.95, text: 'Copy it to the Vita' },
  { t: 22.95, done: 24.3, text: 'Install it in VitaShell' },
  { t: 24.36, done: 25.42, text: 'Open Overlay and press Install' },
  { t: 25.45, done: 26.12, text: 'Restart the Vita: done' },
];
STEPS.window = { t0: 21.3, t1: 27.0 };

// Shots that show features still landing (named in the preview comment).
export const MOCKUPS = [
  { t0: 14.0, t1: 17.2, what: 'the Anki card with its game screenshot (PR #150, overlay-anki-screenshot)' },
  { t0: 19.5, t1: 21.2, what: 'the Overlay app\'s "Read text" setting (PR #146)' },
  { t0: 21.2, t1: 26.12, what: 'the one-VPK install: the release page, VitaShell\'s install, the Overlay app\'s Install page and the restart (feat/one-vpk-install)' },
  { t0: 26.12, t1: 34.7, what: 'after that install, OCR in any game with no per-game setup (PR #148: the kernel loads the worker in every enabled game)' },
];
