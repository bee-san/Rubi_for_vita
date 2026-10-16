// The promo, rendered one frame at a time: window.__setup(cut) once, then window.__frame(t) for each
// frame time. Nothing reads the clock: every value comes from t, so a frame always renders the same.
import * as THREE from 'three';
import { RoomEnvironment } from 'three/addons/environments/RoomEnvironment.js';
import { buildDevice } from './device.js';
import { Screen, SW, SH } from './screen.js';
import { drawCaption, drawSteps, drawEnd, drawCallout } from './captions.js';
import { buildTimeline } from './timeline.js';
import { rng, lerp, clamp } from './util.js';

const CUTS = { '16x9': [1920, 1080], '9x16': [1080, 1920] };
const IMAGES = [
  'vn/s1/s1-m1-notext@2x.png', 'vn/s1/s1-m1-text@2x.png', 'vn/s1/s1-m2-notext@2x.png', 'vn/s1/s1-m2-text@2x.png', 'vn/s1/s1-m2-frame@2x.png',
  'vn/s2/s2-m1-notext@2x.png', 'vn/s2/s2-m1-text@2x.png',
  'vn/s3/s3-m1-notext@2x.png', 'vn/s3/s3-m1-text@2x.png', 'vn/s3/s3-m2-notext@2x.png', 'vn/s3/s3-m2-text@2x.png',
  'overlay/s1/s1-m2-ov01-open@2x.png', 'overlay/s1/s1-m2-ov02-right-inflight@2x.png', 'overlay/s1/s1-m2-ov03-issho@2x.png',
  'overlay/s1/s1-m2-ov04-issho-saving@2x.png', 'overlay/s1/s1-m2-ov05-issho-saved@2x.png', 'overlay/s1/s1-m2-ov06-miniiku@2x.png',
  'overlay/s1/s1-m2-ov07-miniiku-saved@2x.png', 'overlay/s3/s3-m1-ov03-open@2x.png', 'overlay/s3/s3-m1-ov04-kodomonokoro@2x.png',
  'overlay/s3/s3-m1-ov05-mitai@2x.png',
  'anki/with-screenshot-MOCKUP-feat-anki-screenshot/png/card-1-front-desktop-light-MOCKUP.png',
  'anki/with-screenshot-MOCKUP-feat-anki-screenshot/png/card-1-back-desktop-light-MOCKUP.png',
  'anki/with-screenshot-MOCKUP-feat-anki-screenshot/png/card-2-front-desktop-light-MOCKUP.png',
  'app/main-1ed95ca/png/01-games-top.png', 'app/main-1ed95ca/png/02-games-scroll-1.png', 'app/main-1ed95ca/png/03-games-scroll-2.png',
  'app/main-1ed95ca/png/04-games-neon-drift-selected.png', 'app/main-1ed95ca/png/05-games-neon-drift-OFF-saved.png',
  'app/icons/PROMO0001-yuunagi-step.png', 'app/icons/PROMO0002-neon-drift.png', 'app/icons/PROMO0003-courier-run.png', 'app/icons/PROMO0004-ember-garden.png',
  'app/install-MOCKUP-one-vpk/png/MOCKUP-01-install-plan.png', 'app/install-MOCKUP-one-vpk/png/MOCKUP-02-installing-plugins-and-models.png',
  'app/install-MOCKUP-one-vpk/png/MOCKUP-03-installing-checking-the-dictionary-56MB.png', 'app/install-MOCKUP-one-vpk/png/MOCKUP-04-installing-checking-the-dictionary-121MB.png',
  'app/install-MOCKUP-one-vpk/png/MOCKUP-05-installing-config-txt-lines.png', 'app/install-MOCKUP-one-vpk/png/MOCKUP-06-installed-restart-the-vita-now.png',
  'app/pr146-ocr-mode-21488a7-NOT-MERGED/png/PR146-10-read-text-at-top-automatically.png', 'app/pr146-ocr-mode-21488a7-NOT-MERGED/png/PR146-11-read-text-at-top-only-when-I-press-LR.png',
  'app/icons/PROMO0005-hoshizora-diary.png', 'app/icons/PROMO0006-paper-lantern.png', 'app/icons/PROMO0008-sakura-no-kioku.png', 'app/icons/VJOSET001-overlay.png',
];
const JSONS = ['vn/s1/s1-m1-text.json', 'vn/s1/s1-m2-text.json', 'vn/s2/s2-m1-text.json', 'vn/s3/s3-m1-text.json', 'vn/s3/s3-m2-text.json'];
const LOCAL = { 'repo/icon0.png': 0, 'repo/bg.png': 0, 'repo/startup.png': 0, 'vitashell/folder_icon.png': 0, 'vitashell/archive_icon.png': 0, 'vitashell/battery.png': 0, 'vitashell/ftp.png': 0 };
const OPTIONAL = [];

