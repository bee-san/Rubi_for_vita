// Floating panels (drawn into canvas textures of 3D planes) and the mock-ups of features that are still
// landing. Every mock-up here is marked in README.md and in the preview comment; the strings are the
// feature branches' own (feat/one-vpk-install's settings/install_page.c and docs/one-vpk-install.md,
// VitaShell 2.02's english_us.txt) as their authors gave them on 2026-10-16.
import { rr, APP, SW, SH } from './screen.js';
import { clamp, easeOut } from './util.js';

const FONT = '"Noto Sans JP", sans-serif';

// ---- an Anki card face, cropped from Anki's own render of the Lapis note
export function drawCard(g, img, crop, { badge = null } = {}) {
  const W = g.canvas.width, H = g.canvas.height;
  g.clearRect(0, 0, W, H);
  g.save(); rr(g, 0, 0, W, H, 28); g.clip();
  g.fillStyle = '#fffaf0'; g.fillRect(0, 0, W, H);
  g.drawImage(img, ...crop, 0, 0, W, H);
  g.restore();
  g.strokeStyle = 'rgba(0,0,0,0.10)'; g.lineWidth = 3; rr(g, 1.5, 1.5, W - 3, H - 3, 27); g.stroke();
  if (badge) { // the video's own label: which kind of shot this is
    g.font = `700 30px ${FONT}`; const tw = g.measureText(badge).width;
    g.fillStyle = 'rgba(20,28,40,0.78)'; rr(g, 24, H - 66, tw + 36, 46, 23); g.fill();
    g.fillStyle = '#fff'; g.textBaseline = 'middle'; g.fillText(badge, 42, H - 42);
  }
}

