// A white PS Vita PCH-1000-style handheld, built from scratch in three.js.
// Original model from the published dimensions (182.0 x 83.5 x 18.6 mm, 5" 960x544 screen) and the
// layout of PR #134's original front-view drawing; no photos, no logos, no trademarks.
// Units: millimetres. Origin: the body's centre. +X right, +Y up, +Z out of the screen.
import * as THREE from 'three';

const FRONT = 9.3;          // front face z
const GLASS_Z = FRONT + 0.4; // top of the glass panel
export const SCREEN = { w: 110.6, h: 62.7, z: GLASS_Z + 0.02 };

function bodyShape(grow = 0) {
  // the outline (PR #134's path, 0.1 mm units converted): flat top and bottom, bulged ends
  const g = grow, k = (v) => v;
  const s = new THREE.Shape();
  const T = 41.75 + g, R = 91 + g, X = 58, cx = 82.42 + g * 0.6, cy = 30.895;
  s.moveTo(-X, T);
  s.lineTo(X, T);
  s.bezierCurveTo(cx, T, R, cy, R, 0);
  s.bezierCurveTo(R, -cy, cx, -T, X, -T);
  s.lineTo(-X, -T);
  s.bezierCurveTo(-cx, -T, -R, -cy, -R, 0);
  s.bezierCurveTo(-R, cy, -cx, T, k(-X), T);
  return s;
}

function roundRect(w, h, r, cx = 0, cy = 0) {
  const s = new THREE.Shape();
  const x0 = cx - w / 2, y0 = cy - h / 2, x1 = cx + w / 2, y1 = cy + h / 2;
  s.moveTo(x0 + r, y0);
  s.lineTo(x1 - r, y0); s.quadraticCurveTo(x1, y0, x1, y0 + r);
  s.lineTo(x1, y1 - r); s.quadraticCurveTo(x1, y1, x1 - r, y1);
  s.lineTo(x0 + r, y1); s.quadraticCurveTo(x0, y1, x0, y1 - r);
  s.lineTo(x0, y0 + r); s.quadraticCurveTo(x0, y0, x0 + r, y0);
  return s;
}

function canvasTex(w, h, draw, srgb = true) {
  const c = document.createElement('canvas'); c.width = w; c.height = h;
  draw(c.getContext('2d'), w, h);
  const t = new THREE.CanvasTexture(c);
  if (srgb) t.colorSpace = THREE.SRGBColorSpace;
  t.anisotropy = 8;
  return t;
}

// A glow sprite: a soft ring/disc that pulses when a button is pressed (the video's own effect).
function glowTexture() {
  return canvasTex(128, 128, (g, w, h) => {
    const r = g.createRadialGradient(64, 64, 8, 64, 64, 62);
    r.addColorStop(0, 'rgba(255,255,255,0)');
    r.addColorStop(0.55, 'rgba(255,255,255,0.0)');
    r.addColorStop(0.72, 'rgba(255,255,255,0.95)');
    r.addColorStop(1, 'rgba(255,255,255,0)');
    g.fillStyle = r; g.fillRect(0, 0, w, h);
  });
}

