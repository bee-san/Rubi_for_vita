// Captions: the owner's script lines, drawn over the live action. Words rise in one after another and
// the line leaves with a short fade; nothing here is a title card.
import { clamp, easeOut, easeIn } from './util.js';

const FONT = '"Noto Sans JP", sans-serif';

function wrap(g, words, maxW) {
  const lines = []; let cur = [];
  for (const w of words) {
    const test = [...cur, w].join(' ');
    if (cur.length && g.measureText(test).width > maxW) { lines.push(cur); cur = [w]; } else cur.push(w);
  }
  if (cur.length) lines.push(cur);
  return lines;
}

// cap: {t0, t1, text, size?, accent?: [words to colour], pos?: 'bottom'|'top'|'mid'}
export function drawCaption(g, cap, t, cut) {
  const W = g.__W ?? g.canvas.width, H = g.__H ?? g.canvas.height;
  if (t < cap.t0 - 0.01 || t > cap.t1 + 0.18) return;
  const portrait = cut === '9x16';
  const size = cap.size ?? (portrait ? 62 : 70);
  g.save();
  g.font = `800 ${size}px ${FONT}`; g.textBaseline = 'alphabetic';
  const words = cap.text.split(' ');
  const maxW = portrait ? W - 90 : Math.min(1500, W - 260);
  const lines = wrap(g, words, maxW);
  const lh = size * 1.18;
  const blockH = lines.length * lh;
  let y0;
  const pos = cap.pos ?? 'bottom';
  if (portrait) y0 = pos === 'low' ? 1240 : 250; // portrait: above the device, clear of the top 220 px and bottom 400 px
  else y0 = pos === 'top' ? 90 : H - 70 - blockH;
  // a soft scrim behind the block for legibility over any background
  const outK = clamp((t - cap.t1) / 0.18);
  const inK = clamp((t - cap.t0) / 0.25);
  const blockA = Math.min(inK, 1 - outK);
  const sc = g.createLinearGradient(0, y0 - 60, 0, y0 + blockH + 60);
  sc.addColorStop(0, 'rgba(5,8,16,0)'); sc.addColorStop(0.35, 'rgba(5,8,16,0.38)'); sc.addColorStop(0.65, 'rgba(5,8,16,0.38)'); sc.addColorStop(1, 'rgba(5,8,16,0)');
  g.globalAlpha = blockA * 0.9; g.fillStyle = sc; g.fillRect(0, y0 - 60, W, blockH + 120);
  let wi = 0;
  lines.forEach((ln, li) => {
    const lw = g.measureText(ln.join(' ')).width;
    let x = (W - lw) / 2;
    const y = y0 + (li + 1) * lh - size * 0.22;
    for (const w of ln) {
      const k = clamp((t - cap.t0 - wi * 0.07) / 0.32);
      const a = easeOut(k) * (1 - easeIn(outK));
      const dy = (1 - easeOut(k)) * size * 0.45 - easeIn(outK) * size * 0.2;
      g.globalAlpha = a;
      g.shadowColor = 'rgba(0,0,0,0.55)'; g.shadowBlur = size * 0.22; g.shadowOffsetY = size * 0.04;
      const acc = cap.accent && cap.accent.some((s) => w.replace(/[,.:!]/g, '') === s);
      g.fillStyle = acc ? '#7ff0e3' : '#ffffff';
      g.fillText(w, x, y + dy);
      x += g.measureText(w + ' ').width;
      wi++;
    }
  });
  g.restore();
}

// A tick, drawn (fonts differ in whether they have one)
function tick(g, x, y, r, color) {
  g.save(); g.strokeStyle = color; g.lineWidth = r * 0.34; g.lineCap = 'round'; g.lineJoin = 'round';
  g.beginPath(); g.moveTo(x - r * 0.48, y + r * 0.02); g.lineTo(x - r * 0.12, y + r * 0.38); g.lineTo(x + r * 0.52, y - r * 0.36); g.stroke(); g.restore();
}
function stepChip(g, s, i, t, x, yc, size, a) {
  const done = s.done != null && t >= s.done;
  g.globalAlpha = a;
  g.font = `800 ${size}px ${FONT}`;
  const tw = g.measureText(s.text).width, h = size * 1.5;
  g.fillStyle = 'rgba(8,14,24,0.8)'; g.beginPath(); g.roundRect(x, yc - h / 2, tw + size * 2.4, h, h / 2); g.fill();
  const cx = x + h / 2 + size * 0.1, r = size * 0.56;
  const pop = done ? 1 + 0.25 * Math.sin(Math.PI * clamp((t - s.done) / 0.25)) : 1;
  g.fillStyle = done ? '#5bdccf' : 'rgba(255,255,255,0.2)'; g.beginPath(); g.arc(cx, yc, r * pop, 0, Math.PI * 2); g.fill();
  if (done) tick(g, cx, yc, r * pop, '#06201d');
  else { g.fillStyle = '#fff'; g.textAlign = 'center'; g.textBaseline = 'middle'; g.font = `800 ${Math.round(size * 0.72)}px ${FONT}`; g.fillText(String(i + 1), cx, yc + size * 0.04); }
  g.textAlign = 'left'; g.textBaseline = 'middle'; g.font = `800 ${size}px ${FONT}`; g.fillStyle = '#fff';
  g.fillText(s.text, x + h + size * 0.35, yc + size * 0.04);
  return tw + size * 2.4;
}

