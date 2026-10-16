// The player's hands, built from code: stylized smooth fingers (spheres and tapered cylinders), not a
// scan or a photo. They hold the console as players do: palms round the grips, index fingers on L and
// R, the other fingers curled behind, thumbs on the front. A thumb reaches a button by two-bone IK
// and presses it; the index fingers press L and R. Units: millimetres, in the console's own frame.
import * as THREE from 'three';
import { clamp, lerp, smooth, sway } from './util.js';

const V = (x, y, z) => new THREE.Vector3(x, y, z);

// Thumb targets: where the tip's centre hovers over a button and where it is when the button is down.
const FACE_TOP = 10.8, DPAD_TOP = 11.2, TIP_R = 8.0;
const T_ = {
  cross: [74.6, 4.2], circle: [84.0, 14.0], triangle: [74.3, 23.8], square: [64.8, 14.4],
  start: [78.0, -29.6], select: [68.0, -29.6],
  'dpad-right': [-67.2, 14.2], 'dpad-left': [-77.6, 14.4], 'dpad-up': [-72.4, 19.8], 'dpad-down': [-72.4, 9.6], ps: [-75.8, -27.8],
};
export const THUMB_OF = (b) => (['cross', 'circle', 'triangle', 'square', 'start', 'select'].includes(b) ? 'right' : (b.startsWith('dpad') || b === 'ps' ? 'left' : null));
function targets(b) {
  const [x, y] = T_[b];
  const top = b.startsWith('dpad') ? DPAD_TOP : b === 'start' || b === 'select' ? 10.5 : FACE_TOP;
  const travel = b.startsWith('dpad') ? 0.6 : b === 'start' || b === 'select' ? 0.4 : 0.75;
  return { hover: V(x + (x > 0 ? 0.8 : -0.4), y - 0.7, top + TIP_R + 3.4), press: V(x, y, top + TIP_R - travel) };
}
const REST = { right: V(72.0, -3.0, 21.6), left: V(-70.5, -1.0, 21.6) };

