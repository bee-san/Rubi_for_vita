// The choreography: one continuous play session on a white PCH-1000, in the owner's script order.
// Times are seconds; the music bed (promo-assets' bed-44s: 120 BPM, lifts at 4.5 s and 31.0 s, a
// breakdown 23.5-31 s, the last chord at 41.5 s) sets the grid. Both cuts share every event and differ
// only in framing (camera keys, panel places, caption places).
import { keyed, pressAmt, sway, clamp, lerp, inv, easeOut, easeInOut, easeIn, easeOutBack, smooth } from './util.js';
import { SCREEN } from './device.js';
import { APP } from './screen.js';
import { drawCard, drawBrowser, drawFileChip, drawVitaShell, drawReadText } from './panels.js';
import { CAPTIONS, STEPS, MOCKUPS } from './script.js';

const D = 44.0;
const DEG = Math.PI / 180;

// ---------------------------------------------------------------- event times
export const T = {
  type1: 0.15, x1: 2.55, type2: 2.65,
  lr1: 5.0, ovOpen: 5.08, r1: 7.6, r1land: 7.95,
  x2: 10.75, saved1: 11.15, pop1: 11.2, r2: 12.3, x3: 13.2, saved2: 13.3, pop2: 13.35,
  focus: 14.3, flip0: 14.55, flip1: 15.35, cardOut: 16.7,
  circle: 16.85, x4: 17.25, s2: 17.35, type3: 17.6, scan: 18.6, rt0: 19.0, rt1: 21.0,
  br0: 21.1, click: 21.85, dl0: 21.9, dl1: 22.35, fly1: 22.95,
  x5: 23.2, x6: 23.5, perm: 23.66, x7: 23.85, inst1: 24.3, ps1: 24.36, tapOv: 24.62, page: 24.8,
  x8: 25.15, done: 25.7, power: 26.05, boot: 26.42, home2: 26.55,
  tapGame: 27.05, la: 27.25, tapStart: 27.75, game: 28.0, type4: 28.25,
  lr2: 30.95, ov2: 31.03, r3: 32.25, r4: 33.45,
  ps2: 34.95, la2: 35.08, swipe: 35.3, tapOv2: 35.75, la3: 35.95, tapStart2: 36.25, app: 36.5,
  d1: 36.8, d2: 37.05, d3: 37.3, x9: 37.7, ps3: 39.3, swipe2: 39.45, tapCont: 39.95, resume: 40.15,
  x10: 40.45, type5: 40.55, star0: 40.75, end: 41.35,
};

const PRESSES = [
  { t: T.x1, btn: 'cross' },
  { t: T.lr1, btns: ['l', 'r'], hold: 0.2 },
  { t: T.r1, btn: 'dpad-right' },
  { t: T.x2, btn: 'cross' }, { t: T.r2, btn: 'dpad-right' }, { t: T.x3, btn: 'cross' },
  { t: T.circle, btn: 'circle' }, { t: T.x4, btn: 'cross' },
  { t: T.x5, btn: 'cross' }, { t: T.x6, btn: 'cross' }, { t: T.x7, btn: 'cross' }, { t: T.ps1, btn: 'ps' },
  { t: T.x8, btn: 'cross' }, { t: T.power, btn: 'power', hold: 0.25 },
  { t: T.lr2, btns: ['l', 'r'], hold: 0.2 },
  { t: T.r3, btn: 'dpad-right' }, { t: T.r4, btn: 'dpad-right' },
  { t: T.ps2, btn: 'ps' },
  { t: T.d1, btn: 'dpad-down' }, { t: T.d2, btn: 'dpad-down' }, { t: T.d3, btn: 'dpad-down' },
  { t: T.x9, btn: 'cross' }, { t: T.ps3, btn: 'ps' }, { t: T.x10, btn: 'cross' },
];
const BUTTONS = ['l', 'r', 'cross', 'circle', 'triangle', 'square', 'ps', 'dpad-right', 'dpad-down', 'dpad-left', 'dpad-up', 'power', 'start', 'select'];

