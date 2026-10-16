// The Vita's screen: a 1920x1088 canvas (2x the console's 960x544) drawn every frame and used as the
// device's screen texture. Everything here is either a real capture/layer (the VN layers, the overlay
// layers from main's own layout and lookups, the Overlay app's Vita3K captures) or a clearly simplified
// original rendition of the console's own menus (home screen, LiveArea). Mock-ups of features that are
// still landing (VitaShell's install, the Overlay app's Install page) are drawn from their branches'
// strings until real captures arrive; they are listed in README.md.
export const SW = 1920, SH = 1088, K = 2; // K: canvas px per console px

const FONT = '"Noto Sans JP", sans-serif';
export const APP = { bg: 'rgb(9,15,23)', card: 'rgb(21,31,44)', sel: 'rgb(35,56,70)', white: 'rgb(238,244,255)', dim: 'rgb(193,207,222)', accent: 'rgb(91,220,207)', off: 'rgb(244,160,167)' };

export function rr(g, x, y, w, h, r) {
  g.beginPath(); g.moveTo(x + r, y); g.arcTo(x + w, y, x + w, y + h, r); g.arcTo(x + w, y + h, x, y + h, r);
  g.arcTo(x, y + h, x, y, r); g.arcTo(x, y, x + w, y, r); g.closePath();
}

export class Screen {
  constructor(img, json) {
    this.img = img; this.json = json;
    this.canvas = document.createElement('canvas'); this.canvas.width = SW; this.canvas.height = SH;
    this.g = this.canvas.getContext('2d');
  }
  clear(c = '#000') { const g = this.g; g.globalAlpha = 1; g.fillStyle = c; g.fillRect(0, 0, SW, SH); }

  // ---- the game: a message of the original mock VN, typed character by character
  vn(msg, typed, { next = false, t = 0, alpha = 1 } = {}) {
    const g = this.g, [s] = msg.split('-');
    g.save(); g.globalAlpha = alpha;
    g.drawImage(this.img[`vn/${s}/${msg}-notext@2x.png`], 0, 0, SW, SH);
    const chars = this.json[`vn/${s}/${msg}-text.json`].chars;
    const text = this.img[`vn/${s}/${msg}-text@2x.png`];
    const n = Math.min(chars.length, Math.max(0, typed));
    const full = Math.floor(n), frac = n - full;
    const box = (c) => [c.x * K - 3, c.y * K - 6, c.w * K + 6, c.h * K + 12];
    if (full > 0) {
      g.save(); g.beginPath();
      for (let i = 0; i < full; i++) g.rect(...box(chars[i]));
      g.clip(); g.drawImage(text, 0, 0, SW, SH); g.restore();
    }
    if (frac > 0 && full < chars.length) {
      g.save(); g.beginPath(); g.rect(...box(chars[full])); g.clip(); g.globalAlpha = alpha * frac;
      g.drawImage(text, 0, (1 - frac) * 6, SW, SH); g.restore();
    }
    if (next && n >= chars.length) { // the game's own "next" mark, bobbing
      const bob = Math.sin(t * Math.PI * 2 * 1.4) * 3;
      g.fillStyle = '#f5e7cc'; g.shadowColor = 'rgba(0,0,0,0.45)'; g.shadowBlur = 6;
      g.beginPath(); g.moveTo(1756, 1006 + bob); g.lineTo(1784, 1006 + bob); g.lineTo(1770, 1026 + bob); g.closePath(); g.fill();
      g.shadowBlur = 0;
    }
    g.restore();
  }
  // the whole game frame as it was (used for the card's screenshot and for crossfades)
  frame(name, alpha = 1) { const g = this.g; g.save(); g.globalAlpha = alpha; g.drawImage(this.img[name], 0, 0, SW, SH); g.restore(); }

  // ---- the overlay: main's layout per beat, a transparent full-screen layer (90 % black panel)
  overlay(beat, alpha = 1, dy = 0) {
    const [s] = beat.split('-'); const g = this.g;
    g.save(); g.globalAlpha = alpha; g.drawImage(this.img[`overlay/${s}/${beat}@2x.png`], 0, dy, SW, SH); g.restore();
  }

  // ---- a capture of the Overlay app (960x544, Vita3K), shown at 2x
  capture(name, alpha = 1) {
    const g = this.g; g.save(); g.globalAlpha = alpha; g.imageSmoothingQuality = 'high';
    g.drawImage(this.img[name], 0, 0, SW, SH); g.restore();
  }

  // ---- the touch indicator (the video's own: a soft ring where a finger taps)
  ripple(x, y, k) {
    if (k <= 0 || k >= 1) return;
    const g = this.g, r = 30 + 70 * k;
    g.save(); g.globalAlpha = (1 - k) * 0.85; g.strokeStyle = '#fff'; g.lineWidth = 8 * (1 - k) + 2;
    g.beginPath(); g.arc(x, y, r, 0, Math.PI * 2); g.stroke();
    g.globalAlpha = (1 - k) * 0.35; g.fillStyle = '#fff'; g.beginPath(); g.arc(x, y, 34 * (1 - k * 0.5), 0, Math.PI * 2); g.fill();
    g.restore();
  }

  statusBar(time = '21:04', { dark = false } = {}) {
    const g = this.g; g.save();
    g.fillStyle = dark ? 'rgba(0,0,0,0.35)' : 'rgba(0,0,0,0.18)'; g.fillRect(0, 0, SW, 56);
    g.fillStyle = '#fff'; g.font = `500 30px ${FONT}`; g.textAlign = 'right'; g.textBaseline = 'middle';
    g.fillText(time, SW - 40, 29);
    // battery
    g.strokeStyle = '#fff'; g.lineWidth = 3; rr(g, SW - 196, 16, 50, 26, 5); g.stroke();
    g.fillRect(SW - 146, 23, 5, 12); g.fillStyle = '#8ef0b0'; g.fillRect(SW - 191, 21, 36, 16);
    g.restore();
  }

