      // The promo's one paused GSAP timeline (HyperFrames seeks it frame by frame).
      // Built synchronously; every state change is an explicit fromTo/set at a
      // fixed time, so a cold seek to any frame renders the same picture.
      (function () {
        const root = document.getElementById("root");
        const P = root.dataset.layout === "portrait";
        const tl = gsap.timeline({ paused: true });
        const q = (s) => document.querySelector(s);

        // Scene starts in seconds (tools/build.mjs places the sound effects from
        // the same table). The music is 120 BPM: a bar is 2 s.
        const T = /*{{TIMES}}*/ null;

        // ------------------------------------------------------------ helpers
        // Opacity from -> to, starting at t; never rendered before t.
        function fade(sel, t, from, to, dur = 0.2, ease = "power1.out") {
          tl.fromTo(sel, { opacity: from }, { opacity: to, duration: dur, ease, immediateRender: false }, t);
        }
        // A screen layer replaces another (a short crossfade).
        function swap(from, to, t, dur = 0.16) {
          if (to) fade(to, t, 0, 1, dur);
          if (from) fade(from, t + dur * 0.5, 1, 0, dur);
        }
        // The device's camera: its centre and scale on the frame.
        let cam = null;
        function camTo(t, s, dur = 0.8, ease = "power3.inOut") {
          tl.fromTo(
            "#stage",
            { x: cam.x, y: cam.y, scale: cam.s },
            { x: s.x, y: s.y, scale: s.s, duration: dur, ease, immediateRender: false },
            t,
          );
          cam = s;
        }
        // A button press on the device: it sinks a little and glows.
        function press(t, id, sink = 0.9) {
          const el = "#" + id;
          if (id === "btn-l" || id === "btn-r") {
            tl.fromTo(el, { y: 0 }, { y: 9, duration: 0.07, ease: "power1.out", immediateRender: false }, t);
            tl.fromTo(el, { y: 9 }, { y: 0, duration: 0.16, ease: "power1.out", immediateRender: false }, t + 0.16);
          } else {
            tl.fromTo(el, { scale: 1 }, { scale: sink, transformOrigin: "50% 50%", duration: 0.07, ease: "power1.out", immediateRender: false }, t);
            tl.fromTo(el, { scale: sink }, { scale: 1, transformOrigin: "50% 50%", duration: 0.16, ease: "power1.out", immediateRender: false }, t + 0.16);
          }
          fade(el + " .glow", t, 0, 0.9, 0.06);
          fade(el + " .glow", t + 0.2, 0.9, 0, 0.35);
        }
        // A fingertip taps the screen at (x, y) (screen pixels) at time t.
        function tap(t, x, y, fromX = x + 70, fromY = y + 90) {
          tl.fromTo("#finger", { x: fromX, y: fromY, opacity: 0, scale: 1.15 }, { x, y, opacity: 1, scale: 1, duration: 0.3, ease: "power2.out", immediateRender: false }, t - 0.32);
          tl.fromTo("#finger", { scale: 1 }, { scale: 0.82, duration: 0.07, ease: "power1.out", immediateRender: false }, t);
          tl.fromTo("#finger", { scale: 0.82 }, { scale: 1, duration: 0.14, ease: "power1.out", immediateRender: false }, t + 0.08);
          tl.fromTo("#ripple", { scale: 1, opacity: 0.9 }, { scale: 2.4, opacity: 0, duration: 0.45, ease: "power2.out", immediateRender: false }, t);
          fade("#finger", t + 0.3, 1, 0, 0.18);
        }
        // Captions: words rise in one after another; the group fades out.
        function caption(id, tin, tout) {
          const el = q("#" + id);
          const words = el.querySelectorAll(".w, .lockup, p");
          fade(el, tin, 0, 1, 0.01);
          tl.fromTo(words, { opacity: 0, y: 26 }, { opacity: 1, y: 0, duration: 0.42, ease: "power3.out", stagger: 0.045, immediateRender: false }, tin);
          tl.fromTo(el, { opacity: 1, y: 0 }, { opacity: 0, y: -14, duration: 0.3, ease: "power2.in", immediateRender: false }, tout);
        }
        // A side card slides in and later out.
        function card(sel, tin, tout, dx = 0, dy = 40) {
          tl.fromTo(sel, { opacity: 0, x: dx, y: dy, scale: 0.96 }, { opacity: 1, x: 0, y: 0, scale: 1, duration: 0.6, ease: "power3.out", immediateRender: false }, tin);
          if (tout != null) tl.fromTo(sel, { opacity: 1, x: 0, y: 0 }, { opacity: 0, x: -dx * 0.5, y: -dy * 0.5, duration: 0.35, ease: "power2.in", immediateRender: false }, tout);
        }
        // A row of the side panels lights up (others dim).
        function light(sel, t, on) {
          tl.fromTo(sel, { opacity: on ? 0.35 : 1 }, { opacity: on ? 1 : 0.35, duration: 0.2, immediateRender: false }, t);
        }
        // Typewriter text: the characters of sel appear one by one.
        function type(sel, t, per) {
          tl.fromTo(sel + " .c", { opacity: 0 }, { opacity: 1, duration: 0.01, stagger: per, immediateRender: false }, t);
        }

        // ------------------------------------------------------------ layout
        // Camera states and the side panels' places for each frame shape.
        const CAM = P
          ? {
              enter: { x: 540, y: 1460, s: 0.42 },
              center: { x: 540, y: 1030, s: 0.57 },
              zoom: { x: 540, y: 1010, s: 0.74 },
              queue: { x: 540, y: 960, s: 0.57 },
              side: { x: 540, y: 850, s: 0.55 },
              settle: { x: 540, y: 960, s: 0.57 },
              install: { x: 540, y: 820, s: 0.5 },
              out: { x: 540, y: 1000, s: 0.4 },
            }
          : {
              enter: { x: 960, y: 980, s: 0.56 },
              center: { x: 960, y: 668, s: 0.76 },
              zoom: { x: 960, y: 690, s: 0.84 },
              queue: { x: 820, y: 680, s: 0.72 },
              side: { x: 590, y: 690, s: 0.58 },
              settle: { x: 700, y: 680, s: 0.66 },
              install: { x: 1345, y: 690, s: 0.58 },
              out: { x: 960, y: 640, s: 0.5 },
            };
        const place = (sel, x, y, s = 1) => gsap.set(sel, { left: x, top: y, transformOrigin: "50% 50%", scale: s });
        if (P) {
          place("#queue-chip", 300, 1360);
          place("#fly-card", 435, 900);
          place("#anki-window", 90, 1150, 1.0);
          place("#export", 190, 1260);
          place("#settle", 280, 1360);
          place("#terminal", 70, 1110, 1.0);
          place("#steps", 120, 1625);
        } else {
          place("#queue-chip", 1440, 560);
          place("#fly-card", 715, 560);
          place("#anki-window", 1160, 300, 0.97);
          place("#export", 1150, 410);
          place("#settle", 1330, 470);
          place("#terminal", 40, 300, 1.0);
          place("#steps", 60, 828);
        }
        cam = CAM.enter;
        gsap.set("#stage", { x: cam.x, y: cam.y, scale: cam.s });
        gsap.set("#device", { rotationX: 24, transformPerspective: 2400, opacity: 0 });

        // ------------------------------------------------------------ backdrop drift
        tl.fromTo("#blob-a", { x: 0, y: 0 }, { x: 220, y: -120, duration: T.total, ease: "sine.inOut", immediateRender: false }, 0);
        tl.fromTo("#blob-b", { x: 0, y: 0 }, { x: -260, y: 160, duration: T.total, ease: "sine.inOut", immediateRender: false }, 0);
        tl.fromTo("#blob-c", { x: 0, y: 0 }, { x: -200, y: -140, duration: T.total, ease: "sine.inOut", immediateRender: false }, 0);

        // ============================================================ S1: title
        tl.fromTo("#device", { rotationX: 24, opacity: 0 }, { rotationX: 0, opacity: 1, duration: 1.6, ease: "power3.out", immediateRender: false }, 0.05);
        camTo(0.05, CAM.center, 1.6, "power3.out");
        caption("cap1", 0.35, 3.65);
        type("#vn-text-1 .vn-line:nth-child(1)", 0.9, 0.06);
        type("#vn-text-1 .vn-line:nth-child(2)", 2.15, 0.06);
        fade("#vn-next", 3.2, 0, 1, 0.1);

        // ============================================================ S2: lookup, offline
        caption("cap2", T.s2 + 0.1, T.s3 - 0.4);
        press(T.s2 + 0.25, "btn-l");
        press(T.s2 + 0.25, "btn-r");
        fade("#vn-next", T.s2 + 0.3, 1, 0, 0.1);
        fade("#scr-overlay", T.s2 + 0.42, 0, 1, 0.12);
        fade("#ovb-kira", T.s2 + 0.42, 0, 1, 0.01);
        camTo(T.s2 + 0.3, CAM.zoom, 0.9);
        // the tap: 一緒に in the header's second line (x 209..290, line centre y 69)
        tap(T.s2 + 2.75, 252, 70);
        tl.set("#hl-kira", { color: "#ffffff" }, T.s2 + 2.8);
        tl.set("#hl-issho", { color: "#ffd24a" }, T.s2 + 2.8);
        fade("#ovb-kira", T.s2 + 2.8, 1, 0, 0.01);
        fade("#ovb-issho", T.s2 + 2.8, 0, 1, 0.01);

        // ============================================================ S3: mining
        caption("cap3", T.s3 + 0.1, T.s4 - 0.4);
        camTo(T.s3, CAM.queue, 0.8);
        card("#queue-chip", T.s3 + 0.5, T.s4 - 0.35, 40, 0);
        press(T.s3 + 1.0, "btn-cross");
        fade("#ovb-issho", T.s3 + 1.1, 1, 0, 0.01);
        fade("#ovb-queued", T.s3 + 1.1, 0, 1, 0.01);
        // a card flies from the screen to the queue
        tl.fromTo(
          "#fly-card",
          { opacity: 0, scale: 0.6, x: 0, y: 0 },
          { opacity: 1, scale: 1, duration: 0.25, ease: "back.out(2)", immediateRender: false },
          T.s3 + 1.15,
        );
        tl.fromTo(
          "#fly-card",
          { x: 0, y: 0, scale: 1, opacity: 1 },
          P
            ? { x: -110, y: 470, scale: 0.35, opacity: 0, duration: 0.8, ease: "power2.inOut", immediateRender: false }
            : { x: 770, y: 30, scale: 0.35, opacity: 0, duration: 0.8, ease: "power2.inOut", immediateRender: false },
          T.s3 + 1.55,
        );
        fade("#q-n2", T.s3 + 2.3, 1, 0, 0.12);
        fade("#q-n3", T.s3 + 2.3, 0, 1, 0.12);
        tl.fromTo("#queue-chip", { scale: 1 }, { scale: 1.06, duration: 0.12, yoyo: true, repeat: 1, ease: "power1.inOut", immediateRender: false }, T.s3 + 2.3);

        // ============================================================ S4: the Lapis card
        caption("cap4", T.s4 + 0.1, T.s4b - 0.35);
        fade("#scr-overlay", T.s4 + 0.1, 1, 0, 0.15);
        camTo(T.s4, CAM.side, 0.8);
        card("#anki-window", T.s4 + 0.45, T.s4b - 0.3, P ? 0 : 80, P ? 60 : 0);

        // ============================================================ S4b: export the .apkg
        caption("cap4b", T.s4b + 0.1, T.s5 - 0.35);
        swap("#scr-vn", "#app-anki-a", T.s4b);
        press(T.s4b + 0.55, "btn-cross");
        swap("#app-anki-a", "#app-anki-b", T.s4b + 0.65, 0.08);
        swap("#app-anki-b", "#app-anki-c", T.s4b + 1.5, 0.12);
        card("#export", T.s4b + 1.6, T.s5 - 0.25, P ? 0 : 60, P ? 50 : 0);
        tl.fromTo("#file-chip", { scale: 0.7 }, { scale: 1, duration: 0.45, ease: "back.out(1.8)", immediateRender: false }, T.s4b + 1.6);
        ["#tg-desktop", "#tg-phone", "#tg-tablet"].forEach((s, i) => {
          tl.fromTo(s, { opacity: 0, y: 20 }, { opacity: 1, y: 0, duration: 0.35, ease: "power2.out", immediateRender: false }, T.s4b + 2.2 + i * 0.15);
        });

        // ============================================================ S5: when it runs
        caption("cap5", T.s5 + 0.1, T.s6 - 0.35);
        swap("#app-anki-c", "#scr-vn", T.s5, 0.2);
        tl.set("#vn-text-1", { opacity: 0 }, T.s5 + 0.2);
        tl.set("#vn-text-2", { opacity: 1 }, T.s5 + 0.2);
        camTo(T.s5, CAM.settle, 0.8);
        fade("#region-note", T.s5 + 0.35, 0, 1, 0.2);
        card("#settle", T.s5 + 0.35, T.s6 - 0.3, P ? 0 : 50, P ? 40 : 0);
        light("#st-typing", T.s5 + 0.5, true);
        type("#vn-text-2 .vn-line", T.s5 + 0.6, 0.075);
        light("#st-typing", T.s5 + 1.95, false);
        light("#st-read", T.s5 + 2.05, true);
        tl.fromTo("#region-note", { boxShadow: "0 0 0 0 rgba(91,220,207,0)" }, { boxShadow: "0 0 0 10px rgba(91,220,207,0.0)", duration: 0.5, immediateRender: false }, T.s5 + 2.05);
        tl.fromTo("#region-note", { backgroundColor: "rgba(91,220,207,0.28)" }, { backgroundColor: "rgba(91,220,207,0)", duration: 0.6, ease: "power2.out", immediateRender: false }, T.s5 + 2.05);
        light("#st-read", T.s5 + 2.75, false);
        light("#st-idle", T.s5 + 2.85, true);
        fade("#region-note", T.s6 - 0.3, 1, 0, 0.25);

        // ============================================================ S6: install
        caption("cap6", T.s6 + 0.1, T.s7 - 0.35);
        camTo(T.s6, CAM.install, 0.8);
        swap("#scr-vn", "#scr-shell", T.s6 + 0.2);
        fade("#vs-ftp", T.s6 + 0.35, 0, 1, 0.15);
        card("#terminal", T.s6 + 0.4, T.s7 - 0.3, P ? 0 : -60, P ? 40 : 0);
        card("#steps", T.s6 + 0.5, T.s7 - 0.3, P ? 0 : -40, P ? 30 : 0);
        light("#step1", T.s6 + 0.6, true);
        type("#tm-body .cmd", T.s6 + 0.7, 0.016);
        tl.fromTo("#tm-body .out", { opacity: 0 }, { opacity: 1, duration: 0.01, stagger: 0.085, immediateRender: false }, T.s6 + 1.75);
        fade("#vs-ftp", T.s6 + 3.15, 1, 0, 0.12);
        light("#step1", T.s6 + 3.2, false);
        light("#step2", T.s6 + 3.2, true);
        fade("#vs-ask", T.s6 + 3.35, 0, 1, 0.12);
        press(T.s6 + 3.75, "btn-cross");
        fade("#vs-ask", T.s6 + 3.85, 1, 0, 0.08);
        fade("#vs-prog", T.s6 + 3.85, 0, 1, 0.08);
        tl.fromTo("#vs-bar-in", { scaleX: 0 }, { scaleX: 1, duration: 0.6, ease: "power1.inOut", immediateRender: false }, T.s6 + 3.9);
        fade("#vs-prog", T.s6 + 4.55, 1, 0, 0.08);
        fade("#vs-done", T.s6 + 4.55, 0, 1, 0.1);
        light("#step2", T.s6 + 4.9, false);
        light("#step3", T.s6 + 4.9, true);
        swap("#scr-shell", "#scr-boot", T.s6 + 5.0, 0.12);
        tl.fromTo(".boot-dot", { rotation: 0 }, { rotation: 360, duration: 0.8, ease: "none", immediateRender: false }, T.s6 + 5.0);

        // ============================================================ S7: L + R
        caption("cap7", T.s7 + 0.1, T.s8 - 0.35);
        camTo(T.s7 - 0.1, CAM.center, 0.8);
        tl.set("#vn-text-2", { opacity: 0 }, T.s7);
        tl.set("#vn-text-1", { opacity: 1 }, T.s7);
        swap("#scr-boot", "#scr-vn", T.s7 + 0.15, 0.25);
        ["#call-l", "#call-r"].forEach((s) => {
          tl.fromTo(s, { opacity: 0, scale: 0.6, y: 30 }, { opacity: 1, scale: 1, y: 0, duration: 0.4, ease: "back.out(2)", immediateRender: false }, T.s7 + 0.9);
          tl.fromTo(s, { y: 0 }, { y: 12, duration: 0.07, immediateRender: false }, T.s7 + 1.55);
          tl.fromTo(s, { y: 12 }, { y: 0, duration: 0.18, immediateRender: false }, T.s7 + 1.7);
          tl.fromTo(s, { opacity: 1 }, { opacity: 0, duration: 0.3, immediateRender: false }, T.s7 + 2.8);
        });
        press(T.s7 + 1.55, "btn-l");
        press(T.s7 + 1.55, "btn-r");
        tl.set("#hl-kira", { color: "#ffd24a" }, T.s7);
        tl.set("#hl-issho", { color: "#ffffff" }, T.s7);
        tl.set("#ovb-queued", { opacity: 0 }, T.s7);
        tl.set("#ovb-kira", { opacity: 1 }, T.s7);
        fade("#scr-overlay", T.s7 + 1.7, 0, 1, 0.12);
        camTo(T.s7 + 1.7, CAM.zoom, 0.8);

        // ============================================================ S8: the Overlay app, beside the game
        caption("cap8", T.s8 + 0.1, T.s9 - 0.35);
        camTo(T.s8, CAM.center, 0.6);
        press(T.s8 + 0.3, "btn-ps");
        fade("#scr-overlay", T.s8 + 0.35, 1, 0, 0.1);
        tl.fromTo("#scr-home", { opacity: 0, scale: 1.06 }, { opacity: 1, scale: 1, duration: 0.25, ease: "power2.out", immediateRender: false }, T.s8 + 0.4);
        fade("#scr-vn", T.s8 + 0.55, 1, 0, 0.1);
        tap(T.s8 + 1.2, 740, 146);
        tl.fromTo("#scr-la-app", { opacity: 0, scale: 0.92 }, { opacity: 1, scale: 1, duration: 0.25, ease: "power2.out", immediateRender: false }, T.s8 + 1.35);
        fade("#scr-home", T.s8 + 1.55, 1, 0, 0.05);
        tap(T.s8 + 2.0, 480, 293);
        tl.fromTo("#app-games-a", { opacity: 0, scale: 0.94 }, { opacity: 1, scale: 1, duration: 0.25, ease: "power2.out", immediateRender: false }, T.s8 + 2.15);
        fade("#scr-la-app", T.s8 + 2.35, 1, 0, 0.05);
        press(T.s8 + 2.65, "dpad-down");
        swap("#app-games-a", "#app-games-b", T.s8 + 2.72, 0.04);
        press(T.s8 + 3.15, "btn-cross");
        swap("#app-games-b", "#app-games-c", T.s8 + 3.22, 0.04);
        press(T.s8 + 4.0, "btn-ps");
        tl.fromTo("#scr-la-game", { opacity: 0, scale: 1.05 }, { opacity: 1, scale: 1, duration: 0.25, ease: "power2.out", immediateRender: false }, T.s8 + 4.1);
        fade("#app-games-c", T.s8 + 4.3, 1, 0, 0.05);
        tap(T.s8 + 4.65, 260, 385);
        swap("#scr-la-game", "#scr-vn", T.s8 + 4.8, 0.2);

        // ============================================================ S9: the OCR engine
        caption("cap9", T.s9 + 0.1, T.end - 0.3);
        swap("#scr-vn", "#app-ocr-a", T.s9 + 0.05, 0.16);
        camTo(T.s9, CAM.zoom, 0.7);
        tap(T.s9 + 1.05, 702, 178);
        swap("#app-ocr-a", "#app-ocr-b", T.s9 + 1.1, 0.04);

        // portrait: the name and address under the film, from S2 to the end card
        fade("#brand", T.s2 + 0.3, 0, 1, 0.4);
        fade("#brand", T.end - 0.15, 1, 0, 0.3);

        // ============================================================ end card
        tl.fromTo("#stage", { opacity: 1 }, { opacity: 0, duration: 0.35, ease: "power2.in", immediateRender: false }, T.end - 0.15);
        camTo(T.end - 0.15, CAM.out, 0.5, "power2.in");
        fade("#endcard", T.end + 0.1, 0, 1, 0.01);
        tl.fromTo("#endcard img", { scale: 0.5, opacity: 0 }, { scale: 1, opacity: 1, duration: 0.5, ease: "back.out(1.7)", immediateRender: false }, T.end + 0.1);
        tl.fromTo("#endcard h2, #endcard p", { opacity: 0, y: 24 }, { opacity: 1, y: 0, duration: 0.45, stagger: 0.09, ease: "power3.out", immediateRender: false }, T.end + 0.25);

        window.__timelines["main"] = tl;
      })();
