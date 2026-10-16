// Floating panels (drawn into canvas textures of 3D planes) and the VitaShell stand-in. Every mock-up is
// listed in README.md and in the preview comment: the release page follows docs/one-vpk-install.md
// ("What a release ships"); VitaShell's strings are VitaShell 2.02's (resources/english_us.txt).
import { rr, SW, SH } from './screen.js';
import { clamp, easeOut } from './util.js';

const FONT = '"Noto Sans JP", sans-serif';

// ---- an Anki card face, cropped from Anki's own render of the Lapis note
export function drawCard(g, img, crop, { badge = null, hl = null } = {}) {
  const W = g.canvas.width, H = g.canvas.height;
  g.clearRect(0, 0, W, H);
  g.save(); rr(g, 0, 0, W, H, 28); g.clip();
  g.fillStyle = '#fffaf0'; g.fillRect(0, 0, W, H);
  g.drawImage(img, ...crop, 0, 0, W, H);
  g.restore();
  g.strokeStyle = 'rgba(0,0,0,0.10)'; g.lineWidth = 3; rr(g, 1.5, 1.5, W - 3, H - 3, 27); g.stroke();
  if (hl && hl.k > 0) { // the video's own highlight round the card's picture
    const [x, y, w, h] = hl.rect, pad = 10 + 6 * (1 - hl.k);
    g.save(); g.globalAlpha = Math.min(1, hl.k * 1.2); g.strokeStyle = '#2fd3c3'; g.lineWidth = 14;
    g.shadowColor = 'rgba(47,211,195,0.85)'; g.shadowBlur = 26;
    rr(g, x - pad, y - pad, w + pad * 2, h + pad * 2, 18); g.stroke(); g.restore();
  }
  if (badge) { // the video's own label: which kind of shot this is
    g.font = `700 30px ${FONT}`; const tw = g.measureText(badge).width;
    g.fillStyle = 'rgba(20,28,40,0.78)'; rr(g, 24, H - 66, tw + 36, 46, 23); g.fill();
    g.fillStyle = '#fff'; g.textBaseline = 'middle'; g.fillText(badge, 42, H - 42);
  }
}