// ---------------------------------------------------------------- camera keys
// az/el in degrees round the target, w = the frame's width in mm at the target; w9/tgt9/az9/el9 for 9:16.
const CAM = [
  { t: 0.0, tgt: [0, -9, 9.7], tgt9: [0, 0, 9.7], az: 0, el: 0, w: 150, w9: 124 },
  { t: 0.9, tgt: [0, -9, 9.7], tgt9: [0, 0, 9.7], az: 3, el: 1, w: 156, w9: 128, ease: 'smooth' },
  { t: 2.5, tgt: [0, -2, 0], az: -22, el: 10, w: 300, w9: 205, ease: 'inOut' },
  { t: 4.35, tgt: [0, 0, 0], az: -14, el: 18, w: 285, w9: 200, ease: 'smooth' },
  { t: 4.92, tgt: [0, 22, -1], az: -4, el: 50, w: 265, w9: 200, ease: 'quint' },
  { t: 5.3, tgt: [0, 21, -1], az: -2, el: 48, w: 262, w9: 198, ease: 'smooth' },
  { t: 6.35, tgt: [0, -8, 9.7], tgt9: [0, 1, 9.7], az: -5, el: 6, w: 160, w9: 124, ease: 'inOut' },
  { t: 7.6, tgt: [0, -8, 9.7], tgt9: [0, 1, 9.7], az: 3, el: 4, w: 156, w9: 120, ease: 'smooth' },
  { t: 10.6, tgt: [2, -8, 9.7], tgt9: [2, 0, 9.7], az: 9, el: 6, w: 160, w9: 122, ease: 'smooth' },
  { t: 11.6, tgt: [45, 2, 5], tgt9: [0, -22, 5], az: -12, el: 8, w: 340, w9: 215, ease: 'inOut' },
  { t: 14.2, tgt: [52, 2, 5], tgt9: [0, -24, 5], az: -5, el: 6, w: 330, w9: 210, ease: 'smooth' },
  { t: 14.75, tgt: [0, 3, 60], tgt9: [0, -4, 60], az: 0, el: 3, w: 225, w9: 175, ease: 'inOut' },
  { t: 16.6, tgt: [0, 3, 58], tgt9: [0, -4, 58], az: 4, el: 4, w: 218, w9: 170, ease: 'smooth' },
  { t: 17.35, tgt: [0, 0, 0], az: -9, el: 9, w: 255, w9: 195, ease: 'inOut' },
  { t: 18.5, tgt: [0, -10, 6], tgt9: [0, -6, 6], az: -3, el: 5, w: 190, w9: 150, ease: 'smooth' },
  { t: 19.6, tgt: [-62, 0, 0], tgt9: [0, -30, 0], az: -12, el: 7, w: 340, w9: 215, ease: 'inOut' },
  { t: 21.0, tgt: [-66, 0, 0], tgt9: [0, -30, 0], az: -9, el: 6, w: 345, w9: 196, az9: 0, ease: 'smooth' },
  { t: 21.6, tgt: [-40, 4, 30], tgt9: [0, -10, 40], az: -6, el: 4, w: 330, w9: 200, ease: 'inOut' },
  { t: 22.4, tgt: [-40, 4, 30], tgt9: [0, -10, 40], az: -3, el: 5, w: 325, w9: 198, ease: 'smooth' },
  { t: 23.1, tgt: [-46, -3, 4], tgt9: [0, -28, 4], az: -8, el: 5, w: 262, w9: 175, ease: 'inOut' },
  { t: 26.3, tgt: [-50, -3, 4], tgt9: [0, -28, 4], az: -4, el: 6, w: 256, w9: 172, ease: 'smooth' },
  { t: 27.2, tgt: [0, 0, 6], az: 5, el: 6, w: 220, w9: 165, ease: 'inOut' },
  { t: 28.6, tgt: [0, -9, 9.7], tgt9: [0, 0, 9.7], az: 2, el: 3, w: 172, w9: 132, ease: 'smooth' },
  { t: 30.85, tgt: [0, 23, -1], az: 5, el: 50, w: 262, w9: 200, ease: 'inOut' },
  { t: 31.15, tgt: [0, 22, -1], az: 4, el: 48, w: 258, w9: 198, ease: 'smooth' },
  { t: 32.0, tgt: [0, -8, 9.7], tgt9: [0, 1, 9.7], az: 3, el: 5, w: 160, w9: 124, ease: 'inOut' },
  { t: 34.7, tgt: [0, -8, 9.7], tgt9: [0, 1, 9.7], az: -6, el: 4, w: 156, w9: 121, az9: -10, ease: 'inOut' },
  { t: 35.6, tgt: [0, -9, 9.7], tgt9: [0, 0, 9.7], az: -2, el: 3, w: 168, w9: 128, ease: 'smooth' },
  { t: 37.75, tgt: [4, -10, 9.7], tgt9: [4, -3, 9.7], az: 4, el: 3, w: 158, w9: 122, ease: 'smooth' },
  { t: 39.25, tgt: [2, -9, 9.7], tgt9: [2, -2, 9.7], az: -2, el: 4, w: 164, w9: 124, ease: 'smooth' },
  { t: 40.4, tgt: [0, 0, 0], az: -12, el: 9, w: 245, w9: 195, ease: 'inOut' },
  { t: 41.6, tgt: [0, -10, 0], tgt9: [0, -6, 0], az: -24, el: 12, w: 330, w9: 220, ease: 'inOut' },
  { t: 44.0, tgt: [0, -10, 0], tgt9: [0, -6, 0], az: -30, el: 15, w: 345, w9: 228, ease: 'smooth' },
];

