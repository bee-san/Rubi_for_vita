      // The promo's one paused GSAP timeline (HyperFrames seeks it frame by frame). It is built
      // synchronously and every change is an explicit fromTo at a fixed time, so a cold seek to any
      // frame renders the same picture.
      (function () {
        const root = document.getElementById("root");
        const P = root.dataset.layout === "portrait";
        const tl = gsap.timeline({ paused: true });
        const T = /*{{TIMES}}*/ null; // scene starts (src/script.mjs)
        const C = /*{{CUES}}*/ null; // the cue sheet: every press, tap and change
        const K = /*{{CAPS}}*/ null; // the captions' times (src/script.mjs CAPTIONS)
        const GH_VPK_ROW = /*{{GHROW}}*/ 0; // the main download's row on the Releases page (GitHub's order)

        // ------------------------------------------------------------ helpers
        function fade(sel, t, from, to, dur = 0.2, ease = "power1.out") {
          tl.fromTo(sel, { opacity: from }, { opacity: to, duration: dur, ease, immediateRender: false }, t);
        }
        const on = (sel, t, dur = 0.01) => fade(sel, t, 0, 1, dur);
        const off = (sel, t, dur = 0.01) => fade(sel, t, 1, 0, dur);
        // b replaces a (a short crossfade, b first so nothing shows through)
        function swap(a, b, t, dur = 0.12) {
          if (b) fade(b, t, 0, 1, dur);
          if (a) fade(a, t + dur * 0.7, 1, 0, Math.max(0.01, dur * 0.5));
        }
        // a console screen opens as the Vita opens one: it grows in from s0
        function grow(sel, t, s0 = 0.9, dur = 0.24) {
          tl.fromTo(sel, { opacity: 0, scale: s0 }, { opacity: 1, scale: 1, duration: dur, ease: "power2.out", immediateRender: false }, t);
        }
        // the game's text: its characters appear one by one
        function type(sel, t, per = 0.045) {
          tl.fromTo(sel + " .c", { opacity: 0 }, { opacity: 1, duration: 0.01, stagger: per, immediateRender: false }, t);
        }
        // deterministic noise (petals, sparkles)
        let seed = 20261016;
        const rnd = () => {
          seed = (seed * 1664525 + 1013904223) >>> 0;
          return seed / 4294967296;
        };

        // ------------------------------------------------------------ the camera
        // #cam-zoom scales, #cam-rot turns about the look-at point, #cam-pan brings the world point
        // (x, y) to the frame's action centre. CAM: where the camera is for each shot.
        const CAM = P
          ? {
              macro0: { x: 0, y: 236, s: 1.18, rx: 7, ry: -12, rz: -2 },
              macro: { x: 0, y: 236, s: 1.3, rx: 5, ry: -8, rz: -1 },
              hero: { x: 0, y: 20, s: 0.84, rx: 10, ry: -16, rz: -1 },
              heroB: { x: 0, y: 30, s: 0.88, rx: 8, ry: -10, rz: 0 },
              shoulders: { x: 0, y: -330, s: 0.82, rx: 34, ry: 0, rz: 0 },
              screen: { x: 0, y: 22, s: 0.96, rx: 2, ry: -3, rz: 0 },
              screenIn: { x: -12, y: -24, s: 1.01, rx: 1.5, ry: -2.5, rz: 0 },
              screenB: { x: -30, y: 30, s: 1.02, rx: 3, ry: -5, rz: 0 },
              popupZoom: { x: -30, y: -130, s: 1.1, rx: 1, ry: -2, rz: 0 },
              offZoom: { x: 72, y: 100, s: 0.8, rx: 2, ry: -3, rz: 0 }, // "Saved. ..." and OFF inside x 60-900
              offZoomB: { x: 80, y: 135, s: 0.83, rx: 3.5, ry: -1.5, rz: 0 }, // a slow drift in, OFF still in frame
              mine: { x: 0, y: 330, s: 0.84, rx: 8, ry: 8, rz: 0 },
              mineB: { x: 0, y: 340, s: 0.86, rx: 6, ry: 4, rz: 0 },
              readAppB: { x: 0, y: -60, s: 1.04, rx: 3, ry: 4, rz: 0 },
              installDevB: { x: 10, y: 30, s: 0.98, rx: 4, ry: -3, rz: 0 },
              power: { x: -150, y: -150, s: 0.9, rx: 20, ry: 6, rz: 0 },
              powerB: { x: -170, y: -170, s: 0.99, rx: 22, ry: 7, rz: 0 },
              deck: { x: 0, y: 800, s: 0.9, rx: 0, ry: 0, rz: 0 },
              anki: { x: 0, y: 1840, s: 0.64, rx: 0, ry: 0, rz: 0 },
              ankiPic: { x: 320, y: 1560, s: 1.3, rx: 2, ry: -4, rz: 0 },
              ankiAll: { x: 0, y: 1860, s: 0.62, rx: -2, ry: 4, rz: 0 },
              read: { x: 0, y: 20, s: 0.84, rx: 6, ry: 12, rz: 0.5 },
              readB: { x: 0, y: 40, s: 0.88, rx: 5, ry: 6, rz: 0 },
              readApp: { x: 0, y: -70, s: 1.0, rx: 2, ry: 3, rz: 0 },
              installWide: { x: 0, y: -700, s: 0.36, rx: 4, ry: 0, rz: 0 },
              laptop: { x: 0, y: -1340, s: 0.6, rx: 2, ry: 6, rz: 0 },
              installDev: { x: 0, y: 22, s: 0.9, rx: 5, ry: -6, rz: 0 },
              home: { x: 0, y: 22, s: 0.94, rx: 6, ry: 8, rz: 0 },
              shoulders2: { x: 0, y: -330, s: 0.8, rx: 30, ry: 16, rz: 2 },
              shoulders3: { x: 0, y: -280, s: 0.78, rx: 26, ry: 12, rz: 1.5 },
              screen3: { x: 0, y: 22, s: 0.97, rx: 2, ry: 2, rz: 0 },
              app8B: { x: 0, y: 30, s: 0.9, rx: 3, ry: -5, rz: 0 }, // the Games page's right column in the frame
              out: { x: 0, y: 40, s: 0.3, rx: 12, ry: -40, rz: -4 },
            }
          : {
              macro0: { x: -80, y: 216, s: 1.62, rx: 8, ry: -14, rz: -2.5 },
              macro: { x: -40, y: 216, s: 1.85, rx: 6, ry: -10, rz: -1.5 },
              hero: { x: 0, y: -20, s: 0.8, rx: 9, ry: -14, rz: -1 },
              heroB: { x: 20, y: -10, s: 0.84, rx: 7, ry: -9, rz: 0 },
              shoulders: { x: 0, y: -320, s: 1.02, rx: 32, ry: 0, rz: 0 },
              screen: { x: 0, y: -30, s: 1.22, rx: 2, ry: -3, rz: 0 },
              screenIn: { x: -24, y: -64, s: 1.34, rx: 1.5, ry: -2.5, rz: 0 },
              screenB: { x: 40, y: -24, s: 1.26, rx: 3, ry: -5, rz: 0 },
              popupZoom: { x: -60, y: -130, s: 1.75, rx: 1, ry: -2, rz: 0 },
              offZoom: { x: 60, y: 150, s: 1.6, rx: 2, ry: -3, rz: 0 },
              offZoomB: { x: 40, y: 140, s: 1.7, rx: 2.5, ry: -2, rz: 0 },
              mine: { x: 380, y: -10, s: 0.74, rx: 8, ry: 12, rz: 0 },
              mineB: { x: 430, y: 0, s: 0.77, rx: 6, ry: 8, rz: 0 },
              readAppB: { x: 30, y: -60, s: 1.7, rx: 3, ry: 5, rz: 0 },
              installDevB: { x: 20, y: 10, s: 0.98, rx: 4, ry: -3, rz: 0 },
              power: { x: -260, y: -170, s: 1.0, rx: 20, ry: 6, rz: 0 },
              powerB: { x: -300, y: -195, s: 1.12, rx: 22, ry: 7, rz: 0 },
              deck: { x: 1330, y: 20, s: 1.0, rx: 0, ry: 0, rz: 0 },
              anki: { x: 2950, y: 40, s: 0.78, rx: 0, ry: 0, rz: 0 },
              ankiPic: { x: 3080, y: -170, s: 1.08, rx: 2, ry: -4, rz: 0 },
              ankiAll: { x: 2950, y: 60, s: 0.8, rx: -2, ry: 4, rz: 0 },
              read: { x: 0, y: 0, s: 0.86, rx: 6, ry: 10, rz: 0.5 },
              readB: { x: -20, y: 10, s: 0.92, rx: 5, ry: 6, rz: 0 },
              readApp: { x: 0, y: -70, s: 1.62, rx: 2, ry: 3, rz: 0 },
              installWide: { x: -1250, y: 10, s: 0.42, rx: 4, ry: 0, rz: 0 },
              laptop: { x: -2600, y: 10, s: 0.84, rx: 2, ry: 7, rz: 0 },
              installDev: { x: 0, y: 0, s: 0.92, rx: 6, ry: -6, rz: 0 },
              home: { x: 0, y: 0, s: 0.9, rx: 7, ry: 8, rz: 0 },
              shoulders2: { x: 0, y: -330, s: 1.0, rx: 30, ry: 16, rz: 2 },
              shoulders3: { x: 0, y: -290, s: 0.96, rx: 26, ry: 12, rz: 1.5 },
              screen3: { x: 0, y: -28, s: 1.24, rx: 2, ry: 2, rz: 0 },
              app8B: { x: 40, y: -24, s: 1.26, rx: 3, ry: -5, rz: 0 },
              out: { x: 0, y: 40, s: 0.42, rx: 12, ry: -40, rz: -4 },
            };
        let cam = null;
        function camSet(s) {
          gsap.set("#cam-zoom", { scale: s.s });
          gsap.set("#cam-rot", { rotationX: s.rx, rotationY: s.ry, rotationZ: s.rz });
          gsap.set("#cam-pan", { x: -s.x, y: -s.y });
          cam = s;
        }
        function camTo(t, name, dur = 0.8, ease = "power3.inOut") {
          const a = cam, s = CAM[name];
          tl.fromTo("#cam-zoom", { scale: a.s }, { scale: s.s, duration: dur, ease, immediateRender: false }, t);
          tl.fromTo("#cam-rot", { rotationX: a.rx, rotationY: a.ry, rotationZ: a.rz }, { rotationX: s.rx, rotationY: s.ry, rotationZ: s.rz, duration: dur, ease, immediateRender: false }, t);
          tl.fromTo("#cam-pan", { x: -a.x, y: -a.y }, { x: -s.x, y: -s.y, duration: dur, ease, immediateRender: false }, t);
          cam = s;
        }
        // a fast move with a horizontal motion blur that peaks at its fastest (the whip-pan recipe:
        // feGaussianBlur on x only, capped)
        function whip(t, name, dur = 0.6) {
          camTo(t, name, dur, "power3.inOut");
          tl.fromTo("#viewport", { filter: "none" }, { filter: "url(#whip)", duration: 0.01, immediateRender: false }, t);
          tl.fromTo("#whip-blur", { attr: { stdDeviation: "0 0" } }, { attr: { stdDeviation: "22 0" }, duration: dur / 2, ease: "power3.in", immediateRender: false }, t);
          tl.fromTo("#whip-blur", { attr: { stdDeviation: "22 0" } }, { attr: { stdDeviation: "0 0" }, duration: dur / 2, ease: "power3.out", immediateRender: false }, t + dur / 2);
          tl.fromTo("#viewport", { filter: "url(#whip)" }, { filter: "none", duration: 0.01, immediateRender: false }, t + dur);
        }

        // ------------------------------------------------------------ the console's buttons and hands
        // Device coordinates (assets/device/vita-white.svg, 0.1 mm units) of what is pressed.
        const B = {
          L: [292, 44], R: [1528, 44], X: [1650, 410], O: [1745, 315], SQ: [1555, 315],
          DR: [265, 315], DD: [185, 395], PS: [150, 745], PW: [514, 34],
        };
        // a button press: it sinks a little, holds and glows, and comes back up: done within 0.3 s,
        // so two presses of one button 0.45 s or more apart never overlap (the owner, 14:15Z: "the
        // pressing is a little too fast, slow it down a tad": v2's took 0.2 s)
        function press(t, id, sink = 0.9) {
          const el = "#" + id;
          if (id === "btn-l" || id === "btn-r" || id === "btn-power") {
            tl.fromTo(el, { y: 0 }, { y: 10, duration: 0.08, ease: "power1.out", immediateRender: false }, t);
            tl.fromTo(el, { y: 10 }, { y: 0, duration: 0.13, ease: "power1.out", immediateRender: false }, t + 0.16);
          } else {
            tl.fromTo(el, { scale: 1 }, { scale: sink, transformOrigin: "50% 50%", duration: 0.08, ease: "power1.out", immediateRender: false }, t);
            tl.fromTo(el, { scale: sink }, { scale: 1, transformOrigin: "50% 50%", duration: 0.13, ease: "power1.out", immediateRender: false }, t + 0.16);
          }
          fade(el + " .glow", t, 0, 0.9, 0.06);
          fade(el + " .glow", t + 0.16, 0.9, 0, 0.14);
        }
        // fingertips: a finger comes in along its own axis, presses (its pad flattens and its
        // shadow tightens), and goes out the way it came. rot: the finger's angle (0: pointing up).
        const F = {};
        const back = (rot, d) => [-Math.sin((rot * Math.PI) / 180) * d, Math.cos((rot * Math.PI) / 180) * d];
        function fingerIn(id, t, tip, rot, dur = 0.34, d = 260) {
          const [bx, by] = back(rot, d);
          tl.fromTo("#" + id, { x: tip[0] + bx, y: tip[1] + by, z: 150, rotation: rot, opacity: 0, scale: 1 },
            { x: tip[0], y: tip[1], z: 80, opacity: 1, duration: dur, ease: "power2.out", immediateRender: false }, t);
          F[id] = { tip, rot };
        }
        function fingerMove(id, t, tip, dur = 0.22) {
          const f = F[id];
          tl.fromTo("#" + id, { x: f.tip[0], y: f.tip[1] }, { x: tip[0], y: tip[1], duration: dur, ease: "power2.inOut", immediateRender: false }, t);
          f.tip = tip;
        }
        function fingerPress(id, t, hold = 0.1) {
          const el = "#" + id;
          tl.fromTo(el, { z: 80, scale: 1 }, { z: 20, scale: 0.955, duration: 0.09, ease: "power2.out", immediateRender: false }, t - 0.07);
          tl.fromTo(el, { z: 20, scale: 0.955 }, { z: 80, scale: 1, duration: 0.18, ease: "power2.out", immediateRender: false }, t + 0.04 + hold);
          tl.fromTo(el + " .f-shadow", { x: 18, y: 30, opacity: 0.34 }, { x: 5, y: 9, opacity: 0.55, duration: 0.09, immediateRender: false }, t - 0.07);
          tl.fromTo(el + " .f-shadow", { x: 5, y: 9, opacity: 0.55 }, { x: 18, y: 30, opacity: 0.34, duration: 0.18, immediateRender: false }, t + 0.04 + hold);
        }
        function fingerOut(id, t, dur = 0.34, d = 260) {
          const f = F[id], [bx, by] = back(f.rot, d);
          tl.fromTo("#" + id, { x: f.tip[0], y: f.tip[1], z: 80, opacity: 1 },
            { x: f.tip[0] + bx, y: f.tip[1] + by, z: 150, opacity: 0, duration: dur, ease: "power2.in", immediateRender: false }, t);
        }
        // the whole gesture: in, press at t, out
        function poke(id, t, tip, rot, pre = 0.4, post = 0.26) {
          fingerIn(id, t - pre, tip, rot, pre - 0.04);
          fingerPress(id, t);
          fingerOut(id, t + post);
        }
        // one finger pressing several buttons in a row: in before the first, a move between
        // presses, out after the last. presses: [[t, tip], ...]
        function pokeSeq(id, rot, presses, pre = 0.4, post = 0.26, outDur = 0.34) {
          fingerIn(id, presses[0][0] - pre, presses[0][1], rot, pre - 0.04);
          presses.forEach(([t, tip], i) => {
            if (i > 0) fingerMove(id, t - 0.27, tip, 0.18);
            fingerPress(id, t);
          });
          fingerOut(id, presses[presses.length - 1][0] + post, outDur);
        }
        // a tap on the touchscreen at screen pixel (sx, sy): the finger, and the touch's ripple
        const SCR = (sx, sy) => [357 + 1.1520833 * sx, 149 + 1.1520833 * sy];
        function ripple(t, sx, sy) {
          tl.fromTo("#ripple", { x: sx, y: sy, scale: 0.4, opacity: 0.95 }, { scale: 1.7, opacity: 0, duration: 0.38, ease: "power2.out", immediateRender: false }, t);
        }
        function tap(t, sx, sy, pre = 0.36) {
          poke("f-tap", t, SCR(sx, sy), -22, pre, 0.2);
          ripple(t, sx, sy);
        }
        function tapSeq(taps, pre = 0.36, post = 0.2, outDur = 0.34) {
          pokeSeq("f-tap", -22, taps.map(([t, sx, sy]) => [t, SCR(sx, sy)]), pre, post, outDur);
          taps.forEach(([t, sx, sy]) => ripple(t, sx, sy));
        }

        // ------------------------------------------------------------ captions
        function caption(id, tin, tout) {
          const el = document.getElementById(id);
          const words = el.querySelectorAll("h1 .w");
          on(el, tin);
          // the words rise in quickly (a 14-word line is whole after 0.77 s), so it is up long enough to read
          tl.fromTo(words, { opacity: 0, y: 34 }, { opacity: 1, y: 0, duration: 0.38, ease: "power3.out", stagger: 0.03, immediateRender: false }, tin);
          tl.fromTo(el, { opacity: 1, y: 0 }, { opacity: 0, y: -16, duration: 0.28, ease: "power2.in", immediateRender: false }, tout);
        }

        // ------------------------------------------------------------ layout per frame shape
        const place = (sel, x, y) => gsap.set(sel, { left: x, top: y });
        if (P) {
          place("#deck", -165, 360);
          place("#anki", -800, 1320);
          place("#laptop", -860, -1880);
          gsap.set("#pic-ring", { left: 928, top: 109 });
        }
        camSet(CAM.macro0);

        // ------------------------------------------------------------ always moving
        // the backdrop drifts, the camera breathes (a hand-held sway), the game's art lives
        tl.fromTo("#orb-a", { x: 0, y: 0 }, { x: 260, y: -140, duration: T.total, ease: "sine.inOut", immediateRender: false }, 0);
        tl.fromTo("#orb-b", { x: 0, y: 0 }, { x: -300, y: 180, duration: T.total, ease: "sine.inOut", immediateRender: false }, 0);
        tl.fromTo("#orb-c", { x: 0, y: 0 }, { x: -220, y: -160, duration: T.total, ease: "sine.inOut", immediateRender: false }, 0);
        tl.fromTo("#bg-dots", { x: 0, y: 0 }, { x: -120, y: -60, duration: T.total, ease: "none", immediateRender: false }, 0);
        tl.fromTo("#cam", { x: -7, y: 4, rotation: -0.25 }, { x: 7, y: -4, rotation: 0.25, duration: 3.1, ease: "sine.inOut", yoyo: true, repeat: Math.ceil(T.total / 3.1), immediateRender: false }, 0);
        tl.fromTo("#sprite", { scaleY: 1, scaleX: 1 }, { scaleY: 1.01, scaleX: 0.997, duration: 1.5, ease: "sine.inOut", yoyo: true, repeat: Math.ceil(T.total / 1.5), immediateRender: false }, 0);
        // petals over the riverside (scene s1)
        const petals = document.getElementById("petals");
        for (let i = 0; i < 16; i++) {
          const p = document.createElement("i");
          p.className = "petal";
          petals.appendChild(p);
          const x0 = 300 + rnd() * 760, y0 = -40 - rnd() * 60, dur = 3.4 + rnd() * 2.2;
          const dx = -260 - rnd() * 320, dy = 620 + rnd() * 80, r0 = rnd() * 360;
          const ph = rnd(), sc = 0.7 + rnd() * 0.6, r1 = r0 + 300 + rnd() * 240;
          const reps = Math.ceil(T.total / dur) + 1;
          tl.fromTo(p, { x: x0 + dx * ph, y: y0 + dy * ph, rotation: r0 + (r1 - r0) * ph, scale: sc },
            { x: x0 + dx, y: y0 + dy, rotation: r1, duration: dur * (1 - ph), ease: "none", immediateRender: false }, 0);
          tl.fromTo(p, { x: x0, y: y0, rotation: r0 }, { x: x0 + dx, y: y0 + dy, rotation: r1, duration: dur, ease: "none", repeat: reps, immediateRender: false }, dur * (1 - ph));
        }
        // the stars over the hill (scene s3) twinkle
        const sparks = document.getElementById("sparkles");
        for (let i = 0; i < 12; i++) {
          const s = document.createElement("i");
          s.className = "spark";
          s.style.left = 40 + rnd() * 880 + "px";
          s.style.top = 20 + rnd() * 250 + "px";
          sparks.appendChild(s);
          const d = 0.6 + rnd() * 0.9;
          tl.fromTo(s, { opacity: 0, scale: 0.6 }, { opacity: 0.9, scale: 1.3, duration: d, ease: "sine.inOut", yoyo: true, repeat: Math.ceil(T.total / d), immediateRender: false }, rnd() * 1.5);
        }
        // the screen's light on the backdrop follows what the console shows
        const GLOWS = { warm: "rgba(255,170,110,0.5)", violet: "rgba(130,110,255,0.42)", blue: "rgba(100,135,240,0.4)",
          teal: "rgba(91,220,207,0.36)", cream: "rgba(255,236,200,0.34)", green: "rgba(120,255,120,0.16)" };
        const gl = document.getElementById("glow");
        for (const [k, c] of Object.entries(GLOWS)) {
          const g = document.createElement("i");
          g.className = "glow-l";
          g.id = "glow-" + k;
          g.style.background = `radial-gradient(closest-side, ${c}, rgba(0,0,0,0) 100%)`;
          g.style.opacity = k === "warm" ? 1 : 0;
          gl.appendChild(g);
        }
        let glowNow = "warm";
        function glow(t, key, dur = 0.6) {
          if (key === glowNow) return;
          fade("#glow-" + key, t, 0, 1, dur, "power1.inOut");
          fade("#glow-" + glowNow, t, 1, 0, dur, "power1.inOut");
          glowNow = key;
        }
        // the "next" arrow bobs while it waits
        tl.fromTo("#vn-next", { y: 0 }, { y: 4, duration: 0.4, ease: "sine.inOut", yoyo: true, repeat: Math.ceil(T.total / 0.4), immediateRender: false }, 0);

        // ============================================================ S1: someone is playing
        tl.fromTo("#fade", { opacity: 1 }, { opacity: 0, duration: 0.6, ease: "power1.out", immediateRender: false }, 0);
        caption("cap1", K.cap1.in, K.cap1.out);
        type("#vn-m1", C.typeA);
        on("#vn-next", C.typeA + 1.4, 0.15);
        camTo(0.05, "macro", 0.9, "sine.inOut"); // a slow push on the text box as it types
        camTo(1.0, "hero", 2.3, "power2.inOut"); // pull back: a white Vita
        // × shows the next line: the heroine smiles
        poke("t-r", C.pressX1, B.X, -32, 0.42, 0.26);
        press(C.pressX1, "btn-cross");
        off("#vn-next", C.pressX1 + 0.05);
        off("#vn-m1", C.pressX1 + 0.06);
        swap("#spr-surprised", "#spr-smile", C.pressX1 + 0.06, 0.25);
        type("#vn-m2", C.typeB);
        on("#vn-next", C.typeB + 0.8, 0.12);
        camTo(3.3, "heroB", 1.2, "sine.inOut");

        // ============================================================ S2: L + R, a lookup, offline
        caption("cap2", K.cap2.in, K.cap2.out);
        camTo(4.5, "shoulders", 0.55, "power3.inOut");
        fingerIn("f-l", C.lr1 - 0.42, B.L, 180 - 14, 0.38);
        fingerIn("f-r", C.lr1 - 0.42, B.R, 180 + 14, 0.38);
        fingerPress("f-l", C.lr1, 0.14);
        fingerPress("f-r", C.lr1, 0.14);
        press(C.lr1, "btn-l");
        press(C.lr1, "btn-r");
        fingerOut("f-l", C.lr1 + 0.38);
        fingerOut("f-r", C.lr1 + 0.38);
        off("#vn-next", C.ovOpen1);
        on("#scr-ov", C.ovOpen1, 0.12);
        tl.fromTo("#scr-ov", { y: 110 }, { y: 0, duration: 0.22, ease: "power3.out", immediateRender: false }, C.ovOpen1);
        on("#ov-a1", C.ovOpen1);
        camTo(C.ovOpen1 + 0.1, "screen", 0.9, "power3.inOut");
        camTo(C.ovOpen1 + 1.0, "screenIn", C.right1 + 0.35 - (C.ovOpen1 + 1.0), "sine.inOut"); // a slow push while it reads
        // Right: the next word (in flight: Looking up words…), then 一緒に
        poke("t-l", C.right1, B.DR, 32, 0.42, 0.28);
        press(C.right1, "dpad-right");
        swap("#ov-a1", "#ov-a2", C.right1 + 0.05, 0.02);
        swap("#ov-a2", "#ov-a3", C.right1 + 0.33, 0.02);
        camTo(C.right1 + 0.35, "popupZoom", 1.25, "power2.inOut"); // in on the word and its meaning
        camTo(C.right1 + 1.6, "screenB", T.s3 - 0.05 - (C.right1 + 1.6), "power2.inOut");
        glow(5.1, "blue", 0.4);

        // ============================================================ S3: × mines it, with its screenshot
        caption("cap3", K.cap3.in, K.cap3.out);
        camTo(T.s3, "mine", 0.75, "power3.inOut");
        fade("#deck", T.s3 + 0.3, 0, 1, 0.35);
        tl.fromTo("#deck", { x: 60 }, { x: 0, duration: 0.5, ease: "power3.out", immediateRender: false }, T.s3 + 0.3);
        poke("t-r", C.mine1, B.X, -32, 0.42, 0.3);
        press(C.mine1, "btn-cross");
        swap("#ov-a3", "#ov-a4", C.mine1 + 0.05, 0.02);
        // the game's screen is kept (the overlay is not in it): a flash where the shot is taken
        tl.fromTo("#scr-ov", { opacity: 1 }, { opacity: 0.25, duration: 0.05, immediateRender: false }, C.mine1 + 0.06);
        tl.fromTo("#shot-flash", { opacity: 0 }, { opacity: 0.55, duration: 0.05, immediateRender: false }, C.mine1 + 0.06);
        tl.fromTo("#shot-flash", { opacity: 0.55 }, { opacity: 0, duration: 0.3, immediateRender: false }, C.mine1 + 0.11);
        tl.fromTo("#scr-ov", { opacity: 0.25 }, { opacity: 1, duration: 0.25, immediateRender: false }, C.mine1 + 0.13);
        swap("#ov-a4", "#ov-a5", C.saved1, 0.02);
        // the card flies out of the screen into the queue
        function flyCard(sel, t0, land, n) {
          const dest = P ? { x: 0, y: 560 } : { x: 1325, y: -40 };
          tl.fromTo(sel, { x: 0, y: 22, z: 0, scale: 0.35, rotationY: 0, rotation: 0, opacity: 0 },
            { z: 260, scale: 0.9, opacity: 1, duration: 0.3, ease: "back.out(1.6)", immediateRender: false }, t0);
          tl.fromTo(sel, { x: 0, y: 22, z: 260, scale: 0.9, rotation: 0 },
            { x: dest.x, y: dest.y, z: 0, scale: P ? 0.75 : 1, rotation: n === 1 ? -4 : 3, duration: land - t0 - 0.3, ease: "power2.inOut", immediateRender: false }, t0 + 0.3);
          tl.fromTo("#deck", { scale: 1 }, { scale: 1.05, duration: 0.1, yoyo: true, repeat: 1, ease: "power1.inOut", immediateRender: false }, land);
          off("#dq-" + (n - 1), land, 0.08);
          tl.fromTo("#dq-" + n, { opacity: 0, y: 20 }, { opacity: 1, y: 0, duration: 0.25, ease: "back.out(2)", immediateRender: false }, land);
        }
        flyCard("#fly1", C.saved1 - 0.1, C.land1, 1);
        // Right: 見に行く; × mines it too
        poke("t-l", C.right2, B.DR, 32, 0.4, 0.28);
        press(C.right2, "dpad-right");
        swap("#ov-a5", "#ov-a6", C.right2 + 0.28, 0.02);
        poke("t-r", C.mine2, B.X, -32, 0.42, 0.3);
        press(C.mine2, "btn-cross");
        swap("#ov-a6", "#ov-a7", C.mine2 + 0.05, 0.02);
        tl.fromTo("#shot-flash", { opacity: 0 }, { opacity: 0.5, duration: 0.05, immediateRender: false }, C.mine2 + 0.06);
        tl.fromTo("#shot-flash", { opacity: 0.5 }, { opacity: 0, duration: 0.3, immediateRender: false }, C.mine2 + 0.11);
        swap("#ov-a7", "#ov-a8", C.saved2, 0.02);
        flyCard("#fly2", C.saved2 - 0.1, C.land2, 2);
        camTo(T.s3 + 0.75, "mineB", C.whipA - 0.05 - (T.s3 + 0.75), "sine.inOut");

        // ============================================================ S4: with screenshots
        caption("cap4", K.cap4.in, K.cap4.out);
        whip(C.whipA, "anki", 0.6);
        grow("#anki", C.cardOpen - 0.05, 0.86, 0.35);
        tl.fromTo("#anki", { rotationY: -24 }, { rotationY: 0, duration: 0.45, ease: "power3.out", immediateRender: false }, C.cardOpen - 0.05);
        // the card's front (the word), flipped to its back: the game's screen is on it
        const flip = C.cardOpen + 0.5;
        tl.fromTo("#anki", { rotationY: 0 }, { rotationY: 90, duration: 0.22, ease: "power2.in", immediateRender: false }, flip);
        swap("#anki-front", "#anki-card", flip + 0.22, 0.01);
        tl.fromTo("#anki", { rotationY: -90 }, { rotationY: 0, duration: 0.3, ease: "power2.out", immediateRender: false }, flip + 0.23);
        camTo(flip + 0.35, "ankiPic", 1.0, "power2.inOut");
        tl.fromTo("#pic-ring", { opacity: 0, scale: 1.2 }, { opacity: 1, scale: 1, duration: 0.3, ease: "back.out(2)", immediateRender: false }, flip + 0.55);
        tl.fromTo("#pic-ring", { opacity: 1 }, { opacity: 0.45, duration: 0.42, yoyo: true, repeat: 3, ease: "sine.inOut", immediateRender: false }, flip + 0.85);
        camTo(flip + 1.4, "ankiAll", C.whipB - (flip + 1.4), "power2.inOut");
        off("#pic-ring", C.whipB - 0.3, 0.2);
        fade("#deck, #fly1, #fly2", C.whipA + 0.55, 1, 0, 0.3);
        glow(C.whipA, "cream", 0.5);

        // ============================================================ S5: it only reads text when it changes
        caption("cap5", K.cap5.in, K.cap5.out);
        tl.fromTo("#cap5-p", { opacity: 0, y: 20 }, { opacity: 1, y: 0, duration: 0.4, ease: "power3.out", immediateRender: false }, K.cap5.pIn);
        whip(C.whipB, "read", 0.6);
        fade("#anki", C.whipB + 0.45, 1, 0, 0.2);
        camTo(C.whipB + 0.6, "readB", C.appIn - C.whipB - 0.6, "sine.inOut");
        pokeSeq("t-r", -34, [[C.pressO1, B.O], [C.pressX2, B.X]], 0.42, 0.26);
        press(C.pressO1, "btn-circle");
        off("#scr-ov", C.pressO1 + 0.06, 0.05);
        press(C.pressX2, "btn-cross");
        // the story goes on: a new scene, a new line (the overlay reads it once it has settled)
        off("#vn-m2", C.pressX2 + 0.05);
        swap("#bg-s1", "#bg-s2", C.pressX2 + 0.07, 0.4);
        fade("#petals", C.pressX2 + 0.07, 1, 0, 0.3);
        swap("#spr-smile", "#spr-happy", C.pressX2 + 0.07, 0.35);
        type("#vn-m3", C.typeC, 0.05);
        on("#vn-next", C.typeC + 0.7, 0.12);
        glow(C.pressX2, "violet", 0.6);
        // the app's OCR & lookup page, scrolled to Read text (the model choice is not in the video)
        tl.fromTo("#scr-app", { opacity: 0, x: 120 }, { opacity: 1, x: 0, duration: 0.3, ease: "power3.out", immediateRender: false }, C.appIn);
        on("#app-read-a", C.appIn);
        tl.fromTo("#scr-vn", { x: 0 }, { x: -120, duration: 0.3, ease: "power3.out", immediateRender: false }, C.appIn);
        camTo(C.appIn, "readApp", 0.6, "power3.inOut");
        camTo(C.appIn + 0.6, "readAppB", T.s6 - 0.1 - (C.appIn + 0.6), "sine.inOut");
        poke("t-l", C.right3, B.DR, 32, 0.42, 0.3);
        press(C.right3, "dpad-right");
        swap("#app-read-a", "#app-read-b", C.right3 + 0.06, 0.03);
        glow(C.appIn, "teal", 0.5);

        // ============================================================ S6: turn it off for a game (in the app)
        caption("cap6", K.cap6.in, K.cap6.out);
        // from Read text, the whole page comes into view; a tap on its Games tab (settings/main.c: TAB_X(0) 24,
        // TAB_W 220, TAB_Y 52, TAB_H 48; a tab tap changes the page as Select does)
        camTo(T.s6 - 0.1, "screen3", 0.6, "power3.inOut");
        tap(C.tapGames, 134, 76, 0.38);
        swap("#app-read-b", "#games-1", C.tapGames + 0.05, 0.02);
        // the Games page (Vita3K captures of the app): Down, Down to Ember Garden, × turns it OFF
        fingerIn("t-l", C.down1 - 0.4, B.DD, 30, 0.34);
        fingerPress("t-l", C.down1);
        press(C.down1, "dpad-down");
        swap("#games-1", "#games-2", C.down1 + 0.05, 0.02);
        fingerPress("t-l", C.down2);
        press(C.down2, "dpad-down");
        swap("#games-2", "#games-3", C.down2 + 0.05, 0.02);
        fingerOut("t-l", C.down2 + 0.3);
        poke("t-r", C.toggle, B.X, -32, 0.42, 0.28);
        press(C.toggle, "btn-cross");
        swap("#games-3", "#games-4", C.toggle + 0.05, 0.02);
        camTo(C.tapGames + 0.3, "app8B", C.toggle - (C.tapGames + 0.3), "sine.inOut");
        camTo(C.toggle + 0.05, "offZoom", 0.9, "power2.inOut"); // in on OFF and the app's line
        camTo(C.toggle + 0.95, "offZoomB", T.s7 - 0.15 - (C.toggle + 0.95), "sine.inOut"); // a slow drift as it reads

        // ============================================================ S7: easy install (the one-VPK install), last
        caption("cap7", K.cap7.in, K.cap7.out);
        // the steps, one at a time under "Easy install", each as its action starts
        function stepIn(id, t) {
          tl.fromTo(id, { opacity: 0, y: 30 }, { opacity: 1, y: 0, duration: 0.3, ease: "power3.out", immediateRender: false }, t);
        }
        function stepOut(id, t) {
          tl.fromTo(id, { opacity: 1, y: 0 }, { opacity: 0, y: -24, duration: 0.2, ease: "power2.in", immediateRender: false }, t);
        }
        camTo(T.s7 - 0.1, "installWide", 0.6, "power3.inOut");
        swap("#scr-app", "#scr-shell", T.s7 - 0.05, 0.2);
        on("#vs-list-e", T.s7 - 0.05);
        off("#scr-vn", T.s7 + 0.2);
        tl.fromTo("#laptop", { x: -400, opacity: 0 }, { x: 0, opacity: 1, duration: 0.6, ease: "power3.out", immediateRender: false }, C.laptopIn);
        stepIn("#step1", C.laptopIn + 0.3);
        camTo(C.laptopIn + 0.3, "laptop", 0.5, "power2.inOut");
        // the mouse goes to the VPK and clicks it; it downloads
        const vpkY = 548 + 70 * GH_VPK_ROW; // the rows: 58 px and a 12 px gap (src/promo.css .gh-asset)
        tl.fromTo("#mouse", { x: 1300, y: 960, opacity: 0 }, { x: 610, y: vpkY, opacity: 1, duration: 0.42, ease: "power2.inOut", immediateRender: false }, C.clickVpk - 0.46);
        tl.fromTo("#mouse", { scale: 1 }, { scale: 0.85, duration: 0.07, yoyo: true, repeat: 1, immediateRender: false }, C.clickVpk - 0.02);
        tl.fromTo("#gh-vpk", { backgroundColor: "rgba(56,139,253,0)" }, { backgroundColor: "rgba(56,139,253,0.22)", duration: 0.12, immediateRender: false }, C.clickVpk);
        tl.fromTo("#dl-chip", { opacity: 0, y: -20 }, { opacity: 1, y: 0, duration: 0.25, ease: "power3.out", immediateRender: false }, C.clickVpk + 0.05);
        tl.fromTo("#dl-fill", { scaleX: 0 }, { scaleX: 1, duration: C.fileFly - C.clickVpk - 0.1, ease: "power1.inOut", immediateRender: false }, C.clickVpk + 0.1);
        // the file goes to the Vita
        camTo(C.fileFly - 0.15, "installWide", 0.7, "power3.inOut");
        const ffFrom = P ? { x: 540, y: -1737 } : { x: -2100, y: -415 };
        tl.fromTo("#fly-file", { x: ffFrom.x, y: ffFrom.y, scale: 0.6, opacity: 0, rotation: -10 },
          { x: 0, y: -10, scale: 2.2, opacity: 1, rotation: 0, duration: C.shellIn - C.fileFly, ease: "power2.inOut", immediateRender: false }, C.fileFly);
        tl.fromTo("#fly-file", { yPercent: 0 }, { yPercent: -110, duration: (C.shellIn - C.fileFly) / 2, yoyo: true, repeat: 1, ease: "sine.out", immediateRender: false }, C.fileFly);
        fade("#fly-file", C.shellIn + 0.1, 1, 0, 0.2);
        stepOut("#step1", C.shellIn - 0.1);
        stepIn("#step2", C.shellIn + 0.1);
        fade("#laptop", C.shellIn + 0.1, 1, 0, 0.3);
        // VitaShell: the VPK is there; × on it, Yes, the extended-permissions notice, Yes, Installing...
        swap("#vs-list-e", "#vs-list-0", C.shellIn, 0.02);
        tl.fromTo("#vs-list-0 .vs-row:nth-child(2)", { x: 30, opacity: 0 }, { x: 0, opacity: 1, duration: 0.25, ease: "power3.out", immediateRender: false }, C.shellIn);
        // VitaShell opens on "..": Down to the file
        poke("t-l", C.vsDown, B.DD, 30, 0.38, 0.24);
        press(C.vsDown, "dpad-down");
        swap("#vs-list-0", "#vs-list-1", C.vsDown + 0.05, 0.02);
        camTo(C.shellIn, "installDev", 0.6, "power3.inOut");
        camTo(C.shellIn + 0.6, "installDevB", C.powerHold1 - 0.45 - (C.shellIn + 0.6), "sine.inOut");
        glow(C.shellIn, "green", 0.4);
        fingerIn("t-r", C.vsX1 - 0.4, B.X, -32, 0.36);
        function vsOpen(sel, t) {
          on(sel, t);
          tl.fromTo(sel + " .vs-box", { scale: 0 }, { scale: 1, duration: 0.14, ease: "power2.out", immediateRender: false }, t);
          on(sel + " .vs-in", t + 0.14);
        }
        function vsClose(sel, t) {
          off(sel + " .vs-in", t);
          tl.fromTo(sel + " .vs-box", { scale: 1 }, { scale: 0, duration: 0.1, ease: "power2.in", immediateRender: false }, t);
          off(sel, t + 0.1);
        }
        fingerPress("t-r", C.vsX1);
        press(C.vsX1, "btn-cross");
        vsOpen("#vs-ask", C.vsX1 + 0.06);
        fingerPress("t-r", C.vsX2);
        press(C.vsX2, "btn-cross");
        vsClose("#vs-ask", C.vsX2 + 0.06);
        vsOpen("#vs-warn", C.vsX2 + 0.19);
        fingerPress("t-r", C.vsX3);
        press(C.vsX3, "btn-cross");
        vsClose("#vs-warn", C.vsX3 + 0.06);
        vsOpen("#vs-prog", C.vsX3 + 0.18);
        on("#vs-pct0", C.vsX3 + 0.18);
        tl.fromTo("#vs-pfill", { scaleX: 0 }, { scaleX: 0.5, duration: 0.01, immediateRender: false }, C.vsX3 + 0.4);
        swap("#vs-pct0", "#vs-pct50", C.vsX3 + 0.4, 0.01);
        tl.fromTo("#vs-pfill", { scaleX: 0.5 }, { scaleX: 1, duration: 0.01, immediateRender: false }, C.vsDone - 0.1);
        swap("#vs-pct50", "#vs-pct100", C.vsDone - 0.1, 0.01);
        vsClose("#vs-prog", C.vsDone);
        fingerOut("t-r", C.vsX3 + 0.3);
        // a restart, as the console's guide says: hold the power button (two seconds: drawn shorter, a ring fills
        // as it is held) until a screen appears, select [Power Off] on it, and the screen goes dark; the film then
        // cuts to the Vita on again. The user restarts after VitaShell's install (test.4, #194 section 3: the app's
        // folder is readable only after a restart) and after the app's Install (its result asks for it; the app
        // never restarts the Vita).
        function restart(hold, screen, tapAt, dark, from) {
          camTo(hold - 0.45, "power", 0.45, "power2.inOut");
          camTo(hold, "powerB", tapAt - 0.25 - hold, "sine.inOut"); // in as it is held
          const held = screen + 0.1 - hold; // the press, until the screen has appeared
          fingerIn("f-l", hold - 0.42, B.PW, 180 - 8, 0.36);
          fingerPress("f-l", hold, held - 0.04);
          tl.fromTo("#btn-power", { y: 0 }, { y: 8, duration: 0.08, ease: "power1.out", immediateRender: false }, hold);
          tl.fromTo("#btn-power", { y: 8 }, { y: 0, duration: 0.13, ease: "power1.out", immediateRender: false }, hold + held);
          fade("#btn-power .glow", hold, 0, 0.9, 0.06);
          fade("#btn-power .glow", hold + held, 0.9, 0, 0.14);
          on("#hold-ring", hold, 0.08);
          tl.fromTo("#hold-ring circle.fill", { strokeDashoffset: 377 }, { strokeDashoffset: 0, duration: screen - hold, ease: "none", immediateRender: false }, hold);
          off("#hold-ring", hold + held, 0.12);
          fingerOut("f-l", hold + held + 0.18);
          // the screen that appears (simplified): [Power Off], tapped
          tl.set("#po-btn", { backgroundColor: "rgba(255,255,255,0.16)" }, screen);
          tl.fromTo("#scr-poweroff", { opacity: 0 }, { opacity: 1, duration: 0.15, immediateRender: false }, screen);
          tl.fromTo("#scr-poweroff .po-box", { scale: 0.9 }, { scale: 1, duration: 0.2, ease: "power2.out", immediateRender: false }, screen);
          tap(tapAt, 480, 272, 0.38);
          tl.fromTo("#po-btn", { backgroundColor: "rgba(255,255,255,0.16)" }, { backgroundColor: "rgba(91,220,207,0.5)", duration: 0.06, immediateRender: false }, tapAt);
          // off: the screen goes dark
          swap(from, "#scr-black", dark, 0.25);
          off("#scr-poweroff", dark + 0.05, 0.2);
        }
        restart(C.powerHold1, C.powerScreen1, C.tapPowerOff1, C.dark1, "#scr-shell");
        // on again: at the whip's fastest (boot1) the home screen, with the app's bubble, replaces the dark one in
        // one frame
        tl.set("#bub-app", { opacity: 1, scale: 1 }, C.boot1);
        tl.fromTo("#scr-home", { opacity: 0, x: 0, scale: 1 }, { opacity: 1, x: 0, scale: 1, duration: 0.01, immediateRender: false }, C.boot1);
        off("#scr-black", C.boot1);
        glow(C.boot1, "blue", 0.5);
        whip(C.boot1 - 0.3, "home", 0.6);
        stepOut("#step2", C.boot1 - 0.15);
        stepIn("#step3", C.boot1 + 0.05);
        // open the app: its LiveArea, Start; it opens on its Install page
        tapSeq([[C.tapBubbleApp, 639, 350], [C.tapStartApp, 480, 293]], 0.36, 0.2, 0.22);
        tl.fromTo("#scr-la-app", { opacity: 0, scale: 0.25, transformOrigin: "639px 350px" }, { opacity: 1, scale: 1, duration: 0.25, ease: "power2.out", immediateRender: false }, C.tapBubbleApp + 0.06);
        off("#scr-home", C.tapBubbleApp + 0.3);
        grow("#scr-app", C.tapStartApp + 0.08, 0.94, 0.2);
        on("#inst-1", C.tapStartApp + 0.08);
        off("#games-4", C.tapStartApp + 0.08);
        off("#scr-la-app", C.tapStartApp + 0.3);
        poke("t-r", C.installX, B.X, -32, 0.4, 0.26);
        press(C.installX, "btn-cross");
        // the install's run (the app's own frames: its step, a bar with the bytes), then its result
        const run = (C.installDone - C.installX - 0.1) / 4;
        swap("#inst-1", "#inst-2", C.installX + 0.05, 0.02);
        swap("#inst-2", "#inst-3", C.installX + 0.05 + run, 0.02);
        swap("#inst-3", "#inst-4", C.installX + 0.05 + 2 * run, 0.02);
        swap("#inst-4", "#inst-5", C.installX + 0.05 + 3 * run, 0.02);
        swap("#inst-5", "#inst-6", C.installDone, 0.02);
        camTo(C.tapStartApp, "installDev", 0.8, "power2.inOut");
        camTo(C.tapStartApp + 0.8, "installDevB", C.powerHold - 0.45 - (C.tapStartApp + 0.8), "sine.inOut");
        // the restart its result asks for; the film then cuts to the Vita on again, in the game
        restart(C.powerHold, C.powerScreen, C.tapPowerOff, C.dark, "#scr-app");

        // the Vita on again (a cut at the whip's fastest), in the game: L + R, and the overlay opens (no second
        // lookup: the owner, 06:46Z). Right after the install card mining is off: no queue line, no × queue.
        tl.set("#vn-m1", { opacity: 0 }, C.boot);
        tl.set("#vn-m2", { opacity: 0 }, C.boot);
        tl.set("#vn-m3", { opacity: 0 }, C.boot);
        tl.set("#vn-next", { opacity: 0 }, C.boot);
        tl.set("#bg-s1", { opacity: 0 }, C.boot);
        tl.set("#bg-s2", { opacity: 0 }, C.boot);
        tl.set("#bg-s3", { opacity: 1 }, C.boot);
        tl.set("#scr-vn", { opacity: 1, x: 0 }, C.boot);
        tl.set("#scr-ov", { opacity: 0 }, C.boot);
        tl.set(".ov-state", { opacity: 0 }, C.boot);
        off("#scr-black", C.boot); // VitaShell, the home screen and the app are off already
        glow(C.boot, "violet", 0.5);
        whip(C.boot - 0.3, "shoulders2", 0.6);
        stepOut("#step3", C.dark);
        tl.fromTo("#step-done", { opacity: 0, scale: 0.6 }, { opacity: 1, scale: 1, duration: 0.35, ease: "back.out(2.2)", immediateRender: false }, C.dark + 0.2);
        tl.set("#vn-m4 .c", { opacity: 1 }, C.boot); // the game's line, already shown
        fingerIn("f-l", C.lrEnd - 0.42, B.L, 180 - 14, 0.38);
        fingerIn("f-r", C.lrEnd - 0.42, B.R, 180 + 14, 0.38);
        fingerPress("f-l", C.lrEnd, 0.14);
        fingerPress("f-r", C.lrEnd, 0.14);
        press(C.lrEnd, "btn-l");
        press(C.lrEnd, "btn-r");
        fingerOut("f-l", C.lrEnd + 0.38);
        fingerOut("f-r", C.lrEnd + 0.38);
        on("#scr-ov", C.ovEnd, 0.12);
        tl.fromTo("#scr-ov", { y: 110 }, { y: 0, duration: 0.22, ease: "power3.out", immediateRender: false }, C.ovEnd);
        on("#ov-b1", C.ovEnd);
        camTo(C.lrEnd + 0.15, "shoulders3", T.end - 0.4 - (C.lrEnd + 0.15), "sine.out"); // a little back, as it opens

        // ============================================================ end card
        camTo(T.end - 0.4, "out", 0.7, "power2.in");
        tl.fromTo("#viewport", { opacity: 1 }, { opacity: 0, duration: 0.4, ease: "power2.in", immediateRender: false }, T.end - 0.15);
        on("#endcard", T.end + 0.1);
        tl.fromTo("#endcard img", { scale: 0.4, opacity: 0, rotation: -40 }, { scale: 1, opacity: 1, rotation: 0, duration: 0.6, ease: "back.out(1.7)", immediateRender: false }, T.end + 0.1);
        tl.fromTo("#endcard h2, #endcard p", { opacity: 0, y: 30 }, { opacity: 1, y: 0, duration: 0.5, stagger: 0.1, ease: "power3.out", immediateRender: false }, T.end + 0.3);
        tl.fromTo("#endcard img", { y: 0 }, { y: -10, duration: 0.9, yoyo: true, repeat: 1, ease: "sine.inOut", immediateRender: false }, T.end + 0.7);
        tl.fromTo("#endcard", { scale: 0.96 }, { scale: 1.04, duration: T.total - T.end - 0.1, ease: "none", immediateRender: false }, T.end + 0.1);

        window.__timelines = window.__timelines || {};
        window.__timelines["main"] = tl;
      })();