// ---- the release page on a computer: a generic browser (no browser or site logos)
export const RELEASE = {
  version: '0.8.0',
  // the design's assets (docs/one-vpk-install.md "What a release ships"), about 53,000,000 B each, which
  // GitHub shows in 1024-based units; SHA256SUMS is four lines of hash, two spaces and name (372 B)
  assets: [['VitaJPOverlay-0.8.0.vpk', '50.6 MB'], ['VitaJPOverlay-0.8.0-debug.vpk', '50.6 MB'], ['VitaJPOverlay-0.8.0.zip', '50.6 MB'], ['VitaJPOverlay-0.8.0-debug.zip', '50.6 MB'], ['SHA256SUMS', '372 Bytes'], ['Source code (zip)', ''], ['Source code (tar.gz)', '']],
};
export function drawBrowser(g, { pointer = null, click = 0, hover = false, dl = 0 }) {
  const W = g.canvas.width, H = g.canvas.height;
  g.clearRect(0, 0, W, H);
  g.save(); rr(g, 0, 0, W, H, 30); g.clip();
  g.fillStyle = '#f6f8fa'; g.fillRect(0, 0, W, H);
  // window chrome
  g.fillStyle = '#e4e8ee'; g.fillRect(0, 0, W, 96);
  ['#ff6159', '#ffbd2e', '#28c941'].forEach((c, i) => { g.fillStyle = c; g.beginPath(); g.arc(44 + i * 40, 48, 13, 0, Math.PI * 2); g.fill(); });
  g.fillStyle = '#fff'; rr(g, 180, 22, W - 220, 52, 26); g.fill();
  g.fillStyle = '#57606a'; g.font = `500 30px ${FONT}`; g.textBaseline = 'middle';
  g.fillText('github.com/bee-san/vita-jp-overlay/releases', 214, 49);
  // page
  g.fillStyle = '#1f2328'; g.font = `700 50px ${FONT}`; g.fillText('Releases', 60, 170);
  g.fillStyle = '#fff'; g.strokeStyle = '#d0d7de'; g.lineWidth = 3; rr(g, 50, 220, W - 100, H - 270, 18); g.fill(); g.stroke();
  g.fillStyle = '#1f2328'; g.font = `700 46px ${FONT}`; g.fillText(`v${RELEASE.version}`, 90, 290);
  const vw = g.measureText(`v${RELEASE.version}`).width;
  g.strokeStyle = '#1a7f37'; g.lineWidth = 3; rr(g, 110 + vw, 266, 120, 48, 24); g.stroke();
  g.fillStyle = '#1a7f37'; g.font = `600 28px ${FONT}`; g.fillText('Latest', 130 + vw, 291);
  g.fillStyle = '#1f2328'; g.font = `700 32px ${FONT}`; g.fillText(`Assets  ${RELEASE.assets.length}`, 90, 380);
  RELEASE.assets.forEach(([n, sz], i) => {
    const y = 434 + i * 62;
    if (i === 0 && hover) { g.fillStyle = '#eef6ff'; g.fillRect(60, y - 32, W - 120, 64); }
    g.fillStyle = '#57606a'; rr(g, 92, y - 18, 28, 36, 4); g.fill(); g.fillStyle = '#fff'; g.fillRect(98, y - 8, 16, 3); g.fillRect(98, y, 16, 3);
    g.fillStyle = i === 0 ? '#0969da' : '#1f2328'; g.font = `${i === 0 ? 700 : 500} 32px ${FONT}`;
    g.fillText(n, 140, y + 2);
    g.fillStyle = '#57606a'; g.font = `500 28px ${FONT}`; g.textAlign = 'right'; g.fillText(sz, W - 90, y + 2); g.textAlign = 'left';
  });
  // the download bar
  if (dl > 0) {
    const k = easeOut(clamp(dl * 3));
    g.fillStyle = 'rgba(31,35,40,0.92)'; rr(g, 60, H - 150 + (1 - k) * 60, W - 120, 104, 20); g.fill();
    g.fillStyle = '#fff'; g.font = `600 30px ${FONT}`; g.fillText('VitaJPOverlay-0.8.0.vpk', 100, H - 112 + (1 - k) * 60);
    g.fillStyle = 'rgba(255,255,255,0.25)'; rr(g, 100, H - 82 + (1 - k) * 60, W - 320, 14, 7); g.fill();
    g.fillStyle = '#4ac26b'; rr(g, 100, H - 82 + (1 - k) * 60, (W - 320) * clamp(dl), 14, 7); g.fill();
    g.fillStyle = '#c9d1d9'; g.font = `500 26px ${FONT}`; g.textAlign = 'right';
    g.fillText(dl >= 1 ? '50.6 MB · done' : `${(50.6 * clamp(dl)).toFixed(1)} of 50.6 MB`, W - 90, H - 104 + (1 - k) * 60); g.textAlign = 'left';
  }
  g.restore();
  if (pointer) { // an arrow pointer
    const [x, y] = pointer, s = 1 - 0.12 * Math.sin(clamp(click) * Math.PI);
    g.save(); g.translate(x, y); g.scale(s * 1.5, s * 1.5);
    g.fillStyle = '#111'; g.strokeStyle = '#fff'; g.lineWidth = 3;
    g.beginPath(); g.moveTo(0, 0); g.lineTo(0, 34); g.lineTo(9, 26); g.lineTo(16, 41); g.lineTo(22, 38); g.lineTo(15, 23); g.lineTo(27, 23); g.closePath(); g.stroke(); g.fill();
    g.restore();
  }
}

export function drawFileChip(g) {
  const W = g.canvas.width, H = g.canvas.height;
  g.clearRect(0, 0, W, H);
  g.fillStyle = '#ffffff'; rr(g, 4, 4, W - 8, H - 8, 22); g.fill();
  g.fillStyle = '#ff8a1f'; rr(g, 24, 22, 60, H - 44, 8); g.fill();
  g.fillStyle = '#fff'; g.font = `800 26px ${FONT}`; g.textAlign = 'center'; g.textBaseline = 'middle'; g.fillText('VPK', 54, H / 2 + 1);
  g.textAlign = 'left'; g.fillStyle = '#1f2328'; g.font = `700 34px ${FONT}`; g.fillText('VitaJPOverlay-0.8.0.vpk', 104, H / 2 + 1);
}