// ---------------------------------------------------------------- palettes (the studio takes the game's light)
const PAL = {
  s1: { top: '#1c1532', bottom: '#5b3149', spill: '#ffb38a', spillA: 0.33, key: '#ffe4c8', rim: '#ff9a76', bokeh: '#ffb07a', bokeh2: '#ff8fb4', bokehA: 0.45, bokehSize: 90, petals: 1 },
  s2: { top: '#15153a', bottom: '#45306a', spill: '#c49cff', spillA: 0.3, key: '#efe6ff', rim: '#b48cff', bokeh: '#c8a2ff', bokeh2: '#ff9ccf', bokehA: 0.42, bokehSize: 85, petals: 0 },
  app: { top: '#09111a', bottom: '#11303a', spill: '#5bdccf', spillA: 0.26, key: '#ffffff', rim: '#5bdccf', bokeh: '#5bdccf', bokeh2: '#9fd0ff', bokehA: 0.38, bokehSize: 80, petals: 0 },
  s3: { top: '#060a1b', bottom: '#14224a', spill: '#7aa6ff', spillA: 0.28, key: '#e3ebff', rim: '#6fa8ff', bokeh: '#ffffff', bokeh2: '#9fc4ff', bokehA: 0.55, bokehSize: 45, petals: 0 },
};
const PALKEYS = [[0, 's1'], [16.95, 's1'], [17.6, 's2'], [20.95, 's2'], [21.5, 'app'], [26.4, 'app'], [27.9, 's3'], [44, 's3']];
function hex(c) { return [parseInt(c.slice(1, 3), 16), parseInt(c.slice(3, 5), 16), parseInt(c.slice(5, 7), 16)]; }
function mixHex(a, b, k) { const A = hex(a), B = hex(b); return '#' + A.map((v, i) => Math.round(lerp(v, B[i], k)).toString(16).padStart(2, '0')).join(''); }
function palette(t) {
  let i = 0; while (i < PALKEYS.length - 2 && t > PALKEYS[i + 1][0]) i++;
  const [ta, a] = PALKEYS[i], [tb, b] = PALKEYS[i + 1];
  const k = smooth(inv(ta, tb, t)); const A = PAL[a], B = PAL[b]; const out = {};
  for (const key of Object.keys(A)) out[key] = typeof A[key] === 'number' ? lerp(A[key], B[key], k) : mixHex(A[key], B[key], k);
  return out;
}