async function loadImage(src) { const im = new Image(); im.src = src; await im.decode(); return im; }

let S = null;

window.__setup = async (cut, opts = {}) => {
  const [W, H] = CUTS[cut];
  const sc = opts.scale || 1; // drafts: 0.5 renders a quarter of the pixels, laid out as the full frame
  const font = new FontFace('Noto Sans JP', 'url("/assets/shared/fonts/NotoSansJP[wght].ttf")', { weight: '100 900' });
  await font.load(); document.fonts.add(font);
  const img = {}, json = {};
  await Promise.all(IMAGES.map(async (p) => { img[p] = await loadImage('/assets/shared/' + p); }));
  await Promise.all(Object.keys(LOCAL).map(async (p) => { img[p] = await loadImage('/assets/' + p); }));
  await Promise.all(JSONS.map(async (p) => { json[p] = await (await fetch('/assets/shared/' + p)).json(); }));
  for (const extra of OPTIONAL) { try { img[extra] = await loadImage('/assets/shared/' + extra); } catch (e) { /* not landed yet */ } }

  const glc = document.createElement('canvas'); glc.width = Math.round(W * sc); glc.height = Math.round(H * sc);
  const renderer = new THREE.WebGLRenderer({ canvas: glc, antialias: true, preserveDrawingBuffer: true, powerPreference: 'high-performance' });
  renderer.setPixelRatio(1); renderer.setSize(glc.width, glc.height, false);
  renderer.outputColorSpace = THREE.SRGBColorSpace;
  renderer.toneMapping = THREE.ACESFilmicToneMapping; renderer.toneMappingExposure = 0.92;

  const scene = new THREE.Scene();
  const pmrem = new THREE.PMREMGenerator(renderer);
  scene.environment = pmrem.fromScene(new RoomEnvironment(), 0.035).texture;
  scene.environmentIntensity = 0.9;

  // backdrop: a vertical gradient plus a soft light spill behind the device
  const bgc = document.createElement('canvas'); bgc.width = 4; bgc.height = 512;
  const bgTex = new THREE.CanvasTexture(bgc); bgTex.colorSpace = THREE.SRGBColorSpace; scene.background = bgTex;
  const spillC = document.createElement('canvas'); spillC.width = spillC.height = 256;
  { const g = spillC.getContext('2d'); const r = g.createRadialGradient(128, 128, 0, 128, 128, 128); r.addColorStop(0, 'rgba(255,255,255,1)'); r.addColorStop(0.4, 'rgba(255,255,255,0.35)'); r.addColorStop(1, 'rgba(255,255,255,0)'); g.fillStyle = r; g.fillRect(0, 0, 256, 256); }
  const spillTex = new THREE.CanvasTexture(spillC); spillTex.colorSpace = THREE.SRGBColorSpace;
  const spill = new THREE.Mesh(new THREE.PlaneGeometry(2600, 2600), new THREE.MeshBasicMaterial({ map: spillTex, transparent: true, depthWrite: false, opacity: 0.5, toneMapped: false }));
  spill.position.set(0, 60, -1800); scene.add(spill);

  // lights
  const key = new THREE.DirectionalLight(0xffffff, 1.9); key.position.set(-300, 500, 600); scene.add(key);
  const rim = new THREE.DirectionalLight(0xffffff, 2.4); rim.position.set(500, 250, -500); scene.add(rim);
  const fill = new THREE.HemisphereLight(0xdfe8ff, 0x20222a, 0.35); scene.add(fill);

  // bokeh: soft discs far behind the device (the video's own atmosphere)
  const bokC = document.createElement('canvas'); bokC.width = bokC.height = 128;
  { const g = bokC.getContext('2d'); const r = g.createRadialGradient(64, 64, 0, 64, 64, 64); r.addColorStop(0, 'rgba(255,255,255,0.9)'); r.addColorStop(0.6, 'rgba(255,255,255,0.55)'); r.addColorStop(0.8, 'rgba(255,255,255,0.18)'); r.addColorStop(1, 'rgba(255,255,255,0)'); g.fillStyle = r; g.fillRect(0, 0, 128, 128); }
  const bokTex = new THREE.CanvasTexture(bokC); bokTex.colorSpace = THREE.SRGBColorSpace;
  const NB = 90, R = rng(7);
  const bokBase = []; const bokPos = new Float32Array(NB * 3), bokCol = new Float32Array(NB * 3);
  for (let i = 0; i < NB; i++) bokBase.push({ x: (R() - 0.5) * 3600, y: (R() - 0.5) * 2200, z: -700 - R() * 2400, s: R(), p: R() * 10, c: R() });
  const bokGeo = new THREE.BufferGeometry();
  bokGeo.setAttribute('position', new THREE.BufferAttribute(bokPos, 3)); bokGeo.setAttribute('color', new THREE.BufferAttribute(bokCol, 3));
  const bokMat = new THREE.PointsMaterial({ size: 70, map: bokTex, transparent: true, depthWrite: false, blending: THREE.AdditiveBlending, vertexColors: true, sizeAttenuation: true, opacity: 0.5, toneMapped: false });
  const bokeh = new THREE.Points(bokGeo, bokMat); scene.add(bokeh);

  // petals for the riverside scene, drifting round the device
  const petC = document.createElement('canvas'); petC.width = 64; petC.height = 64;
  { const g = petC.getContext('2d'); const gr = g.createLinearGradient(0, 0, 64, 64); gr.addColorStop(0, '#ffd3e2'); gr.addColorStop(1, '#f58fb2'); g.fillStyle = gr; g.beginPath(); g.moveTo(32, 4); g.bezierCurveTo(60, 14, 58, 50, 32, 60); g.bezierCurveTo(6, 50, 4, 14, 32, 4); g.fill(); g.fillStyle = 'rgba(0,0,0,0)'; }
  const petTex = new THREE.CanvasTexture(petC); petTex.colorSpace = THREE.SRGBColorSpace;
  const NP = 46; const petals = new THREE.InstancedMesh(new THREE.PlaneGeometry(9, 9), new THREE.MeshStandardMaterial({ map: petTex, transparent: true, side: THREE.DoubleSide, depthWrite: false, roughness: 0.6 }), NP);
  const petBase = []; const R2 = rng(11);
  for (let i = 0; i < NP; i++) { const side = R2() < 0.5 ? -1 : 1; petBase.push({ x: side * (130 + R2() * 330), y: R2() * 600, z: -420 + R2() * 380, ph: R2() * 6.28, sp: 0.6 + R2() * 0.7, r: R2() }); }
  scene.add(petals);

  // shooting star (end beat)
  const starC = document.createElement('canvas'); starC.width = 512; starC.height = 32;
  { const g = starC.getContext('2d'); const gr = g.createLinearGradient(0, 0, 512, 0); gr.addColorStop(0, 'rgba(255,255,255,0)'); gr.addColorStop(0.85, 'rgba(200,225,255,0.8)'); gr.addColorStop(1, 'rgba(255,255,255,1)'); g.fillStyle = gr; g.beginPath(); g.moveTo(0, 16); g.lineTo(512, 9); g.lineTo(512, 23); g.closePath(); g.fill(); }
  const starTex = new THREE.CanvasTexture(starC); starTex.colorSpace = THREE.SRGBColorSpace;
  const star = new THREE.Mesh(new THREE.PlaneGeometry(900, 26), new THREE.MeshBasicMaterial({ map: starTex, transparent: true, depthWrite: false, blending: THREE.AdditiveBlending, opacity: 0, toneMapped: false }));
  scene.add(star);

  // the device and its screen
  const screen = new Screen(img, json);
  const scrTex = new THREE.CanvasTexture(screen.canvas); scrTex.colorSpace = THREE.SRGBColorSpace; scrTex.anisotropy = renderer.capabilities.getMaxAnisotropy();
  const device = buildDevice({ screenTexture: scrTex });
  scene.add(device.root);

  const camera = new THREE.PerspectiveCamera(30, W / H, 5, 30000);

  // floating panels (cards, the browser, the app's setting): planes with canvas textures
  const panels = {};
  function makePanel(name, wmm, hmm, cw, ch, { back = false } = {}) {
    const mk = () => { const c = document.createElement('canvas'); c.width = cw; c.height = ch; const t = new THREE.CanvasTexture(c); t.colorSpace = THREE.SRGBColorSpace; t.anisotropy = 8; return { c, t, g: c.getContext('2d') }; };
    const f = mk(); const grp = new THREE.Group();
    const mf = new THREE.Mesh(new THREE.PlaneGeometry(wmm, hmm), new THREE.MeshBasicMaterial({ map: f.t, transparent: true, toneMapped: false, side: back ? THREE.FrontSide : THREE.DoubleSide }));
    grp.add(mf);
    let b = null;
    if (back) { b = mk(); const mb = new THREE.Mesh(new THREE.PlaneGeometry(wmm, hmm), new THREE.MeshBasicMaterial({ map: b.t, transparent: true, toneMapped: false })); mb.rotation.y = Math.PI; grp.add(mb); }
    // a soft dark halo behind, so the panel separates from the backdrop
    const sh = new THREE.Mesh(new THREE.PlaneGeometry(wmm * 1.25, hmm * 1.3), new THREE.MeshBasicMaterial({ map: spillTex, color: 0x000000, transparent: true, opacity: 0.0, depthWrite: false }));
    sh.position.z = -2; grp.add(sh);
    grp.visible = false; scene.add(grp);
    panels[name] = { grp, front: f, back: b, halo: sh, mats: [mf.material, ...(b ? [grp.children[1].material] : [])], wmm, hmm };
    return panels[name];
  }

  const out = document.createElement('canvas'); out.width = glc.width; out.height = glc.height;
  const og = out.getContext('2d');
  og.setTransform(sc, 0, 0, sc, 0, 0); og.__W = W; og.__H = H; // the 2D layers are laid out at full size
  document.body.appendChild(out);

  S = { cut, W, H, renderer, scene, camera, device, screen, scrTex, panels, makePanel, img, json, out, og,
    bg: { c: bgc, tex: bgTex }, spill, key, rim, fill, bokeh: { base: bokBase, pos: bokPos, col: bokCol, geo: bokGeo, mat: bokMat },
    petals: { mesh: petals, base: petBase }, star, THREE };
  S.tl = buildTimeline(S);
  window.__duration = S.tl.duration;
  return { duration: S.tl.duration, gl: renderer.getContext().getParameter(renderer.getContext().VERSION) };
};