// ---- the release page on a computer: a generic browser (no browser or site logos)
export const RELEASE = {
  version: '0.8.0',
  assets: [['VitaJPOverlay-0.8.0.vpk', '53 MB'], ['VitaJPOverlay-0.8.0-debug.vpk', '53 MB'], ['VitaJPOverlay-0.8.0.zip', '53 MB'], ['VitaJPOverlay-0.8.0-debug.zip', '53 MB'], ['SHA256SUMS', '1 KB']],
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
  g.fillStyle = '#57606a'; g.font = `500 28px ${FONT}`; g.fillText('One VPK: install it in VitaShell, then open Overlay and choose Install.', 90, 350);
  g.fillStyle = '#1f2328'; g.font = `700 32px ${FONT}`; g.fillText('Assets  5', 90, 420);
  RELEASE.assets.forEach(([n, sz], i) => {
    const y = 470 + i * 72;
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
    g.fillText(dl >= 1 ? '53 MB  ✓' : `${Math.round(53 * clamp(dl))} of 53 MB`, W - 90, H - 104 + (1 - k) * 60); g.textAlign = 'left';
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

// ---- VitaShell 2.02 (Default theme), drawn after its sources: a stand-in until promo-assets' captures land
const VS = { bg: '#000000', text: '#ffffff', path: '#ffffff', focus: 'rgba(255,255,255,0.20)', archive: '#ff7f00', folder: '#00ffff', dialog: '#2f2f2f', bar: '#7fff00', barBg: '#4c4c4c' };
export function drawVitaShell(screen, { focus = 1, dialog = null, progress = 0, open = 1 }) {
  const g = screen.g, X = (v) => v * 2; // console px -> canvas px
  g.save(); g.fillStyle = VS.bg; g.fillRect(0, 0, SW, SH);
  g.font = `400 ${X(18)}px ${FONT}`; g.textBaseline = 'top'; g.fillStyle = VS.text;
  g.fillText('VitaShell 2.02', X(20), X(14));
  g.textAlign = 'right'; g.fillText('2026/10/16  21:04', X(890), X(14)); g.textAlign = 'left';
  g.fillStyle = '#8ef0b0'; g.fillRect(X(906), X(18), X(30), X(13));
  g.fillStyle = VS.path; g.fillText('ux0:data/', X(20), X(45));
  const rows = [['..', '', 'folder'], ['VitaJPOverlay-0.8.0.vpk', '50.55 MB', 'archive'], ['VitaJPOverlay', 'Folder', 'folder']];
  rows.forEach(([n, sz, kind], i) => {
    const y = X(84 + i * 23);
    if (i === focus) { g.fillStyle = VS.focus; g.fillRect(X(16), y - X(1), X(928), X(23)); }
    g.fillStyle = kind === 'archive' ? VS.archive : VS.folder; g.fillText(n, X(36), y + X(1));
    g.fillStyle = VS.text; g.textAlign = 'right'; g.fillText(sz, X(940), y + X(1)); g.textAlign = 'left';
  });
  if (dialog) {
    const lines = {
      ask: ['Do you want to install this package?', '', '╳ Yes    ○ No'],
      perm: ['This package requests extended permissions.', 'It will have access to your personal information.', 'If you did not obtain it from a trusted source,', 'please proceed at your own caution.', '', 'Would you like to continue the install?', '', '╳ Yes    ○ No'],
      progress: ['Installing...', '', '', '', ''],
    }[dialog];
    const lh = X(23), w = X(dialog === 'perm' ? 560 : 460), h = lines.length * lh + X(36);
    const s = easeOut(clamp(open));
    g.save(); g.translate(SW / 2, SH / 2); g.scale(s, s);
    g.fillStyle = VS.dialog; g.fillRect(-w / 2, -h / 2, w, h);
    if (s > 0.98) {
      g.fillStyle = VS.text; g.textAlign = 'center';
      lines.forEach((l, i) => g.fillText(l, 0, -h / 2 + X(18) + i * lh));
      if (dialog === 'progress') {
        g.fillStyle = VS.barBg; g.fillRect(-X(210), -h / 2 + X(18) + lh * 1.6, X(420), X(8));
        g.fillStyle = VS.bar; g.fillRect(-X(210), -h / 2 + X(18) + lh * 1.6, X(420) * clamp(progress), X(8));
        g.fillStyle = VS.text; g.fillText(`${Math.round(clamp(progress) * 100)}%`, 0, -h / 2 + X(18) + lh * 2.4);
      }
    }
    g.restore();
  }
  g.restore();
}

// ---- the Overlay app's Install page (feat/one-vpk-install, not merged): a stand-in drawn in the app's
// own colours and layout until promo-assets' capture lands
export function drawInstallPage(screen, { mode = 'plan', progress = 0, pressK = 0 }) {
  const g = screen.g, X = (v) => v * 2;
  g.save(); g.fillStyle = APP.bg; g.fillRect(0, 0, SW, SH);
  g.fillStyle = APP.accent; g.fillRect(0, 0, SW, X(4));
  g.textBaseline = 'middle';
  g.fillStyle = APP.white; g.font = `400 ${X(28)}px ${FONT}`; g.fillText('Overlay', X(26), X(26));
  g.fillStyle = APP.dim; g.font = `400 ${X(20)}px ${FONT}`; g.textAlign = 'right'; g.fillText('Select: page', X(878), X(27)); g.textAlign = 'left';
  const tabs = ['Games', 'OCR & lookup', 'Anki', 'Install'];
  tabs.forEach((tb, i) => {
    const x = X(24 + i * 228), w = X(220);
    g.fillStyle = i === 3 ? APP.accent : APP.card; g.fillRect(x, X(52), w, X(48));
    g.fillStyle = i === 3 ? 'rgb(9,15,23)' : APP.white; g.font = `400 ${X(24)}px ${FONT}`; g.textAlign = 'center'; g.fillText(tb, x + w / 2, X(77)); g.textAlign = 'left';
  });
  g.fillStyle = APP.card; g.fillRect(X(24), X(112), X(912), X(330));
  const title = mode === 'plan' ? 'Install the overlay (v0.8.0)' : mode === 'installing' ? 'Installing...' : 'The overlay is installed (v0.8.0)';
  g.fillStyle = APP.white; g.font = `500 ${X(26)}px ${FONT}`; g.fillText(title, X(44), X(140));
  g.font = `400 ${X(17)}px ${FONT}`; g.fillStyle = APP.dim;
  const plan = [
    '1. Copy the 3 plugins and their RCO to ur0:tai and ur0:data/VitaJPOverlay, and the OCR',
    '   models (10.6 MB) to ux0:data/VitaJPOverlay/ocr; move the dictionary (143 MB)',
    '   to ux0:data/VitaJPOverlay/dictionaries from this app: moved, not copied.',
    '2. Back up ux0:tai/config.txt and ur0:tai/config.txt as config.txt.vjo-20261016-071503.bak',
    '   in each folder (kept).',
    '3. ux0:tai/config.txt (taiHEN reads this one at boot): add the kernel line under *KERNEL',
    '   and the Shell line under *main.',
    '5. Read everything back to check it.',
    'Then restart the Vita yourself: this app never restarts it.',
  ];
  if (mode === 'plan') plan.forEach((l, i) => g.fillText(l, X(44), X(178 + i * 27)));
  if (mode === 'installing') {
    const p = clamp(progress), mb = (10.6 * p).toFixed(1);
    g.fillText(p < 0.5 ? 'Moving plaintext.vjdict' : 'Copying VOCR3_H15_w8.vocr', X(44), X(196));
    g.fillStyle = 'rgba(255,255,255,0.14)'; g.fillRect(X(44), X(232), X(870), X(18));
    g.fillStyle = APP.accent; g.fillRect(X(44), X(232), X(870) * p, X(18));
    g.fillStyle = APP.white; g.fillText(`${mb} of 10.6 MB`, X(44), X(276));
    g.fillStyle = APP.dim; g.fillText('Leave the Vita on and this app open: the PS button is held until it ends.', X(44), X(316));
  }
  if (mode === 'done') {
    ['Installed the overlay v0.8.0. Restart the Vita now: the new plugins load at its next start.',
      'The dictionary (143 MB) was moved out of this app, not copied.',
      'Backup: ux0:tai/config.txt.vjo-20261016-071503.bak (kept).'].forEach((l, i) => { g.fillStyle = i === 0 ? APP.white : APP.dim; g.fillText(l, X(44), X(190 + i * 34)); });
  }
  // button row
  const bx = X(24), by = X(452);
  if (mode === 'plan') {
    const s = 1 - 0.06 * Math.sin(clamp(pressK) * Math.PI);
    g.save(); g.translate(bx + X(120), by + X(26)); g.scale(s, s);
    g.fillStyle = APP.accent; g.fillRect(-X(120), -X(26), X(240), X(52));
    g.fillStyle = 'rgb(9,15,23)'; g.font = `500 ${X(24)}px ${FONT}`; g.textAlign = 'center'; g.fillText('Install', 0, X(1)); g.restore();
  }
  g.fillStyle = APP.dim; g.font = `400 ${X(19)}px ${FONT}`; g.textAlign = 'left';
  g.fillText(mode === 'plan' ? '×: Install   Left/Right: action   Up/Down: scroll   ○: exit' : '○: exit', X(26), X(524));
  g.restore();
}

// ---- the Overlay app's "Read text" card (PR #146, not merged): a stand-in in the app's colours until
// promo-assets' Vita3K capture of the PR branch lands; the strings are #146's.
export function drawReadText(g, { choice = 0, img = null, crop = null } = {}) {
  const W = g.canvas.width, H = g.canvas.height;
  g.clearRect(0, 0, W, H);
  g.save(); rr(g, 0, 0, W, H, 26); g.clip();
  if (img) { g.drawImage(img, ...crop, 0, 0, W, H); g.restore(); return; }
  g.fillStyle = APP.bg; g.fillRect(0, 0, W, H);
  g.fillStyle = APP.sel; g.fillRect(20, 20, W - 40, H - 40);
  g.strokeStyle = APP.accent; g.lineWidth = 4; g.strokeRect(22, 22, W - 44, H - 44);
  g.textBaseline = 'middle';
  g.fillStyle = APP.white; g.font = `500 54px ${FONT}`; g.fillText('Read text', 60, 96);
  const opts = ['Automatically (when it changes)', 'Only when I press L + R'];
  opts.forEach((o, i) => {
    const x = 60 + i * ((W - 140) / 2 + 20), w = (W - 140) / 2, y = 150;
    g.fillStyle = i === choice ? APP.accent : APP.card; g.fillRect(x, y, w, 96);
    g.fillStyle = i === choice ? 'rgb(9,15,23)' : APP.white; g.font = `500 36px ${FONT}`; g.textAlign = 'center';
    g.fillText(o, x + w / 2, y + 49); g.textAlign = 'left';
  });
  g.fillStyle = APP.dim; g.font = `400 38px ${FONT}`;
  g.fillText(choice === 0 ? 'The popup is ready when you press' : 'Nothing runs until you press', 60, 318);
  g.restore();
}