// ---- VitaShell 2.02, Default theme: a stand-in until promo-assets' capture lands. Colours from
// resources/default/colors.txt (ABGR), layout from main.h/main.c/browser.c (SHELL_MARGIN 20/18, rows of
// FONT_Y_SPACE 23 from START_Y 87, icons at x 20, names at FILE_X 46, sizes right-aligned at
// INFORMATION_X 680, dates at 940), the theme's own icons; Noto Sans JP stands in for the PGF font.
const VS = {
  bg: '#000000', title: '#7f00ff', path: '#bfbfbf', date: '#ffffff', focus: '#00ff00', file: '#ffffff', folder: '#00ffff',
  archive: '#ff7f00', dialog: '#ffffff', dialogBg: '#2f2f2f', bar: '#7fff00', barBg: '#7f7f7f',
};
export function drawVitaShell(screen, img, { rows, focus = 0, dialog = null, progress = 0, open = 1, ftp = true, flash = 0 }) {
  const g = screen.g, X = (v) => v * 2; // console px -> canvas px
  const text = (str, x, y, color, align = 'left') => { g.fillStyle = color; g.textAlign = align; g.fillText(str, X(x), X(y)); };
  g.save(); g.fillStyle = VS.bg; g.fillRect(0, 0, SW, SH);
  g.font = `400 ${X(19)}px ${FONT}`; g.textBaseline = 'top';
  text('VitaShell 2.02', 20, 18, VS.title);
  // status bar, right to left: battery, date and time, the FTP icon while FTP runs
  const bx = 940 - 40; g.drawImage(img['vitashell/battery.png'], X(bx), X(21), X(40), X(20));
  g.fillStyle = '#00ff00'; g.fillRect(X(bx + 3), X(23), X(30), X(16));
  const dt = '2026/10/16  21:04'; g.font = `400 ${X(19)}px ${FONT}`;
  const dw = g.measureText(dt).width / 2, dx = bx - 12 - dw;
  text(dt, dx, 18, VS.date);
  if (ftp) g.drawImage(img['vitashell/ftp.png'], X(dx - 12 - 18), X(21), X(18), X(20));
  text('ux0:data/', 20, 41, VS.path);
  rows.forEach((r, i) => {
    const y = 87 + i * 23;
    const color = i === focus ? VS.focus : r.kind === 'archive' ? VS.archive : r.kind === 'folder' ? VS.folder : VS.file;
    if (r.kind !== 'up') g.drawImage(img[r.kind === 'archive' ? 'vitashell/archive_icon.png' : 'vitashell/folder_icon.png'], X(20), X(y + 3), X(20), X(20));
    else g.drawImage(img['vitashell/folder_icon.png'], X(20), X(y + 3), X(20), X(20));
    if (r.flash) { g.globalAlpha = r.flash; g.fillStyle = 'rgba(0,255,0,0.25)'; g.fillRect(X(20), X(y + 3), X(920), X(23)); g.globalAlpha = 1; }
    text(r.name, 46, y, color);
    if (r.info) { text(r.info, 680, y, color, 'right'); text(r.date, 940, y, color, 'right'); }
  });
  if (dialog) {
    const lines = {
      ask: ['Do you want to install this package?', '', '╳ Yes    ○ No'],
      perm: ['This package requests extended permissions.', 'It will have access to your personal information.', 'If you did not obtain it from a trusted source,', 'please proceed at your own caution.', '', 'Would you like to continue the install?', '', '╳ Yes    ○ No'],
      progress: ['Installing...', '', '', ''],
    }[dialog];
    const lh = X(23);
    g.font = `400 ${X(19)}px ${FONT}`;
    const tw = Math.max(dialog === 'progress' ? X(420) : 0, ...lines.map((l) => g.measureText(l).width));
    const w = tw + X(40), h = lines.length * lh + X(36);
    const sc = easeOut(clamp(open));
    g.save(); g.translate(SW / 2, SH / 2); g.scale(sc, sc);
    g.fillStyle = VS.dialogBg; g.fillRect(-w / 2, -h / 2, w, h);
    if (sc > 0.98) {
      g.fillStyle = VS.dialog; g.textAlign = 'center'; g.textBaseline = 'top';
      lines.forEach((l, i) => g.fillText(l, 0, -h / 2 + X(18) + i * lh));
      if (dialog === 'progress') {
        const by = -h / 2 + X(18) + lh + X(10);
        g.fillStyle = VS.barBg; g.fillRect(-X(210), by, X(420), X(8));
        g.fillStyle = VS.bar; g.fillRect(-X(210), by, X(420) * clamp(progress), X(8));
        g.fillStyle = VS.dialog; g.fillText(`${Math.round(clamp(progress) * 100)}%`, 0, by + X(18));
      }
    }
    g.restore();
  }
  g.restore();
}