// A press whose button is outside the frame (a close-up of the screen) shows a badge at the nearest
// edge: the video's own sign of the press.
const _bv = new THREE.Vector3();
function glyph(g, b, r) {
  g.strokeStyle = '#1b2333'; g.fillStyle = '#1b2333'; g.lineWidth = r * 0.16; g.lineCap = 'round'; g.lineJoin = 'round';
  g.beginPath();
  if (b === 'cross') { const k = r * 0.38; g.moveTo(-k, -k); g.lineTo(k, k); g.moveTo(k, -k); g.lineTo(-k, k); g.stroke(); }
  else if (b === 'circle') { g.arc(0, 0, r * 0.42, 0, Math.PI * 2); g.stroke(); }
  else if (b === 'triangle') { g.moveTo(0, -r * 0.45); g.lineTo(r * 0.45, r * 0.33); g.lineTo(-r * 0.45, r * 0.33); g.closePath(); g.stroke(); }
  else if (b === 'square') { g.rect(-r * 0.36, -r * 0.36, r * 0.72, r * 0.72); g.stroke(); }
  else if (b.startsWith('dpad')) {
    const a = r * 0.2, L = r * 0.62;
    g.fillStyle = 'rgba(27,35,51,0.22)'; g.fillRect(-a, -L, 2 * a, 2 * L); g.fillRect(-L, -a, 2 * L, 2 * a);
    const dir = { 'dpad-up': [0, -1], 'dpad-down': [0, 1], 'dpad-left': [-1, 0], 'dpad-right': [1, 0] }[b];
    g.save(); g.rotate(Math.atan2(dir[1], dir[0])); g.fillStyle = '#1b2333';
    g.beginPath(); g.moveTo(L, 0); g.lineTo(a * 0.6, -a * 1.35); g.lineTo(a * 0.6, a * 1.35); g.closePath(); g.fill(); g.restore();
  } else if (b === 'power') { g.arc(0, r * 0.04, r * 0.36, -Math.PI * 0.32, Math.PI * 1.32); g.stroke(); g.beginPath(); g.moveTo(0, -r * 0.46); g.lineTo(0, -r * 0.06); g.stroke(); }
  else { const txt = { ps: 'PS', l: 'L', r: 'R', start: 'START', select: 'SELECT' }[b] || b; g.font = `800 ${Math.round(r * (txt.length > 2 ? 0.34 : 0.62))}px "Noto Sans JP", sans-serif`; g.textAlign = 'center'; g.textBaseline = 'middle'; g.fillText(txt, 0, r * 0.04); }
}
function pressBadges(g, presses) {
  const W = S.W, H = S.H, P = S.cut === '9x16';
  const R = P ? 52 : 56, m = R + 30, y0 = P ? 520 : 120, y1 = P ? 1380 : 800;
  for (const pr of presses) for (const b of pr.btns) {
    const part = S.device.parts[b]; if (!part) continue;
    if (b === 'l' || b === 'r') { _bv.set(b === 'l' ? -66 : 66, 44, -2); S.device.root.localToWorld(_bv); } else part.obj.getWorldPosition(_bv);
    _bv.project(S.camera);
    const x = (_bv.x + 1) / 2 * W, y = (1 - _bv.y) / 2 * H;
    if (_bv.z < 1 && x > 30 && x < W - 30 && y > 30 && y < H - 30) continue; // the button is in view
    // the badge sits at the frame's edge, level with the console's corner below the screen (not on it)
    const left = b.startsWith('dpad') || b === 'ps' || b === 'l' || b === 'power';
    if (b === 'l' || b === 'r') _bv.set(left ? -66 : 66, 44, -2); else _bv.set(left ? -60 : 60, -39, 9.3);
    S.device.root.localToWorld(_bv); _bv.project(S.camera);
    const ax = (_bv.x + 1) / 2 * W, ay = (1 - _bv.y) / 2 * H;
    const cx = clamp(ax, m, W - m), cy = clamp(ay, y0, y1), age = pr.age;
    const sc = age < 0.08 ? 0.6 + 0.4 * (age / 0.08) : 1, a = 1 - clamp((age - 0.32) / 0.18);
    if (a <= 0) continue;
    g.save(); g.translate(cx, cy); g.globalAlpha = a;
    g.strokeStyle = 'rgba(99,230,218,' + (1 - clamp(age / 0.5)) + ')'; g.lineWidth = 7;
    g.beginPath(); g.arc(0, 0, R * (1.05 + age * 1.6), 0, Math.PI * 2); g.stroke();
    g.scale(sc, sc); g.shadowColor = 'rgba(0,0,0,0.45)'; g.shadowBlur = 18; g.shadowOffsetY = 4;
    g.fillStyle = 'rgba(248,250,252,0.95)'; g.beginPath(); g.arc(0, 0, R, 0, Math.PI * 2); g.fill(); g.shadowBlur = 0; g.shadowOffsetY = 0;
    glyph(g, b, R);
    g.restore();
  }
}