  // ---- the home screen: a simplified, original rendition (bubbles of the installed titles)
  home({ icons, focus = -1, pressK = 0, scroll = 0, running = 0, alpha = 1 } = {}) {
    const g = this.g; g.save(); g.globalAlpha = alpha;
    const bg = g.createLinearGradient(0, 0, SW, SH);
    bg.addColorStop(0, '#1d2f55'); bg.addColorStop(0.55, '#3a4f86'); bg.addColorStop(1, '#6b7fb4');
    g.fillStyle = bg; g.fillRect(0, 0, SW, SH);
    // soft light bands
    g.globalAlpha = alpha * 0.12; g.fillStyle = '#fff';
    for (let i = 0; i < 4; i++) { g.beginPath(); g.ellipse(300 + i * 480 + scroll * 0.3, 1200 - i * 90, 700, 160, -0.35, 0, Math.PI * 2); g.fill(); }
    g.globalAlpha = alpha;
    const pos = [[330, 330], [750, 330], [1170, 330], [1590, 330], [540, 700], [960, 700], [1380, 700]];
    icons.forEach((ic, i) => {
      if (!ic || i >= pos.length) return;
      let [x, y] = pos[i]; x += scroll;
      const sc = i === focus ? 1 + 0.1 * Math.sin(Math.min(1, pressK) * Math.PI) : 1;
      const R = 112 * sc;
      g.save(); g.shadowColor = 'rgba(0,0,0,0.35)'; g.shadowBlur = 24; g.shadowOffsetY = 10;
      g.fillStyle = '#fff'; g.beginPath(); g.arc(x, y, R + 6, 0, Math.PI * 2); g.fill(); g.restore();
      g.save(); g.beginPath(); g.arc(x, y, R, 0, Math.PI * 2); g.clip();
      g.drawImage(ic, x - R, y - R, R * 2, R * 2);
      const gl = g.createLinearGradient(x, y - R, x, y + R); gl.addColorStop(0, 'rgba(255,255,255,0.32)'); gl.addColorStop(0.5, 'rgba(255,255,255,0)');
      g.fillStyle = gl; g.fillRect(x - R, y - R, R * 2, R * 2); g.restore();
      if (i === running) { // the running app's mark
        g.fillStyle = '#7cf0ff'; g.beginPath(); g.arc(x + R * 0.72, y - R * 0.72, 16, 0, Math.PI * 2); g.fill();
      }
    });
    // page dots
    for (let i = 0; i < 3; i++) { g.fillStyle = i === 0 ? '#fff' : 'rgba(255,255,255,0.45)'; g.beginPath(); g.arc(SW - 50, 420 + i * 60, 11, 0, Math.PI * 2); g.fill(); }
    g.restore();
    this.statusBar();
  }

  // ---- a LiveArea page: a simplified, original rendition (art, title, the gate)
  liveArea({ art, artRect = null, icon, title, gate = 'Start', gateImg = null, pressK = 0, alpha = 1, slide = 0 }) {
    const g = this.g; g.save(); g.globalAlpha = alpha; g.translate(slide, 0);
    g.fillStyle = '#0d1424'; g.fillRect(-SW, 0, SW * 3, SH);
    // the page's frame
    g.save(); rr(g, 70, 100, 1780, 950, 36); g.clip();
    if (art) { if (artRect) g.drawImage(art, ...artRect, 70, 100, 1780, 950); else g.drawImage(art, 70, 100, 1780, 950); }
    const v = g.createLinearGradient(0, 100, 0, 1050); v.addColorStop(0, 'rgba(0,0,0,0.45)'); v.addColorStop(0.3, 'rgba(0,0,0,0)'); v.addColorStop(1, 'rgba(0,0,0,0.25)');
    g.fillStyle = v; g.fillRect(70, 100, 1780, 950);
    g.restore();
    // title row
    if (icon) { g.save(); g.beginPath(); g.arc(160, 180, 56, 0, Math.PI * 2); g.clip(); g.drawImage(icon, 104, 124, 112, 112); g.restore(); }
    g.fillStyle = '#fff'; g.font = `700 50px ${FONT}`; g.textBaseline = 'middle'; g.textAlign = 'left';
    g.shadowColor = 'rgba(0,0,0,0.6)'; g.shadowBlur = 10; g.fillText(title, 240, 182); g.shadowBlur = 0;
    // the gate
    const gs = 1 - 0.05 * Math.sin(Math.min(1, pressK) * Math.PI);
    g.save(); g.translate(1520, 600); g.scale(gs, gs);
    g.shadowColor = 'rgba(0,0,0,0.5)'; g.shadowBlur = 30; g.fillStyle = '#fff'; rr(g, -230, -250, 460, 500, 40); g.fill(); g.shadowBlur = 0;
    g.save(); rr(g, -218, -238, 436, 380, 32); g.clip();
    if (gateImg) g.drawImage(gateImg, -218, -238, 436, 380); else { g.fillStyle = '#26324a'; g.fillRect(-218, -238, 436, 380); }
    g.restore();
    g.fillStyle = '#1b2333'; g.font = `700 54px ${FONT}`; g.textAlign = 'center'; g.fillText(gate, 0, 196);
    g.restore();
    g.restore();
    this.statusBar('21:04', { dark: true });
  }

  black(a = 1) { if (a <= 0) return; const g = this.g; g.save(); g.globalAlpha = a; g.fillStyle = '#000'; g.fillRect(0, 0, SW, SH); g.restore(); }
}