// ---------------------------------------------------------------- build
export function buildTimeline(S) {
  const { camera, device, screen, img, json, makePanel, THREE } = S;
  const P = S.cut === '9x16';
  const vfov = P ? 42 : 30;
  const htan = Math.tan(vfov / 2 * DEG) * (S.W / S.H);
  const nChars = (m) => json[`vn/${m.split('-')[0]}/${m}-text.json`].chars.length;
  const icons = {
    yuunagi: img['app/icons/PROMO0001-yuunagi-step.png'], neon: img['app/icons/PROMO0002-neon-drift.png'], courier: img['app/icons/PROMO0003-courier-run.png'],
    ember: img['app/icons/PROMO0004-ember-garden.png'], hoshizora: img['app/icons/PROMO0005-hoshizora-diary.png'], lantern: img['app/icons/PROMO0006-paper-lantern.png'],
    sakura: img['app/icons/PROMO0008-sakura-no-kioku.png'], overlay: img['app/icons/VJOSET001-overlay.png'],
  };
  const HOME_BEFORE = [icons.yuunagi, icons.neon, icons.ember, icons.hoshizora, icons.courier, icons.lantern, icons.sakura];
  const HOME_AFTER = [icons.yuunagi, icons.neon, icons.ember, icons.hoshizora, icons.courier, icons.lantern, icons.overlay];
  const OV_BUBBLE = 6, GAME_BUBBLE = 0;
  const bubbleXY = [[330, 330], [750, 330], [1170, 330], [1590, 330], [540, 700], [960, 700], [1380, 700]];

  // camera keys for this cut. Portrait: the device sits above the middle (its centre about y 780 of
  // 1920), the caption above it and the cards, panels and word call-outs below it.
  const CAMK = CAM.map((k) => {
    if (!P) return { t: k.t, ease: k.ease, tgt: k.tgt, w: k.w, az: k.az, el: k.el };
    const w = k.w9 ?? k.w, base = k.tgt9 ?? k.tgt;
    return { t: k.t, ease: k.ease, w, az: k.az9 ?? k.az, el: k.el9 ?? k.el, tgt: [base[0], base[1] - 0.15 * w, base[2]] };
  });

  // ---- panels
  const CARD_W = 120, CARD_H = 61.8; // a 1320 x 680 crop of Anki's 1920 x 1200 render
  const crop = [300, 20, 1320, 680];
  const card1 = makePanel('card1', CARD_W, CARD_H, 1320, 680, { back: true });
  drawCard(card1.front.g, img['anki/with-screenshot-MOCKUP-feat-anki-screenshot/png/card-1-front-desktop-light-MOCKUP.png'], crop);
  drawCard(card1.back.g, img['anki/with-screenshot-MOCKUP-feat-anki-screenshot/png/card-1-back-desktop-light-MOCKUP.png'], crop);
  card1.dirty = true;
  const card2 = makePanel('card2', CARD_W, CARD_H, 1320, 680);
  drawCard(card2.front.g, img['anki/with-screenshot-MOCKUP-feat-anki-screenshot/png/card-2-front-desktop-light-MOCKUP.png'], crop); card2.dirty = true;
  const browser = makePanel('browser', 160, 106, 1600, 1060);
  const chip = makePanel('chip', 52, 14, 1040, 280); drawFileChip(chip.front.g); chip.dirty = true;
  // #146's real app page (Vita3K capture of the PR branch): the header, the tabs and the Read text card
  const rtImg = img['app/pr146-ocr-mode-21488a7-NOT-MERGED/png/PR146-10-read-text-at-top-automatically.png'] || null;
  const readText = rtImg ? makePanel('readtext', 124, 28.4, 1920, 440) : makePanel('readtext', 118, 40, 1180, 400);
  drawReadText(readText.front.g, { choice: 0, img: rtImg, crop: rtImg ? [0, 0, 960, 220] : null }); readText.dirty = true;
  // the scan sweep: the video's own graphic over the text box, attached to the device
  const scanC = document.createElement('canvas'); scanC.width = 256; scanC.height = 64;
  { const g = scanC.getContext('2d'); const gr = g.createLinearGradient(0, 0, 256, 0); gr.addColorStop(0, 'rgba(91,220,207,0)'); gr.addColorStop(0.8, 'rgba(91,220,207,0.55)'); gr.addColorStop(0.96, 'rgba(200,255,250,1)'); gr.addColorStop(1, 'rgba(91,220,207,0)'); g.fillStyle = gr; g.fillRect(0, 0, 256, 64); }
  const scanTex = new THREE.CanvasTexture(scanC); scanTex.colorSpace = THREE.SRGBColorSpace;
  const scan = new THREE.Mesh(new THREE.PlaneGeometry(26, 17.5), new THREE.MeshBasicMaterial({ map: scanTex, transparent: true, depthWrite: false, blending: THREE.AdditiveBlending, opacity: 0, toneMapped: false }));
  scan.position.set(0, -21.4, SCREEN.z + 0.6); scan.renderOrder = 6; device.root.add(scan);
  const frameBox = new THREE.Mesh(new THREE.PlaneGeometry(103.4, 17.6), new THREE.MeshBasicMaterial({ color: 0x5bdccf, transparent: true, opacity: 0, depthWrite: false, blending: THREE.AdditiveBlending, toneMapped: false }));
  frameBox.position.set(0, -21.4, SCREEN.z + 0.5); device.root.add(frameBox);

  const DECK = P ? { p: [0, -78, 18], ry: 0, rz: 0.0, s: 0.5 } : { p: [152, 14, 12], ry: -0.38, rz: 0.06, s: 0.52 };
  const FOCUS = P ? { p: [0, -40, 64], s: 0.98 } : { p: [0, 3, 66], s: 1.18 };

  function setPanel(p, { pos, rot = [0, 0, 0], s = 1, a = 1, halo = 0.35 }) {
    p.grp.visible = a > 0.003;
    if (!p.grp.visible) return;
    p.grp.position.set(...pos); p.grp.rotation.set(...rot); p.grp.scale.setScalar(s);
    for (const m of p.mats) { m.opacity = a; }
    p.halo.material.opacity = a * halo;
  }
  function bez(a, b, c, k) { return a.map((v, i) => (1 - k) * (1 - k) * v + 2 * (1 - k) * k * b[i] + k * k * c[i]); }

  function cardFlight(k, from, peak, to) { // 0..1 -> {pos, s, spin}
    const e = easeInOut(k);
    return bez(from, peak, to, e);
  }
  const SCREEN_W = [0, 0, SCREEN.z + 2];

  function panelsAt(t) {
    // card 1: pops at pop1, lands on the deck, is focused and flipped in line 4, then goes
    let c1 = { a: 0 }, c2 = { a: 0 };
    const deckPos = DECK.p, deckRot = [0, DECK.ry, DECK.rz];
    if (t >= T.pop1) {
      const k = inv(T.pop1, T.pop1 + 0.8, t);
      if (k < 1) {
        const pos = cardFlight(k, SCREEN_W, P ? [0, -30, 90] : [60, 34, 95], deckPos);
        c1 = { pos, rot: [0, lerp(0, DECK.ry, k) + Math.sin(k * Math.PI) * 0.5, lerp(0, DECK.rz, k)], s: lerp(0.16, DECK.s, easeOut(k)), a: clamp(k * 6) };
      } else c1 = { pos: deckPos, rot: deckRot, s: DECK.s, a: 1 };
    }
    if (t >= T.pop2) {
      const k = inv(T.pop2, T.pop2 + 0.8, t);
      const off = P ? [5, -4, -1.5] : [6, -5, -1.5];
      const to = deckPos.map((v, i) => v + off[i]);
      if (k < 1) c2 = { pos: cardFlight(k, SCREEN_W, P ? [0, -30, 90] : [60, 34, 95], to), rot: [0, lerp(0, DECK.ry, k) + Math.sin(k * Math.PI) * 0.5, lerp(0, DECK.rz - 0.1, k)], s: lerp(0.16, DECK.s, easeOut(k)), a: clamp(k * 6) };
      else c2 = { pos: to, rot: [0, DECK.ry, DECK.rz - 0.1], s: DECK.s, a: 1 };
      // card 1 lifts out of the deck for the close-up
    }
    if (t >= T.focus) {
      const k = easeInOut(inv(T.focus, T.focus + 0.5, t));
      const flip = easeInOut(inv(T.flip0, T.flip1, t));
      const pos = deckPos.map((v, i) => lerp(v, FOCUS.p[i], k));
      pos[1] += Math.sin(flip * Math.PI) * 6;
      c1 = { pos, rot: [lerp(0, -0.04, k), lerp(DECK.ry, 0, k) + flip * Math.PI, lerp(DECK.rz, 0, k)], s: lerp(DECK.s, FOCUS.s, k), a: 1 };
      if (t >= T.cardOut) {
        const o = easeIn(inv(T.cardOut, T.cardOut + 0.45, t));
        c1.pos = FOCUS.p.map((v, i) => lerp(v, (P ? [0, -82, 18] : DECK.p)[i], o)); c1.s = lerp(FOCUS.s, DECK.s, o);
        c1.rot = [0, lerp(Math.PI, Math.PI * 2 + DECK.ry, o), lerp(0, DECK.rz, o)];
      }
    }
    // the deck leaves once line 5 starts
    const deckOut = easeIn(inv(T.cardOut + 0.45, T.cardOut + 0.9, t));
    if (deckOut > 0) { for (const c of [c1, c2]) if (c.a) { c.a *= 1 - deckOut; c.s *= 1 - deckOut * 0.4; } }
    setPanel(card1, c1.a ? c1 : { pos: [0, 0, 0], a: 0 });
    setPanel(card2, c2.a ? c2 : { pos: [0, 0, 0], a: 0 });

    // the Read text card (#146)
    {
      const kin = easeOutBack(inv(T.rt0, T.rt0 + 0.5, t), 1.2), kout = easeIn(inv(T.rt1 - 0.3, T.rt1 + 0.1, t));
      const a = clamp(inv(T.rt0, T.rt0 + 0.2, t)) * (1 - kout);
      const pos = P ? [0, lerp(-130, -82, kin), 26] : [lerp(-260, -150, kin), 8, lerp(-40, 26, kin)];
      setPanel(readText, { pos, rot: P ? [-0.06, 0, 0] : [0, 0.32, 0], s: P ? 1.25 : 0.95, a });
    }
    // the browser: the release page, then the .vpk flies to the Vita
    {
      const kin = easeOutBack(inv(T.br0, T.br0 + 0.55, t), 1.1), kout = easeIn(inv(T.dl1 + 0.05, T.dl1 + 0.6, t));
      const a = clamp(inv(T.br0, T.br0 + 0.2, t)) * (1 - kout);
      const pos = P ? [lerp(0, 0, kin), lerp(-160, -10, kin) + kout * 140, 70 - kout * 40] : [lerp(260, 48, kin) + kout * 230, 8 + kout * 30, lerp(-60, 62, kin) - kout * 90];
      setPanel(browser, { pos, rot: P ? [0, 0, 0] : [0, -0.18 - kout * 0.4, 0], s: P ? 0.78 : 1, a, halo: 0.45 });
      if (a > 0) {
        const lt = t - T.br0;
        const ptr = [lerp(1300, 760, easeInOut(inv(0.35, 0.72, lt))), lerp(940, 482, easeInOut(inv(0.35, 0.72, lt)))];
        drawBrowser(browser.front.g, { pointer: ptr, click: inv(T.click, T.click + 0.12, t), hover: t > T.click - 0.15, dl: inv(T.dl0, T.dl1, t) });
        browser.dirty = true;
      }
      // the file chip: from the browser's asset row into the Vita's screen
      const kf = inv(T.dl1 - 0.02, T.fly1, t);
      if (kf > 0 && kf < 1) {
        const from = P ? [-20, 0, 80] : [30, 20, 70], to = [0, 0, SCREEN.z + 3];
        const pos = bez(from, P ? [0, 40, 110] : [-20, 60, 110], to, easeInOut(kf));
        setPanel(chip, { pos, rot: [0, Math.sin(kf * Math.PI) * 0.4, 0], s: lerp(1, 0.25, easeIn(kf)), a: clamp(kf * 8) * (1 - easeIn(inv(0.8, 1, kf))), halo: 0.3 });
      } else setPanel(chip, { pos: [0, 0, 0], a: 0 });
    }
    // the scan sweep (line 5): once, after the text has settled
    {
      const k = inv(T.scan, T.scan + 0.6, t);
      scan.material.opacity = k > 0 && k < 1 ? Math.sin(k * Math.PI) * 0.95 : 0;
      scan.position.x = lerp(-48, 48, easeInOut(k));
      frameBox.material.opacity = k > 0 && k < 1 ? Math.sin(k * Math.PI) * 0.10 : 0;
    }
  }

  // ---------------------------------------------------------------- the screen
  const typed = (t, t0, rate, n) => clamp((t - t0) * rate, 0, n);
  const N = { m11: nChars('s1-m1'), m12: nChars('s1-m2'), m21: nChars('s2-m1'), m31: nChars('s3-m1'), m32: nChars('s3-m2') };
  const OV1 = (t) => (t < T.r1 + 0.03 ? 's1-m2-ov01-open' : t < T.r1land ? 's1-m2-ov02-right-inflight' : t < T.x2 + 0.05 ? 's1-m2-ov03-issho' : t < T.saved1 ? 's1-m2-ov04-issho-saving'
    : t < T.r2 + 0.03 ? 's1-m2-ov05-issho-saved' : t < T.saved2 ? 's1-m2-ov06-miniiku' : 's1-m2-ov07-miniiku-saved');
  const OV2 = (t) => (t < T.r3 + 0.03 ? 's3-m1-ov03-open' : t < T.r4 + 0.03 ? 's3-m1-ov04-kodomonokoro' : 's3-m1-ov05-mitai');
  const ovIn = (t, t0) => { const k = easeOut(inv(t0, t0 + 0.16, t)); return [k, (1 - k) * 36]; };
  const tap = (t, t0, x, y) => screen.ripple(x, y, inv(t0, t0 + 0.45, t));
  const installCap = (t) => {
    if (t < T.x8 + 0.05) return 'MOCKUP-01-install-plan';
    if (t >= T.done) return 'MOCKUP-06-installed-restart-the-vita-now';
    const steps = ['MOCKUP-02-installing-plugins-and-models', 'MOCKUP-03-installing-checking-the-dictionary-56MB', 'MOCKUP-04-installing-checking-the-dictionary-121MB', 'MOCKUP-05-installing-config-txt-lines'];
    return steps[Math.min(3, Math.floor(inv(T.x8 + 0.05, T.done, t) * 4))];
  };
  const appCap = (t) => (t < T.d1 + 0.03 ? '01-games-top' : t < T.d2 + 0.03 ? '02-games-scroll-1' : t < T.d3 + 0.03 ? '03-games-scroll-2' : t < T.x9 + 0.04 ? '04-games-neon-drift-selected' : '05-games-neon-drift-OFF-saved');
  const gateArt = document.createElement('canvas'); gateArt.width = 436; gateArt.height = 380;
  { const g = gateArt.getContext('2d'); g.drawImage(img['vn/s3/s3-m1-notext@2x.png'], 560, 60, 800, 697, 0, 0, 436, 380);
    g.fillStyle = 'rgba(10,14,30,0.35)'; g.fillRect(0, 290, 436, 90); g.fillStyle = '#fff'; g.font = '700 40px "Noto Sans JP", sans-serif'; g.textAlign = 'center'; g.textBaseline = 'middle'; g.fillText('夕凪ステップ', 218, 336); }
  const gameLA = (gate, pressK, extra = {}) => screen.liveArea({ art: img['vn/s1/s1-m2-notext@2x.png'], artRect: [0, 0, 1920, 760], icon: icons.yuunagi, title: '夕凪ステップ', gate, gateImg: gateArt, pressK, ...extra });
  const appLA = (pressK, extra = {}) => screen.liveArea({ art: img['repo/bg.png'], icon: icons.overlay, title: 'Overlay', gate: 'Start', gateImg: img['repo/startup.png'], pressK, ...extra });

  function drawScreen(t) {
    screen.clear();
    if (t < T.x1 + 0.06) { screen.vn('s1-m1', typed(t, T.type1, 22, N.m11), { next: true, t }); return; }
    if (t < T.lr1 + 0.06) { screen.vn('s1-m2', typed(t, T.type2, 20, N.m12), { next: true, t }); return; }
    if (t < T.circle + 0.04) { // the overlay over the game (lines 2-4)
      screen.vn('s1-m2', N.m12);
      const [a, dy] = ovIn(t, T.ovOpen); screen.overlay(OV1(t), a, dy); return;
    }
    if (t < T.x4 + 0.06) { screen.vn('s1-m2', N.m12, { next: true, t }); return; }
    if (t < T.br0 + 1.6) { // scene 2: the street at dusk
      const k = inv(T.x4 + 0.06, T.s2, t);
      screen.vn('s2-m1', typed(t, T.type3, 18, N.m21), { next: true, t });
      if (k < 1) screen.vn('s1-m2', N.m12, { alpha: 1 - k });
      return;
    }
    if (t < T.fly1) { screen.vn('s2-m1', N.m21); screen.black(0.25 * inv(T.br0 + 1.6, T.fly1, t)); return; }
    if (t < T.inst1 + 0.06) { // VitaShell: install the VPK
      const dlg = t < T.x5 + 0.04 ? null : t < T.x6 + 0.04 ? 'ask' : t < T.perm ? 'progress' : t < T.x7 + 0.04 ? 'perm' : 'progress';
      const open = dlg === 'ask' ? inv(T.x5 + 0.04, T.x5 + 0.16, t) : dlg === 'perm' ? inv(T.perm, T.perm + 0.12, t) : 1;
      const prog = t < T.perm ? 0 : inv(T.x7 + 0.08, T.inst1 - 0.04, t);
      drawVitaShell(screen, { focus: 1, dialog: dlg, progress: prog, open });
      screen.black(1 - inv(T.fly1, T.fly1 + 0.1, t));
      return;
    }
    if (t < T.page) { // home: the new Overlay bubble
      screen.home({ icons: HOME_AFTER, focus: OV_BUBBLE, pressK: inv(T.tapOv, T.tapOv + 0.25, t), running: -1 });
      tap(t, T.tapOv, ...bubbleXY[OV_BUBBLE]); return;
    }
    if (t < T.power + 0.05) { // the app's Install page: promo-assets' mock-up, drawn by the app's own code (feat/one-vpk-install's design)
      screen.capture(`app/install-MOCKUP-one-vpk/png/${installCap(t)}.png`);
      screen.black(1 - inv(T.page, T.page + 0.12, t));
      return;
    }
    if (t < T.boot) { screen.capture('app/install-MOCKUP-one-vpk/png/MOCKUP-06-installed-restart-the-vita-now.png'); screen.black(inv(T.power + 0.05, T.power + 0.25, t)); return; }
    if (t < T.la) { // restarted: home
      screen.home({ icons: HOME_AFTER, focus: GAME_BUBBLE, pressK: inv(T.tapGame, T.tapGame + 0.25, t), running: -1 });
      tap(t, T.tapGame, ...bubbleXY[GAME_BUBBLE]); screen.black(1 - inv(T.boot, T.home2, t)); return;
    }
    if (t < T.game) { gameLA('Start', inv(T.tapStart, T.tapStart + 0.25, t), { alpha: 1 }); tap(t, T.tapStart, 1520, 600); return; }
    if (t < T.ps2 + 0.05) { // scene 3: the hilltop; L + R; the walk through the words
      screen.vn('s3-m1', typed(t, T.type4, 18, N.m31), { next: true, t });
      screen.black(1 - inv(T.game, T.game + 0.25, t));
      if (t >= T.ov2) { const [a, dy] = ovIn(t, T.ov2); screen.overlay(OV2(t), a, dy); }
      return;
    }
    if (t < T.swipe) { gameLA('Continue', 0, { alpha: inv(T.ps2 + 0.05, T.la2 + 0.06, t) }); return; }
    if (t < T.la3) { // swipe to home, tap Overlay
      const k = easeInOut(inv(T.swipe, T.swipe + 0.3, t));
      screen.home({ icons: HOME_AFTER, focus: OV_BUBBLE, pressK: inv(T.tapOv2, T.tapOv2 + 0.25, t), running: GAME_BUBBLE, scroll: (1 - k) * -1920 });
      if (k < 1) gameLA('Continue', 0, { slide: k * 1920 });
      tap(t, T.tapOv2, ...bubbleXY[OV_BUBBLE]); return;
    }
    if (t < T.app) { appLA(inv(T.tapStart2, T.tapStart2 + 0.25, t)); tap(t, T.tapStart2, 1520, 600); return; }
    if (t < T.ps3 + 0.05) { screen.capture(`app/main-1ed95ca/png/${appCap(t)}.png`); screen.black(1 - inv(T.app, T.app + 0.12, t)); return; }
    if (t < T.swipe2) { appLA(0); return; }
    if (t < T.resume) {
      const k = easeInOut(inv(T.swipe2, T.swipe2 + 0.3, t));
      gameLA('Continue', inv(T.tapCont, T.tapCont + 0.25, t), { slide: (1 - k) * -1920 });
      if (k < 1) appLA(0, { slide: k * 1920 });
      tap(t, T.tapCont, 1520, 600); return;
    }
    if (t < T.x10 + 0.06) { screen.vn('s3-m1', N.m31, { next: true, t }); screen.black(1 - inv(T.resume, T.resume + 0.2, t)); return; }
    screen.vn('s3-m2', typed(t, T.type5, 18, N.m32), { next: true, t });
  }

  // ---------------------------------------------------------------- SFX (the mix is made by tools/audio.py)
  const sfx = [];
  const add = (t, name, gain = 1) => sfx.push({ t: +t.toFixed(3), name, gain });
  for (const p of PRESSES) {
    const b = p.btn || p.btns[0];
    add(p.t, b === 'l' ? 'lr' : b.startsWith('dpad') ? 'dpad' : b === 'power' ? 'shoulder' : 'click', b === 'l' ? 1 : 0.8);
  }
  const blips = (t0, rate, n) => { for (let i = 0; i < n; i += 2) add(t0 + i / rate, `text-blip-${(i / 2) % 5 + 1}`, 0.32); };
  blips(T.type1, 22, N.m11); blips(T.type2, 20, N.m12); blips(T.type3, 18, N.m21); blips(T.type4, 18, N.m31); blips(T.type5, 18, N.m32);
  add(T.ovOpen - 0.02, 'whoosh-in', 0.55); add(T.ov2 - 0.02, 'whoosh-in', 0.55);
  add(T.saved1, 'chime', 0.6); add(T.saved2, 'chime', 0.6);
  add(T.pop1, 'pop', 0.7); add(T.pop1 + 0.1, 'swish', 0.5); add(T.pop2, 'pop', 0.7); add(T.pop2 + 0.1, 'swish', 0.5);
  add(T.focus, 'whoosh-in', 0.5); add(T.flip0 + 0.1, 'swish', 0.6); add(T.cardOut, 'whoosh-out', 0.5);
  add(T.scan, 'swish', 0.35); add(T.rt0, 'whoosh-in', 0.45); add(T.rt1 - 0.3, 'whoosh-out', 0.4);
  add(T.br0, 'whoosh-in', 0.5); add(T.click, 'click', 0.7); add(T.dl1, 'notify', 0.55); add(T.dl1 + 0.05, 'swish', 0.55);
  add(T.inst1, 'confirm', 0.55); add(T.tapOv, 'click', 0.6); add(T.done, 'chime', 0.6); add(T.home2, 'notify', 0.45);
  add(T.tapGame, 'click', 0.6); add(T.tapStart, 'confirm', 0.55); add(T.swipe, 'swish', 0.4); add(T.tapOv2, 'click', 0.6);
  add(T.tapStart2, 'confirm', 0.55); add(T.x9 + 0.05, 'confirm', 0.5); add(T.swipe2, 'swish', 0.4); add(T.tapCont, 'confirm', 0.55);
  add(T.star0, 'chime', 0.45);
  sfx.sort((a, b) => a.t - b.t);

  // portrait only: the popup's word, large, under the device (the video's own call-out of the lookup)
  const CALLOUTS = [
    { t0: T.ovOpen + 0.1, t1: T.r1 + 0.03, word: 'また明日', reading: 'またあした', mean: 'see you tomorrow' },
    { t0: T.r1land, t1: T.r2 + 0.03, word: '一緒に', reading: 'いっしょに', mean: 'together (with)' },
    { t0: T.r2 + 0.06, t1: T.focus, word: '見に行く', reading: 'みにいく', mean: 'to go to see' },
    { t0: T.ov2 + 0.1, t1: T.r3 + 0.03, word: 'キラキラ', reading: '', mean: 'glittering, sparkling' },
    { t0: T.r3 + 0.06, t1: T.r4 + 0.03, word: '子供の頃', reading: 'こどものころ', mean: "(time of) one's childhood" },
    { t0: T.r4 + 0.06, t1: T.ps2, word: 'みたい', reading: '', mean: '-like, similar to' },
  ];
  function calloutAt(t) { return CALLOUTS.find((c) => t >= c.t0 && t < c.t1 + 0.25) || null; }

  const captionList = CAPTIONS.map((c) => ({ ...c }));
  const steps = STEPS.map((s) => ({ ...s })); steps.note = STEPS.note; steps.window = STEPS.window;

  return {
    duration: D, sfx, captionList, mockups: MOCKUPS,
    apply(t) {
      // camera
      const c = keyed(CAMK, t);
      const w = c.w, tgt = c.tgt;
      const az = (c.az + sway(t, 1) * 0.9) * DEG, el = (c.el + sway(t, 2) * 0.7) * DEG;
      const dist = (w / 2) / htan;
      camera.fov = vfov; camera.aspect = S.W / S.H; camera.updateProjectionMatrix();
      camera.position.set(tgt[0] + dist * Math.cos(el) * Math.sin(az), tgt[1] + dist * Math.sin(el), tgt[2] + dist * Math.cos(el) * Math.cos(az));
      camera.up.set(Math.sin(sway(t, 3, 0.2) * 0.008), 1, 0);
      camera.lookAt(tgt[0] + sway(t, 4) * 0.6, tgt[1] + sway(t, 5) * 0.5, tgt[2]);
      // the device floats a little, as if held
      device.root.rotation.set(sway(t, 6, 0.3) * 0.012, sway(t, 7, 0.3) * 0.016, sway(t, 8, 0.25) * 0.006);
      device.root.position.set(0, sway(t, 9, 0.3) * 1.2, 0);
      for (const b of BUTTONS) {
        let age = -1;
        for (const p of PRESSES) if ((p.btn === b || (p.btns && p.btns.includes(b))) && t >= p.t && t - p.t < 0.6) age = t - p.t;
        device.setPress(b, pressAmt(PRESSES, b, t), age);
      }
      device.setStick('stick-l', 0, 0);
      drawScreen(t);
      panelsAt(t);
      // the shooting star at the end, across the backdrop
      const ks = inv(T.star0, T.star0 + 0.75, t);
      S.star.material.opacity = ks > 0 && ks < 1 ? Math.sin(ks * Math.PI) * 0.9 : 0;
      S.star.position.set(lerp(-700, 900, ks), lerp(420, 120, ks), -900); S.star.rotation.z = -0.19;
      const pal = palette(t);
      const fade = t > D - 0.45 ? inv(D - 0.45, D, t) : t < 0.12 ? 1 - inv(0, 0.12, t) : 0;
      return { palette: pal, captions: captionList, callout: P ? calloutAt(t) : null, steps: t >= STEPS.window.t0 - 0.1 && t <= STEPS.window.t1 + 0.5 ? steps : null, end: t >= T.end ? T.end + 0.05 : null, vignette: 0.32, fade };
    },
  };
}