const tmpObj = new THREE.Object3D();
function updateAtmosphere(t, pal) {
  // background gradient
  const g = S.bg.c.getContext('2d'); const gr = g.createLinearGradient(0, 0, 0, 512);
  gr.addColorStop(0, pal.top); gr.addColorStop(1, pal.bottom); g.fillStyle = gr; g.fillRect(0, 0, 4, 512); S.bg.tex.needsUpdate = true;
  S.spill.material.color.set(pal.spill); S.spill.material.opacity = pal.spillA;
  S.key.color.set(pal.key); S.rim.color.set(pal.rim);
  // bokeh drift
  const c = new THREE.Color(pal.bokeh), c2 = new THREE.Color(pal.bokeh2);
  S.bokeh.base.forEach((b, i) => {
    S.bokeh.pos[i * 3] = b.x + Math.sin(t * 0.15 + b.p) * 60;
    S.bokeh.pos[i * 3 + 1] = b.y + ((t * (14 + b.s * 20) + b.p * 200) % 2200) - 1100;
    S.bokeh.pos[i * 3 + 2] = b.z;
    const cc = b.c < 0.5 ? c : c2; const tw = 0.6 + 0.4 * Math.sin(t * 1.3 + b.p * 3);
    S.bokeh.col[i * 3] = cc.r * tw; S.bokeh.col[i * 3 + 1] = cc.g * tw; S.bokeh.col[i * 3 + 2] = cc.b * tw;
  });
  S.bokeh.geo.attributes.position.needsUpdate = true; S.bokeh.geo.attributes.color.needsUpdate = true;
  S.bokeh.mat.opacity = pal.bokehA; S.bokeh.mat.size = pal.bokehSize;
  // petals
  const pm = S.petals.mesh; pm.visible = pal.petals > 0.01; pm.material.opacity = pal.petals;
  S.petals.base.forEach((p, i) => {
    const fall = (t * 38 * p.sp + p.y) % 620;
    tmpObj.position.set(p.x + Math.sin(t * 0.9 * p.sp + p.ph) * 40 + t * 12, 330 - fall, p.z + Math.cos(t * 0.7 + p.ph) * 30);
    tmpObj.rotation.set(t * 1.7 * p.sp + p.ph, t * 1.1 + p.ph * 2, t * 0.8 * p.sp);
    tmpObj.scale.setScalar(0.7 + p.r * 0.8); tmpObj.updateMatrix(); pm.setMatrixAt(i, tmpObj.matrix);
  });
  pm.instanceMatrix.needsUpdate = true;
}

