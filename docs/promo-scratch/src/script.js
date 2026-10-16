// The owner's script (PR #134, 2026-10-16 06:59Z), as the conductor set it on 09:07Z: "the script as
// written". Lines 4-6 show features that land for test.4 (mock-ups until they merge; see MOCKUPS).
// Each line is one caption over live action; swap a line here and re-render.
export const CAPTIONS = [
  { line: 1, t0: 0.2, t1: 5.0, text: 'Learn Japanese with your PS Vita for free', accent: ['free'] },
  { line: 2, t0: 5.2, t1: 10.5, text: 'Look up words from the games you play, entirely offline', accent: ['offline'] },
  { line: 3, t0: 10.65, t1: 14.3, text: "Create flashcards of words you don't know", accent: ['flashcards'] },
  // line 4: true once overlay-anki-screenshot merges (the card keeps a picture of the game screen)
  { line: 4, t0: 14.45, t1: 16.75, text: 'With screenshots', accent: ['screenshots'], size: 84 },
  // line 5 per the Auto setting (#146 "Read text": Automatically / Only when I press L + R); no battery claim
  { line: 5, t0: 16.95, t1: 21.05, text: 'It only reads the text when it changes', accent: ['changes'] },
  // line 6: the one-VPK install of feat/one-vpk-install (four touches, then a restart)
  { line: 6, t0: 21.2, t1: 26.45, text: 'Easy install', accent: ['Easy'], size: 84 },
  { line: 7, t0: 26.6, t1: 30.35, text: "Now it's installed, go to your game", accent: ['installed,'] },
  { line: 7, t0: 30.6, t1: 34.7, text: 'and press L + R to look up words.', accent: ['L', '+', 'R'] },
  { line: 8, t0: 34.9, t1: 41.3, text: "Turn it off for specific games, so it doesn't get in the way", accent: ['off'] },
];

export const STEPS = [
  { t: 21.25, done: 22.35, text: 'Download the .vpk from GitHub Releases' },
  { t: 22.35, done: 22.95, text: 'Copy it to the Vita' },
  { t: 22.95, done: 24.3, text: 'Install it in VitaShell' },
  { t: 24.36, done: 25.7, text: 'Open Overlay and press Install' },
  { t: 25.75, done: 26.45, text: 'Restart: done' },
];
STEPS.window = { t0: 21.2, t1: 26.8 };
STEPS.note = { t: 21.7, text: 'Once: HENkaku Settings › Enable Unsafe Homebrew' };

// Shots that show features still landing (named in the preview comment).
export const MOCKUPS = [
  { t0: 14.3, t1: 16.9, what: 'the Anki card with its game screenshot (overlay-anki-screenshot)' },
  { t0: 19.0, t1: 21.0, what: 'the Overlay app\'s "Read text" setting (#146)' },
  { t0: 21.1, t1: 26.5, what: 'the one-VPK install: the release, VitaShell\'s install, the Overlay app\'s Install page (feat/one-vpk-install)' },
];