// The install steps. 16:9: the current step, large, just above the caption, sliding up as it changes.
// 9:16: the list under the console, each step ticked off when it is done.
export function drawSteps(g, steps, t, cut, { t0, t1 }) {
  if (t < t0 || t > t1 + 0.4) return;
  const portrait = cut === '9x16';
  const W = g.__W ?? g.canvas.width, H = g.__H ?? g.canvas.height;
  const outK = clamp((t - t1) / 0.4);
  g.save();
  if (!portrait) {
    let cur = -1; steps.forEach((s, i) => { if (t >= s.t) cur = i; });
    if (cur < 0) { g.restore(); return; }
    const size = 58, yc = H - 70 - 99 - 30 - size * 0.75;
    const draw = (i, a, dy) => {
      g.font = `800 ${size}px ${FONT}`;
      const w = g.measureText(steps[i].text).width + size * 2.4;
      stepChip(g, steps[i], i, t, (W - w) / 2, yc + dy, size, a);
    };
    const k = easeOut(clamp((t - steps[cur].t) / 0.28));
    if (cur > 0 && k < 1) draw(cur - 1, (1 - k) * (1 - outK), -k * size * 1.2);
    draw(cur, k * (1 - outK), (1 - k) * size * 1.1);
  } else {
    const x0 = 60, y0 = 1070, gap = 86;
    steps.forEach((s, i) => {
      const k = clamp((t - s.t) / 0.3);
      if (k <= 0) return;
      let size = 46; g.font = `800 ${size}px ${FONT}`;
      const maxW = W - 2 * x0 - size * 2.4; const tw = g.measureText(s.text).width;
      if (tw > maxW) size = Math.floor(size * maxW / tw);
      stepChip(g, s, i, t, x0 - (1 - easeOut(k)) * 40, y0 + i * gap, size, easeOut(k) * (1 - outK));
    });
  }
  g.restore();
}

// The end card's type, over the device.
export function drawEnd(g, t, cut, t0) {
  if (t < t0) return;
  const portrait = cut === '9x16';
  const W = g.__W ?? g.canvas.width, H = g.__H ?? g.canvas.height;
  const k = (d) => easeOut(clamp((t - t0 - d) / 0.45));
  g.save(); g.textAlign = 'center'; g.textBaseline = 'alphabetic';
  g.shadowColor = 'rgba(0,0,0,0.5)'; g.shadowBlur = 18;
  const yT = portrait ? 420 : 150;
  g.globalAlpha = k(0); g.fillStyle = '#fff'; g.font = `900 ${portrait ? 104 : 96}px ${FONT}`;
  g.fillText('Vita JP Overlay', W / 2, yT + (1 - k(0)) * 30);
  g.globalAlpha = k(0.15); g.fillStyle = '#7ff0e3'; g.font = `700 ${portrait ? 50 : 44}px ${FONT}`;
  g.fillText('Free · Offline · Open source', W / 2, yT + (portrait ? 84 : 72) + (1 - k(0.15)) * 24);
  g.globalAlpha = k(0.3); g.fillStyle = '#e8eef7'; g.font = `600 ${portrait ? 40 : 36}px ${FONT}`;
  g.fillText('github.com/bee-san/vita-jp-overlay', W / 2, portrait ? 1376 : H - 128);
  g.globalAlpha = k(0.45) * 0.8; g.fillStyle = '#c3cfdd'; g.font = `500 ${portrait ? 24 : 22}px ${FONT}`;
  g.shadowBlur = 6;
  const fine = 'Dictionary: JMdict (EDRDG, CC BY-SA 4.0) · Frequency ranks: Jiten (CC BY-SA 4.0) · Cards: Lapis note type (GPL-3.0) · The game shown is an original mock-up';
  const sony = 'Not affiliated with or endorsed by Sony Interactive Entertainment.';
  if (portrait) {
    const half = fine.split(' · ');
    g.fillText(half.slice(0, 2).join(' · '), W / 2, 1436); g.fillText(half.slice(2).join(' · '), W / 2, 1470); g.fillText(sony, W / 2, 1504);
  } else { g.fillText(fine, W / 2, H - 78); g.fillText(sony, W / 2, H - 46); }
  g.restore();
}

// Portrait: the looked-up word, large, under the device (the video's own call-out; the popup above is
// the real overlay).
export function drawCallout(g, c, t) {
  if (!c) return;
  const W = g.__W ?? g.canvas.width;
  const k = easeOut(clamp((t - c.t0) / 0.25)), o = clamp((t - c.t1) / 0.25);
  const a = k * (1 - o);
  if (a <= 0) return;
  g.save(); g.textAlign = 'center'; g.globalAlpha = a;
  const y = 1296 + (1 - k) * 30;
  g.fillStyle = 'rgba(8,12,22,0.55)'; const bh = c.note && t >= c.note.t ? 350 : 290; g.beginPath(); g.roundRect(90, y - 120, W - 180, bh, 36); g.fill();
  g.shadowColor = 'rgba(0,0,0,0.5)'; g.shadowBlur = 12;
  g.fillStyle = '#ffd24a'; g.font = `800 96px ${FONT}`; g.fillText(c.word, W / 2, y);
  if (c.reading) { g.fillStyle = '#9fd0ff'; g.font = `600 44px ${FONT}`; g.fillText(c.reading, W / 2, y + 64); }
  g.fillStyle = '#e6edf6'; g.font = `600 46px ${FONT}`; g.fillText(c.mean, W / 2, y + (c.reading ? 132 : 92));
  if (c.note && t >= c.note.t) { // what the overlay says after ×
    const nk = easeOut(clamp((t - c.note.t) / 0.2));
    g.globalAlpha = a * nk; g.fillStyle = '#7cd992'; g.font = `800 ${Math.round(48 * (0.85 + 0.15 * nk))}px ${FONT}`;
    g.fillText(c.note.text, W / 2, y + (c.reading ? 200 : 160));
  }
  g.restore();
}