window.__frame = async (t, { format = 'png' } = {}) => {
  const st = S.tl.apply(t);           // sets the camera, the device, panels; draws the screen
  updateAtmosphere(t, st.palette);
  S.scrTex.needsUpdate = true;
  for (const p of Object.values(S.panels)) if (p.dirty) { p.front.t.needsUpdate = true; if (p.back) p.back.t.needsUpdate = true; p.dirty = false; }
  S.renderer.render(S.scene, S.camera);
  const g = S.og; g.globalAlpha = 1; g.drawImage(S.renderer.domElement, 0, 0, S.W, S.H);
  if (st.presses) pressBadges(g, st.presses);
  for (const cap of st.captions) drawCaption(g, cap, t, S.cut);
  if (st.callout) drawCallout(g, st.callout, t);
  if (st.steps) drawSteps(g, st.steps, t, S.cut, st.steps.window);
  if (st.end != null) drawEnd(g, t, S.cut, st.end);
  if (st.vignette) { const v = g.createRadialGradient(S.W / 2, S.H / 2, Math.min(S.W, S.H) * 0.45, S.W / 2, S.H / 2, Math.max(S.W, S.H) * 0.75); v.addColorStop(0, 'rgba(0,0,0,0)'); v.addColorStop(1, `rgba(0,0,0,${st.vignette})`); g.fillStyle = v; g.fillRect(0, 0, S.W, S.H); }
  if (st.fade) { g.fillStyle = `rgba(0,0,0,${st.fade})`; g.fillRect(0, 0, S.W, S.H); }
  if (format === 'none') return '';
  return S.out.toDataURL('image/png').split(',')[1];
};
window.__sfx = () => S.tl.sfx;
window.__captions = () => S.tl.captionList;
window.__ready = true;