export function buildDevice({ screenTexture, envIntensity = 1 }) {
  const root = new THREE.Group();      // positioned/rotated by the timeline
  const parts = {};                    // pressable parts by name
  const glows = {};

  const white = new THREE.MeshPhysicalMaterial({
    color: 0xf2f3f5, roughness: 0.36, metalness: 0, clearcoat: 1, clearcoatRoughness: 0.09,
    envMapIntensity: envIntensity,
  });
  const capWhite = new THREE.MeshPhysicalMaterial({
    color: 0xf7f8fa, roughness: 0.28, clearcoat: 0.8, clearcoatRoughness: 0.12, envMapIntensity: envIntensity,
  });
  const rimMat = new THREE.MeshPhysicalMaterial({
    color: 0xc9ced6, roughness: 0.32, metalness: 0.75, clearcoat: 0.4, envMapIntensity: envIntensity,
  });
  const wellMat = new THREE.MeshStandardMaterial({ color: 0x9aa0a9, roughness: 0.65, envMapIntensity: envIntensity * 0.6 });
  const darkWell = new THREE.MeshStandardMaterial({ color: 0x6f757e, roughness: 0.7, envMapIntensity: envIntensity * 0.5 });
  const glassMat = new THREE.MeshPhysicalMaterial({
    color: 0x040506, roughness: 0.07, metalness: 0, clearcoat: 1, clearcoatRoughness: 0.03, envMapIntensity: envIntensity * 0.8,
  });
  const stickMat = new THREE.MeshPhysicalMaterial({ color: 0xe3e6ea, roughness: 0.62, clearcoat: 0.1, envMapIntensity: envIntensity });
  const shoulderMat = new THREE.MeshPhysicalMaterial({ color: 0xe6e9ed, roughness: 0.3, clearcoat: 0.7, clearcoatRoughness: 0.15, envMapIntensity: envIntensity });

  // ---- body
  const bodyGeo = new THREE.ExtrudeGeometry(bodyShape(0), {
    depth: 12.6, bevelEnabled: true, bevelThickness: 3.0, bevelSize: 2.3, bevelOffset: -2.3, bevelSegments: 7, curveSegments: 72,
  });
  bodyGeo.translate(0, 0, -6.3);
  const body = new THREE.Mesh(bodyGeo, white);
  root.add(body);

  // the silver band round the edge
  const bandShape = bodyShape(0.28);
  bandShape.holes.push(new THREE.Path(bodyShape(-1.2).getPoints(160)));
  const bandGeo = new THREE.ExtrudeGeometry(bandShape, { depth: 4.2, bevelEnabled: true, bevelThickness: 0.25, bevelSize: 0.2, bevelOffset: -0.2, bevelSegments: 2, curveSegments: 72 });
  bandGeo.translate(0, 0, -1.6);
  root.add(new THREE.Mesh(bandGeo, rimMat));

  // ---- the front glass panel and the screen
  const edge = new THREE.Mesh(new THREE.ShapeGeometry(roundRect(117.6, 70.7, 3.3), 12), new THREE.MeshStandardMaterial({ color: 0xc4c9d0, roughness: 0.4, metalness: 0.3 }));
  edge.position.z = FRONT + 0.01; root.add(edge);
  const glassGeo = new THREE.ExtrudeGeometry(roundRect(117, 70.1, 3.0), { depth: 0.2, bevelEnabled: true, bevelThickness: 0.18, bevelSize: 0.18, bevelOffset: -0.18, bevelSegments: 2, curveSegments: 10 });
  glassGeo.translate(0, 0, FRONT + 0.02);
  root.add(new THREE.Mesh(glassGeo, glassMat));

  const screenMat = new THREE.MeshPhysicalMaterial({
    color: 0x000000, emissive: 0xffffff, emissiveMap: screenTexture, emissiveIntensity: 1,
    roughness: 0.08, metalness: 0, clearcoat: 0.25, clearcoatRoughness: 0.05, envMapIntensity: envIntensity * 0.12, toneMapped: false,
  });
  const screen = new THREE.Mesh(new THREE.PlaneGeometry(SCREEN.w, SCREEN.h), screenMat);
  screen.position.z = SCREEN.z;
  root.add(screen);
  parts.screen = screen;

  // front camera
  const cam = new THREE.Mesh(new THREE.CircleGeometry(1.2, 32), new THREE.MeshPhysicalMaterial({ color: 0x0b1020, roughness: 0.1, metalness: 0.6, clearcoat: 1 }));
  cam.position.set(53, 33.25, GLASS_Z + 0.02); root.add(cam);
  const camHi = new THREE.Mesh(new THREE.CircleGeometry(0.42, 16), new THREE.MeshBasicMaterial({ color: 0x3a4a6a }));
  camHi.position.set(52.6, 33.6, GLASS_Z + 0.03); root.add(camHi);

  // ---- helpers for buttons
  const gtex = glowTexture();
  function glow(name, x, y, z, size, sx = 1, sy = 1) {
    const m = new THREE.Mesh(new THREE.PlaneGeometry(size * sx, size * sy), new THREE.MeshBasicMaterial({
      map: gtex, transparent: true, opacity: 0, depthWrite: false, blending: THREE.AdditiveBlending, color: 0x8ff5ea, toneMapped: false,
    }));
    m.position.set(x, y, z); m.renderOrder = 5; root.add(m); glows[name] = m; return m;
  }
  function ring(x, y, r0, r1, mat, z = FRONT + 0.02) {
    const m = new THREE.Mesh(new THREE.RingGeometry(r0, r1, 64), mat); m.position.set(x, y, z); root.add(m); return m;
  }
  function capGeo(r, h) {
    const pts = [
      new THREE.Vector2(0, h), new THREE.Vector2(r * 0.55, h - 0.03), new THREE.Vector2(r - 0.55, h - 0.12),
      new THREE.Vector2(r - 0.18, h - 0.32), new THREE.Vector2(r, h - 0.7), new THREE.Vector2(r, 0),
    ];
    const g = new THREE.LatheGeometry(pts, 48); g.rotateX(Math.PI / 2); return g;
  }
  function symbolTex(kind) {
    return canvasTex(128, 128, (g) => {
      g.strokeStyle = '#fff'; g.lineWidth = 11; g.lineCap = 'round'; g.lineJoin = 'round';
      g.beginPath();
      if (kind === 'triangle') { g.moveTo(64, 30); g.lineTo(97, 88); g.lineTo(31, 88); g.closePath(); }
      if (kind === 'circle') { g.arc(64, 64, 30, 0, Math.PI * 2); }
      if (kind === 'cross') { g.moveTo(40, 40); g.lineTo(88, 88); g.moveTo(88, 40); g.lineTo(40, 88); }
      if (kind === 'square') { g.rect(37, 37, 54, 54); }
      if (kind === 'ps') { g.lineWidth = 3; g.globalAlpha = 0.55; g.arc(64, 64, 44, 0, Math.PI * 2); }
      g.stroke();
    }, false);
  }
  function roundButton(name, x, y, r, sym) {
    ring(x, y, r + 0.05, r + 0.6, wellMat);
    const grp = new THREE.Group(); grp.position.set(x, y, FRONT);
    const h = 1.5;
    grp.add(new THREE.Mesh(capGeo(r, h), capWhite.clone()));
    if (sym) {
      const d = new THREE.Mesh(new THREE.CircleGeometry(r * 0.9, 40), new THREE.MeshStandardMaterial({
        color: 0x737b86, alphaMap: symbolTex(sym), transparent: true, roughness: 0.45, depthWrite: false,
      }));
      d.position.z = h + 0.005; grp.add(d);
    }
    root.add(grp); parts[name] = { obj: grp, axis: 'z', travel: -0.75, base: grp.position.clone() };
    glow(name, x, y, FRONT + h + 0.3, r * 3.6);
  }

  // ---- face buttons (PR #134's layout, 0.1 mm -> mm)
  roundButton('triangle', 74, 24.25, 3.9, 'triangle');
  roundButton('circle', 83.5, 14.75, 3.9, 'circle');
  roundButton('cross', 74, 5.25, 3.9, 'cross');
  roundButton('square', 64.5, 14.75, 3.9, 'square');
  roundButton('ps', -76, -28.25, 3.4, 'ps');

  // ---- d-pad: four separate buttons in a cross
  const DX = -72.5, DY = 14.75;
  {
    const wellA = new THREE.Mesh(new THREE.ShapeGeometry(roundRect(9.0, 19.6, 2.2, DX, DY), 8), darkWell);
    const wellB = new THREE.Mesh(new THREE.ShapeGeometry(roundRect(19.6, 9.0, 2.2, DX, DY), 8), darkWell);
    wellA.position.z = wellB.position.z = FRONT + 0.015; root.add(wellA, wellB);
    const w = 7.3, L = 8.9, tip = 1.1;
    const arm = new THREE.Shape();
    arm.moveTo(0, tip);
    arm.lineTo(w / 2, tip + w / 2 - 0.4);
    arm.lineTo(w / 2, L - 1.7); arm.quadraticCurveTo(w / 2, L, w / 2 - 1.7, L);
    arm.lineTo(-w / 2 + 1.7, L); arm.quadraticCurveTo(-w / 2, L, -w / 2, L - 1.7);
    arm.lineTo(-w / 2, tip + w / 2 - 0.4);
    arm.lineTo(0, tip);
    const armGeo = new THREE.ExtrudeGeometry(arm, { depth: 1.0, bevelEnabled: true, bevelThickness: 0.45, bevelSize: 0.4, bevelOffset: -0.4, bevelSegments: 3, curveSegments: 6 });
    armGeo.translate(0, 0, 0.45);
    const dirs = { up: 0, left: Math.PI / 2, down: Math.PI, right: -Math.PI / 2 };
    for (const [d, a] of Object.entries(dirs)) {
      const grp = new THREE.Group(); grp.position.set(DX, DY, FRONT); grp.rotation.z = a;
      grp.add(new THREE.Mesh(armGeo, capWhite.clone()));
      // a small arrow notch
      const n = new THREE.Mesh(new THREE.CircleGeometry(0.75, 3), new THREE.MeshStandardMaterial({ color: 0xb4bac2, roughness: 0.5 }));
      n.position.set(0, L - 3.0, 1.92); n.rotation.z = Math.PI / 2; grp.add(n);
      root.add(grp);
      parts['dpad-' + d] = { obj: grp, axis: 'z', travel: -0.6, base: grp.position.clone() };
      const gx = DX + Math.sin(-a) * 5.0, gy = DY + Math.cos(a) * 5.0;
      glow('dpad-' + d, gx, gy, FRONT + 2.4, 11);
    }
  }

  // ---- analog sticks
  function stick(name, x, y) {
    ring(x, y, 6.25, 7.1, darkWell);
    ring(x, y, 7.1, 7.35, new THREE.MeshBasicMaterial({ color: 0xffffff, transparent: true, opacity: 0.85 }), FRONT + 0.021);
    const grp = new THREE.Group(); grp.position.set(x, y, FRONT);
    const pts = [
      new THREE.Vector2(0.01, 3.0), new THREE.Vector2(2.4, 3.06), new THREE.Vector2(4.4, 3.28), new THREE.Vector2(5.35, 3.62),
      new THREE.Vector2(5.8, 3.66), new THREE.Vector2(6.05, 3.42), new THREE.Vector2(6.05, 2.55), new THREE.Vector2(5.6, 2.3),
      new THREE.Vector2(2.7, 2.3), new THREE.Vector2(2.5, 0.0),
    ];
    const g = new THREE.LatheGeometry(pts, 56); g.rotateX(Math.PI / 2);
    grp.add(new THREE.Mesh(g, stickMat));
    root.add(grp);
    parts[name] = { obj: grp, axis: 'tilt', base: grp.position.clone() };
  }
  stick('stick-l', -66, -12.75);
  stick('stick-r', 66, -12.75);

  // ---- select / start
  function pill(name, x, y) {
    const wl = new THREE.Mesh(new THREE.ShapeGeometry(roundRect(7.0, 3.2, 1.6, x, y), 10), wellMat);
    wl.position.z = FRONT + 0.02; root.add(wl);
    const geo = new THREE.ExtrudeGeometry(roundRect(6.4, 2.6, 1.3), { depth: 0.6, bevelEnabled: true, bevelThickness: 0.3, bevelSize: 0.3, bevelOffset: -0.3, bevelSegments: 3, curveSegments: 8 });
    geo.translate(0, 0, 0.3);
    const grp = new THREE.Group(); grp.position.set(x, y, FRONT); grp.add(new THREE.Mesh(geo, capWhite.clone())); root.add(grp);
    parts[name] = { obj: grp, axis: 'z', travel: -0.4, base: grp.position.clone() };
    glow(name, x, y, FRONT + 1.4, 11, 1.6, 0.9);
  }
  pill('select', 68, -29.75);
  pill('start', 78, -29.75);

  // printed labels and speaker grilles: one decal over the lower front
  {
    const tex = canvasTex(2048, 256, (g, w, h) => {
      // decal spans X -91..91 (182 mm), Y -42..-31 (11 mm): 11.25 px per mm horizontally, 23.3 vertically
      const px = (X) => (X + 91) / 182 * w, py = (Y) => (-31 - Y) / 11 * h;
      g.fillStyle = '#9aa1ab';
      g.font = '600 19px "Noto Sans JP", sans-serif'; g.textAlign = 'center'; g.textBaseline = 'middle';
      g.save(); g.scale(1, 2.07); g.fillText('SELECT', px(68), py(-33.6) / 2.07); g.fillText('START', px(78), py(-33.6) / 2.07); g.restore();
      g.fillStyle = '#6d747e';
      for (const cx of [-60, 60]) for (let r = 0; r < 3; r++) for (let i = 0; i < 10; i++) {
        const X = cx - 4.05 + i * 0.9 + (r % 2) * 0.45, Y = -36.4 - r * 1.05;
        g.beginPath(); g.ellipse(px(X), py(Y), 3.6, 7.4, 0, 0, Math.PI * 2); g.fill();
      }
      g.beginPath(); g.ellipse(px(0), py(-38.95), 3.4, 7.0, 0, 0, Math.PI * 2); g.fill();
    });
    const decal = new THREE.Mesh(new THREE.PlaneGeometry(182, 11), new THREE.MeshStandardMaterial({ map: tex, transparent: true, roughness: 0.5, depthWrite: false }));
    decal.position.set(0, -36.5, FRONT + 0.025); root.add(decal);
  }

  // ---- shoulder buttons (L/R): the outline of PR #134's drawing, extruded towards the back
  function shoulder(name, mirror) {
    const m = mirror ? -1 : 1;
    const s = new THREE.Shape();
    const P = (x, y) => [m * x, y];
    s.moveTo(...P(-48, 40.05)); s.lineTo(...P(-48, 42.25));
    s.quadraticCurveTo(...P(-48, 44.25), ...P(-50, 44.25));
    s.lineTo(...P(-58, 44.25));
    s.bezierCurveTo(...P(-73, 44.25), ...P(-81.5, 37.75), ...P(-84.56, 31.24));
    s.lineTo(...P(-82, 30.25));
    s.bezierCurveTo(...P(-78.8, 36.75), ...P(-71, 40.05), ...P(-58, 40.05));
    s.lineTo(...P(-48, 40.05));
    const geo = new THREE.ExtrudeGeometry(s, { depth: 8.4, bevelEnabled: true, bevelThickness: 0.7, bevelSize: 0.6, bevelOffset: -0.6, bevelSegments: 4, curveSegments: 24 });
    geo.translate(0, 0, -7.6);
    const grp = new THREE.Group(); grp.add(new THREE.Mesh(geo, shoulderMat.clone())); root.add(grp);
    parts[name] = { obj: grp, axis: 'y', travel: -1.1, base: grp.position.clone() };
    const gl = glow(name, m * -66, 41.5, 2.5, 34, 1.0, 0.5);
    gl.rotation.x = -Math.PI / 2; gl.position.set(m * -67, 45.6, -3.0);
  }
  shoulder('l', false);
  shoulder('r', true);

  // ---- top-edge details: power, volume, game card slot, accessory port
  {
    const topY = 41.75;
    const pillGeo = (len) => { const g = new THREE.ExtrudeGeometry(roundRect(len, 3.0, 1.4), { depth: 0.5, bevelEnabled: true, bevelThickness: 0.25, bevelSize: 0.25, bevelOffset: -0.25, bevelSegments: 2 }); g.rotateX(-Math.PI / 2); return g; };
    const pw = new THREE.Mesh(pillGeo(7), capWhite); pw.position.set(-38, topY - 0.1, -1.5); root.add(pw);
    parts.power = { obj: pw, axis: 'y', travel: -0.4, base: pw.position.clone() };
    for (const x of [-27, -21]) { const v = new THREE.Mesh(pillGeo(4.5), capWhite); v.position.set(x, topY - 0.1, -1.5); root.add(v); }
    const slot = new THREE.Mesh(new THREE.PlaneGeometry(22, 4.2), new THREE.MeshStandardMaterial({ color: 0xdfe2e6, roughness: 0.45 }));
    slot.rotation.x = -Math.PI / 2; slot.position.set(30, topY + 0.02, -1.5); root.add(slot);
    const port = new THREE.Mesh(new THREE.PlaneGeometry(7, 1.6), new THREE.MeshStandardMaterial({ color: 0x3a3f46, roughness: 0.6 }));
    port.rotation.x = -Math.PI / 2; port.position.set(50, topY + 0.03, -1.5); root.add(port);
  }

  // ---- the back: a shallow touchpad panel and the rear camera
  {
    const pad = new THREE.Mesh(new THREE.ShapeGeometry(roundRect(118, 62, 6, 0, -2), 10), new THREE.MeshPhysicalMaterial({ color: 0xeceef1, roughness: 0.55, clearcoat: 0.2 }));
    pad.position.z = -FRONT - 0.02; pad.rotation.y = Math.PI; root.add(pad);
    const rc = new THREE.Mesh(new THREE.CircleGeometry(1.3, 24), new THREE.MeshPhysicalMaterial({ color: 0x0b1020, roughness: 0.1, metalness: 0.6 }));
    rc.position.set(0, 36, -FRONT - 0.03); rc.rotation.y = Math.PI; root.add(rc);
  }

  // ---- press API: amt 0..1 is the travel; age (s since the press began, or <0) drives a ripple and a tint
  const tint = new THREE.Color(0x63e6da);
  function setPress(name, amt, age = -1) {
    const p = parts[name]; if (!p) return;
    const o = p.obj;
    o.position.copy(p.base);
    if (p.axis === 'z') o.position.z += p.travel * amt;
    if (p.axis === 'y') o.position.y += p.travel * amt;
    o.traverse((m) => {
      if (!m.isMesh || !m.material.color || m.material === screenMat) return;
      if (!m.userData.base) m.userData.base = m.material.color.clone();
      m.material.color.copy(m.userData.base).lerp(tint, 0.55 * amt);
    });
    for (const gname of [name, name + '-front']) {
      const gl = glows[gname]; if (!gl) continue;
      const k = age >= 0 && age < 0.45 ? age / 0.45 : 1;
      gl.material.opacity = age >= 0 && age < 0.45 ? (1 - k) * 0.9 : 0;
      const sc = 0.75 + k * 0.9; gl.scale.set(sc, sc, sc);
    }
  }
  function setStick(name, x, y) {
    const p = parts[name]; if (!p) return;
    p.obj.rotation.set(-y * 0.32, x * 0.32, 0);
  }
  function resetPresses() { for (const n of Object.keys(parts)) { if (parts[n].axis === 'tilt') setStick(n, 0, 0); else if (parts[n].base) setPress(n, 0); } }

  return { root, parts, setPress, setStick, resetPresses, screenMat, FRONT };
}

// Where a button is, in device coordinates (for touch ripples, camera targets, card launches).
export const ANCHORS = {
  screen: [0, 0, SCREEN.z], l: [-66, 44, -2], r: [66, 44, -2], top: [0, 42, -1],
  dpad: [-72.5, 14.75, FRONT], face: [74, 14.75, FRONT], cross: [74, 5.25, FRONT], ps: [-76, -28.25, FRONT],
};
