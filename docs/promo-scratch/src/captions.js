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

// Small step chips (the install steps): a vertical list that builds up.
export function drawSteps(g, steps, t, cut, { t0, t1 }) {
  if (t < t0 || t > t1 + 0.4) return;
  const portrait = cut === '9x16';
  const W = g.__W ?? g.canvas.width;
  const size = portrait ? 40 : 34;
  const x0 = portrait ? 90 : 80, y0 = portrait ? 1080 : 210, gap = portrait ? 74 : 64;
  const outK = clamp((t - t1) / 0.4);
  g.save(); g.font = `700 ${size}px ${FONT}`; g.textBaseline = 'middle';
  steps.forEach((s, i) => {
    const k = clamp((t - s.t) / 0.3);
    if (k <= 0) return;
    const done = s.done != null && t >= s.done;
    const a = easeOut(k) * (1 - outK);
    const y = y0 + i * gap, x = x0 - (1 - easeOut(k)) * 40;
    g.globalAlpha = a;
    const tw = g.measureText(s.text).width;
    g.fillStyle = 'rgba(8,14,24,0.72)';
    g.beginPath(); g.roundRect(x, y - gap * 0.42, tw + size * 2.6, gap * 0.84, gap * 0.42); g.fill();
    g.fillStyle = done ? '#5bdccf' : 'rgba(255,255,255,0.18)';
    g.beginPath(); g.arc(x + size * 0.95, y, size * 0.62, 0, Math.PI * 2); g.fill();
    g.fillStyle = done ? '#06201d' : '#fff'; g.textAlign = 'center';
    g.fillText(done ? '✓' : String(i + 1), x + size * 0.95, y + 1);
    g.textAlign = 'left'; g.fillStyle = '#fff';
    g.fillText(s.text, x + size * 1.95, y + 1);
  });
  if (steps.note) {
    const k = clamp((t - steps.note.t) / 0.3) * (1 - outK);
    g.globalAlpha = k * 0.9; g.font = `500 ${portrait ? 28 : 24}px ${FONT}`; g.fillStyle = '#c9d6e6';
    g.fillText(steps.note.text, x0 + 6, y0 + steps.length * gap + (portrait ? 10 : 4));
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
  g.fillText('github.com/bee-san/vita-jp-overlay', W / 2, portrait ? 1420 : H - 120);
  g.globalAlpha = k(0.45) * 0.8; g.fillStyle = '#c3cfdd'; g.font = `500 ${portrait ? 24 : 22}px ${FONT}`;
  g.shadowBlur = 6;
  const fine = 'Dictionary: JMdict (EDRDG, CC BY-SA 4.0) · Frequency ranks: Jiten (CC BY-SA 4.0) · Cards: Lapis note type (GPL-3.0) · The game shown is an original mock-up';
  if (portrait) {
    const half = fine.split(' · ');
    g.fillText(half.slice(0, 2).join(' · '), W / 2, 1470); g.fillText(half.slice(2).join(' · '), W / 2, 1506);
  } else g.fillText(fine, W / 2, H - 70);
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
  const y = 1250 + (1 - k) * 30;
  g.fillStyle = 'rgba(8,12,22,0.55)'; g.beginPath(); g.roundRect(90, y - 120, W - 180, 290, 36); g.fill();
  g.shadowColor = 'rgba(0,0,0,0.5)'; g.shadowBlur = 12;
  g.fillStyle = '#ffd24a'; g.font = `800 96px ${FONT}`; g.fillText(c.word, W / 2, y);
  if (c.reading) { g.fillStyle = '#9fd0ff'; g.font = `600 44px ${FONT}`; g.fillText(c.reading, W / 2, y + 64); }
  g.fillStyle = '#e6edf6'; g.font = `600 46px ${FONT}`; g.fillText(c.mean, W / 2, y + (c.reading ? 132 : 92));
  g.restore();
}
