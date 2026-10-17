// render_cards.js: screenshots the card HTML Anki rendered (anki_cards.py: card.question()/card.answer()),
// inside a page shaped like Anki's reviewer (<body class="card card1 [nightMode night_mode]"><div id="qa">),
// in chrome-headless-shell, with the OFL Noto fonts that Lapis's font stacks name.
//   NODE_PATH=docs/promo/node_modules FONTS=DIR node render_cards.js CARDS_DIR OUT_DIR  (from promo-assets src/render_cards.js)
// Writes OUT_DIR/card-<n>-<front|back>-<desktop|phone>-<light|dark>.png.
const fs = require("fs");
const path = require("path");
const puppeteer = require("puppeteer-core");
const CHROME = process.env.CHROME ||
  path.join(process.env.HOME, ".cache/hyperframes/chrome/chrome-headless-shell/linux-152.0.7977.30/chrome-headless-shell-linux64/chrome-headless-shell");
const FONTS = process.env.FONTS || path.resolve(__dirname, "..", "fonts"); // Noto Sans JP and Noto Serif JP (OFL), as Lapis names them
const VIEWS = { desktop: { width: 1280, height: 800, deviceScaleFactor: 1.5 },
                phone: { width: 430, height: 932, deviceScaleFactor: 3, isMobile: true, hasTouch: true } };

function page(html, mode, view) {
  const night = mode === "dark" ? " nightMode night_mode" : "";
  const mobile = view === "phone" ? " mobile android" : "";
  return `<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
@font-face { font-family: "Noto Sans JP"; src: url("file://${FONTS}/NotoSansJP[wght].ttf"); font-weight: 100 900; }
@font-face { font-family: "Noto Serif JP"; src: url("file://${FONTS}/NotoSerifJP[wght].ttf"); font-weight: 200 900; }
html { height: 100%; }
body { margin: 20px; overflow-wrap: break-word; min-height: calc(100% - 40px); }
</style></head>
<body class="card card1${night}${mobile}"><div id="qa">${html}</div></body></html>`;
}

(async () => {
  const [cardsDir, outDir] = process.argv.slice(2);
  fs.mkdirSync(outDir, { recursive: true });
  const browser = await puppeteer.launch({ executablePath: CHROME, headless: true,
    args: ["--allow-file-access-from-files", "--disable-web-security", "--font-render-hinting=none"] });
  const files = fs.readdirSync(cardsDir).filter((f) => /^card-\d+-(front|back)\.html$/.test(f)).sort();
  for (const f of files) {
    const html = fs.readFileSync(path.join(cardsDir, f), "utf8")
      .replace(/src="(?!https?:|file:|data:)([^"]+)"/g, (m, p) => `src="file://${path.join(cardsDir, "media", p)}"`);
    for (const view of Object.keys(VIEWS)) {
      for (const mode of ["light", "dark"]) {
        const p = await browser.newPage();
        await p.setViewport(VIEWS[view]);
        const tmp = path.join(outDir, `.${f}.${view}.${mode}.html`);
        fs.writeFileSync(tmp, page(html, mode, view));
        await p.goto("file://" + tmp, { waitUntil: "load" });
        await p.evaluate(() => document.fonts.ready);
        await new Promise((r) => setTimeout(r, 400));  // the templates' scripts
        const out = path.join(outDir, f.replace(".html", `-${view}-${mode}.png`));
        await p.screenshot({ path: out, fullPage: false });
        fs.unlinkSync(tmp);
        await p.close();
        console.log(out);
      }
    }
  }
  await browser.close();
})().catch((e) => { console.error(e); process.exit(1); });
