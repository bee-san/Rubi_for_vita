// Small deterministic helpers: easing, keyframes, value noise. No randomness depends on wall time.
export const clamp = (v, a = 0, b = 1) => Math.max(a, Math.min(b, v));
export const lerp = (a, b, k) => a + (b - a) * k;
export const inv = (a, b, v) => clamp((v - a) / (b - a));
export const smooth = (k) => k * k * (3 - 2 * k);
export const easeInOut = (k) => (k < 0.5 ? 4 * k * k * k : 1 - Math.pow(-2 * k + 2, 3) / 2);
export const easeOut = (k) => 1 - Math.pow(1 - k, 3);
export const easeIn = (k) => k * k * k;
export const easeOutExpo = (k) => (k >= 1 ? 1 : 1 - Math.pow(2, -10 * k));
export const easeInOutQuint = (k) => (k < 0.5 ? 16 * k ** 5 : 1 - Math.pow(-2 * k + 2, 5) / 2);
export const easeOutBack = (k, s = 1.5) => 1 + (s + 1) * Math.pow(k - 1, 3) + s * Math.pow(k - 1, 2);
export const EASES = { linear: (k) => k, smooth, inOut: easeInOut, out: easeOut, in: easeIn, expo: easeOutExpo, quint: easeInOutQuint, back: easeOutBack };

// Keyframes: [{t, ...values, ease}] -> the interpolated values at time t (numbers and arrays of numbers).
export function keyed(keys, t) {
  if (t <= keys[0].t) return keys[0];
  for (let i = 1; i < keys.length; i++) {
    const a = keys[i - 1], b = keys[i];
    if (t <= b.t) {
      const e = EASES[b.ease || 'inOut'];
      const k = e(inv(a.t, b.t, t));
      const out = { t };
      for (const key of Object.keys(b)) {
        if (key === 't' || key === 'ease') continue;
        const va = a[key] ?? b[key], vb = b[key];
        out[key] = Array.isArray(vb) ? vb.map((v, j) => lerp(va[j], v, k)) : (typeof vb === 'number' ? lerp(va, vb, k) : (k < 0.5 ? va : vb));
      }
      for (const key of Object.keys(a)) if (!(key in out) && key !== 't' && key !== 'ease') out[key] = a[key];
      return out;
    }
  }
  return keys[keys.length - 1];
}

// A press: down in 50 ms, held, up in 90 ms. Returns 0..1.
export function pressAmt(presses, name, t) {
  let v = 0;
  for (const p of presses) {
    if (p.btn !== name && !(p.btns && p.btns.includes(name))) continue;
    const hold = p.hold ?? 0.12;
    if (t < p.t || t > p.t + 0.05 + hold + 0.09) continue;
    const lt = t - p.t;
    const a = lt < 0.05 ? easeOut(lt / 0.05) : lt < 0.05 + hold ? 1 : 1 - easeOut((lt - 0.05 - hold) / 0.09);
    v = Math.max(v, a);
  }
  return v;
}

// 1-D value noise, smooth, seeded; used for the hand-held sway.
function hash(n) { const s = Math.sin(n * 127.1 + 311.7) * 43758.5453123; return s - Math.floor(s); }
export function noise1(x, seed = 0) {
  const i = Math.floor(x), f = x - i, u = f * f * (3 - 2 * f);
  return lerp(hash(i + seed * 101.3), hash(i + 1 + seed * 101.3), u) * 2 - 1;
}
export const sway = (t, seed, speed = 0.35) => noise1(t * speed, seed) * 0.6 + noise1(t * speed * 2.3, seed + 7) * 0.4;

// a seeded PRNG for particle layouts
export function rng(seed) { let s = seed >>> 0; return () => { s = (s * 1664525 + 1013904223) >>> 0; return s / 4294967296; }; }