export function buildHands(device, { skin = 0xe2a684 } = {}) {
  const group = new THREE.Group();
  device.root.add(group);
  const skinMat = new THREE.MeshPhysicalMaterial({
    color: skin, roughness: 0.6, metalness: 0, sheen: 0.35, sheenRoughness: 0.8, sheenColor: new THREE.Color(0xffd2bd),
    clearcoat: 0.06, clearcoatRoughness: 0.7,
  });
  const nailMat = new THREE.MeshPhysicalMaterial({ color: 0xf7ddd2, roughness: 0.28, clearcoat: 0.7, clearcoatRoughness: 0.25 });
  const UP = V(0, 1, 0);
  const ball = (r, mat = skinMat) => { const m = new THREE.Mesh(new THREE.SphereGeometry(r, 32, 22), mat); group.add(m); return m; };
  const seg = (r0, r1) => { const m = new THREE.Mesh(new THREE.CylinderGeometry(r1, r0, 1, 32, 1, true), skinMat); group.add(m); return m; };
  const place = (m, a, b) => { const d = b.clone().sub(a), L = Math.max(0.01, d.length()); m.position.copy(a).addScaledVector(d, 0.5); m.quaternion.setFromUnitVectors(UP, d.multiplyScalar(1 / L)); m.scale.set(1, L, 1); };
  const blob = (c, r, rot = [0, 0, 0]) => { const m = ball(1); m.position.copy(c); m.scale.set(...r); m.rotation.set(...rot); return m; };

  function chain(rs) { // joints with radii rs, a segment between each pair
    return { balls: rs.map((r) => ball(r)), segs: rs.slice(1).map((r, i) => seg(rs[i], r)) };
  }
  function setChain(c, pts) { pts.forEach((p, i) => c.balls[i].position.copy(p)); c.segs.forEach((s, i) => place(s, pts[i], pts[i + 1])); }
  function nail(len = 7.2, wid = 5.6) { const m = new THREE.Mesh(new THREE.SphereGeometry(1, 24, 16), nailMat); m.scale.set(wid, len, 1.5); group.add(m); return m; }
  const _q = new THREE.Quaternion(), _m = new THREE.Matrix4();
  function setNail(n, tip, prev, dorsal, r) { // on the back of the last segment, near the tip
    const ax = tip.clone().sub(prev).normalize();
    const up = dorsal.clone().sub(ax.clone().multiplyScalar(dorsal.dot(ax))).normalize();
    const side = new THREE.Vector3().crossVectors(ax, up);
    n.position.copy(tip).addScaledVector(up, r * 0.86).addScaledVector(ax, -r * 0.35);
    _m.makeBasis(side, ax, up); n.quaternion.setFromRotationMatrix(_m);
  }

  const hands = {};
  for (const side of ['right', 'left']) {
    const s = side === 'right' ? 1 : -1;
    const X = (v) => V(v[0] * s, v[1], v[2]);
    const h = { s };
    // the forearm, the wrist, the palm and the curled fingers behind the grip (fixed)
    // the forearm goes back and down, towards the player behind the console
    const W0 = X([106, -34, -26]), W1 = X([190, -175, -300]);
    place(seg(19.5, 26), W0, W1); blob(W0, [19.5, 19.5, 19.5]);
    blob(X([100, -2, -17]), [15.5, 33, 14.5], [0, 0, -0.16 * s]);
    blob(X([100.5, -25, -1]), [12.5, 15, 11.5], [0, 0, 0.25 * s]); // the thumb's base
    for (const f of [[[105, 11, -20], [89, 13, -17.4], [73, 12.5, -16.7], 7.4], [[104, -3, -20], [89, -4, -17.4], [76, -5, -16.7], 7.1], [[101, -16, -19], [90, -17.5, -17.1], [81, -18.5, -16.5], 6.3]]) {
      setChain(chain([f[3], f[3] * 0.96, f[3] * 0.92]), f.slice(0, 3).map(X));
    }
    // the index finger, on the shoulder button
    h.index = chain([8.2, 7.6, 7.0, 6.6]);
    h.indexRest = [X([106, 18, -16]), X([86, 39.2, -5.0]), X([70, 47.8, -3.6]), X([55, 51.0, -3.2])];
    h.indexNail = nail(6.6, 5.2);
    // the thumb: base M (fixed), the knuckle by IK, the tip on its target
    h.thumb = chain([9.6, 8.7, TIP_R]);
    h.thumbBase = X([101, -27, 3]);
    h.pole = X([8, 6, 40]);
    h.thumbNail = nail(7.4, 6.0);
    hands[side] = h;
  }

  // two-bone IK: base M, target T, bone lengths a and b, bending towards pole direction P
  function ik(M, T, a, b, P) {
    const d = T.clone().sub(M); let L = d.length();
    L = clamp(L, Math.abs(a - b) + 0.05, a + b - 0.05);
    const u = d.normalize();
    const cosA = (a * a + L * L - b * b) / (2 * a * L), sinA = Math.sqrt(Math.max(0, 1 - cosA * cosA));
    const pv = P.clone().sub(u.clone().multiplyScalar(P.dot(u))).normalize();
    const K = M.clone().addScaledVector(u, a * cosA).addScaledVector(pv, a * sinA);
    return [K, M.clone().addScaledVector(u, L)];
  }

  // a thumb's track: rest, then for each of its presses hover -> press -> hover, back to rest when idle
  function thumbTrack(events) {
    const keys = [];
    for (let i = 0; i < events.length; i++) {
      const e = events[i], prev = events[i - 1], next = events[i + 1];
      const tg = targets(e.btn);
      const fromRest = !prev || e.t - prev.t > 0.95;
      if (fromRest) keys.push({ t: e.t - 0.34, p: null });
      keys.push({ t: e.t - 0.05, p: tg.hover, e });
      keys.push({ t: e.t + 0.24, p: tg.hover, e });
      if (!next || next.t - e.t > 0.95) keys.push({ t: e.t + 0.7, p: null });
    }
    return keys;
  }

  let tracks = { right: [], left: [] };
  function setPresses(presses) {
    const ev = { right: [], left: [] };
    for (const p of presses) for (const b of p.btns || [p.btn]) { const th = THUMB_OF(b); if (th) ev[th].push({ t: p.t, btn: b }); }
    for (const k of ['right', 'left']) { ev[k].sort((a, b) => a.t - b.t); tracks[k] = thumbTrack(ev[k]); }
  }

  function thumbAt(side, t, pressAmt) {
    const keys = tracks[side], rest = REST[side].clone();
    rest.x += sway(t, side === 'right' ? 21 : 22, 0.5) * 0.9; rest.y += sway(t, side === 'right' ? 23 : 24, 0.45) * 0.9;
    if (!keys.length || t <= keys[0].t) return rest;
    let i = 0; while (i < keys.length - 1 && t > keys[i + 1].t) i++;
    const a = keys[i], b = keys[i + 1];
    const pa = a.p || rest;
    let pos;
    if (!b) pos = pa.clone();
    else { const k = smooth(clamp((t - a.t) / Math.max(1e-3, b.t - a.t))); pos = pa.clone().lerp(b.p || rest, k); }
    // the press itself: from hover down to the button, as far as the button travels
    const e = (a.e && Math.abs(t - a.e.t) < 0.5) ? a.e : (b && b.e && Math.abs(t - b.e.t) < 0.5 ? b.e : null);
    if (e) { const amt = pressAmt(e.btn); if (amt > 0) { const tg = targets(e.btn); pos.lerp(tg.press, amt); } }
    return pos;
  }

  function update(t, pressAmt, extra = {}) {
    for (const side of ['right', 'left']) {
      const h = hands[side];
      // thumb
      const T = thumbAt(side, t, pressAmt);
      const [K, tip] = ik(h.thumbBase, T, 31, 26, h.pole);
      setChain(h.thumb, [h.thumbBase, K, tip]);
      setNail(h.thumbNail, tip, K, h.pole.clone().normalize(), TIP_R);
      // index finger: pressing L or R bends it down onto the button; `slide` moves it to the power button
      const btn = side === 'right' ? 'r' : 'l';
      const amt = pressAmt(btn), slide = side === 'left' ? (extra.powerSlide || 0) : 0, pw = side === 'left' ? pressAmt('power') : 0;
      const off = [0, 0.35, 0.75, 1.0];
      const pts = h.indexRest.map((p, i) => {
        const q = p.clone();
        q.y -= (amt * 1.9 + pw * 1.3) * off[i];
        q.x += h.s * -1 * slide * [0, 5, 11, 16][i]; q.y -= slide * [0, 0.2, 1.4, 2.5][i]; q.z += slide * [0, 0.4, 1.1, 1.7][i];
        q.y += sway(t, side === 'right' ? 31 : 32, 0.4) * 0.35 * off[i];
        return q;
      });
      setChain(h.index, pts);
      setNail(h.indexNail, pts[3], pts[2], V(0, 1, 0.25), 6.6);
    }
  }

  return { group, setPresses, update };
}
