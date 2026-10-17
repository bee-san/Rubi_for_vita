# Where the composition's art comes from

Written by `tools/prep_assets.py` from `capture/run.sh`'s output for Rubi `8e267fb` (the real
captures: the overlay's states, the card, the app in Vita3K, the release's sizes), that checkout's
`settings/assets/`, and promo-assets' shared folder (the original mock game, the sound; its MANIFEST.md has each
file's licence and source).

| output | input | input sha256 | output sha256 | how |
| --- | --- | --- | --- | --- |
| `src/overlay-states.json` | `capture: overlay-states.json` | `d633e62d13e05dab…` | `d633e62d13e05dab…` | copy |
| `assets/vn/bg-s1.jpg` | `promo-assets: vn/s1/bg@2x.png` | `2aeb413bec240e47…` | `6b69c21fe305ef8b…` | JPEG q90 4:4:4 |
| `assets/vn/bg-s2.jpg` | `promo-assets: vn/s2/bg@2x.png` | `4e0a209a223e8716…` | `dc30783dfc06959a…` | JPEG q90 4:4:4 |
| `assets/vn/bg-s3.jpg` | `promo-assets: vn/s3/bg@2x.png` | `a517433c1f074d0e…` | `f94d818c5d6ba76c…` | JPEG q90 4:4:4 |
| `assets/vn/hinata-smile.png` | `promo-assets: vn/sprite/hinata-smile.png` | `738fcbb0a9cdf2bb…` | `738fcbb0a9cdf2bb…` | copy |
| `assets/vn/hinata-surprised.png` | `promo-assets: vn/sprite/hinata-surprised.png` | `9c456346a5f1297b…` | `9c456346a5f1297b…` | copy |
| `assets/vn/hinata-happy.png` | `promo-assets: vn/sprite/hinata-happy.png` | `3a317e40da848713…` | `3a317e40da848713…` | copy |
| `assets/vn/shot-s1-m2.jpg` | `capture: anki/shot-s1-m2-q70.jpg` | `8088ef7224a9c853…` | `8088ef7224a9c853…` | copy (core/jpegsw.c at quality 70, host vjo-jpeg) |
| `assets/anki/card-1-front.png` | `capture: anki/png/card-1-front-desktop-light.png` | `39a5c568c60439a6…` | `39a5c568c60439a6…` | copy (Anki 26.09.3, Lapis 1.7.0) |
| `assets/anki/card-1-back.png` | `capture: anki/png/card-1-back-desktop-light.png` | `50c007688d04e6ac…` | `50c007688d04e6ac…` | copy (Anki 26.09.3, Lapis 1.7.0) |
| `assets/shots/install-1-plan.png` | `capture: app/run/holds-1/hold-04.png` | `4aaa125e94db5eb2…` | `4aaa125e94db5eb2…` | PNG (colours checked against settings/main.c) |
| `assets/shots/install-2-checking.png` | `capture: app/run/progress/f-010.png` | `b14ee03c61a16b72…` | `e5623e893985d1fa…` | PNG (colours checked against settings/main.c) |
| `assets/shots/install-3-checking.png` | `capture: app/run/progress/f-027.png` | `bed2f06c456eacda…` | `7dd6d4aa95dae7b5…` | PNG (colours checked against settings/main.c) |
| `assets/shots/install-4-copying.png` | `capture: app/run/progress/f-037.png` | `72842609ded62231…` | `6c216e6096185010…` | PNG (colours checked against settings/main.c) |
| `assets/shots/install-5-copying.png` | `capture: app/run/progress/f-040.png` | `b6af2448521042c1…` | `440aa59bc6bba683…` | PNG (colours checked against settings/main.c) |
| `assets/shots/install-6-done.png` | `capture: app/run/holds-1/hold-05.png` | `79d70f90542b85ce…` | `79d70f90542b85ce…` | PNG (colours checked against settings/main.c) |
| `assets/shots/games-1-top.png` | `capture: app/run/holds-2/hold-03.png` | `91ae9fbf32cea1f4…` | `91ae9fbf32cea1f4…` | PNG (colours checked against settings/main.c) |
| `assets/shots/games-2-down.png` | `capture: app/run/holds-2/hold-04.png` | `1b8a2e6efec87985…` | `1b8a2e6efec87985…` | PNG (colours checked against settings/main.c) |
| `assets/shots/games-3-down.png` | `capture: app/run/holds-2/hold-05.png` | `b06b7dc9ec116e55…` | `b06b7dc9ec116e55…` | PNG (colours checked against settings/main.c) |
| `assets/shots/games-4-off.png` | `capture: app/run/holds-2/hold-06.png` | `4ae316c3f4589741…` | `4ae316c3f4589741…` | PNG (colours checked against settings/main.c) |
| `assets/shots/read-1-auto.png` | `capture: app/run/holds-2/hold-17.png` | `b159854f57825f8a…` | `b159854f57825f8a…` | PNG (colours checked against settings/main.c) |
| `assets/shots/read-2-on-press.png` | `capture: app/run/holds-2/hold-18.png` | `ea17fb3ecd58e64c…` | `ea17fb3ecd58e64c…` | PNG (colours checked against settings/main.c) |
| `assets/app/icon0.png` | `checkout: settings/assets/icon0.png` | `63ac81a4dcd7a500…` | `63ac81a4dcd7a500…` | copy |
| `assets/app/bg.png` | `checkout: settings/assets/bg.png` | `14e1b661c9b37b4c…` | `14e1b661c9b37b4c…` | copy |
| `assets/app/startup.png` | `checkout: settings/assets/startup.png` | `9856bd12d7b03508…` | `9856bd12d7b03508…` | copy |
| `assets/audio/bed-42s.m4a` | `promo-assets: src/make_audio.py` | `1d37fbf1d120e770…` | `78737dd4b04a20b2…` | tools/make_bed.py: bed-44s's own code without its bar 13 (42 s), AAC |
| `assets/audio/sfx-click.wav` | `promo-assets: audio/sfx/click.wav` | `2f246507f900bfb9…` | `2f246507f900bfb9…` | copy |
| `assets/audio/sfx-lr.wav` | `promo-assets: audio/sfx/lr.wav` | `7f4e65b7caea7adc…` | `7f4e65b7caea7adc…` | copy |
| `assets/audio/sfx-dpad.wav` | `promo-assets: audio/sfx/dpad.wav` | `5e0e54b67e12e213…` | `5e0e54b67e12e213…` | copy |
| `assets/audio/sfx-whoosh-in.wav` | `promo-assets: audio/sfx/whoosh-in.wav` | `9ebe45a9c536d130…` | `9ebe45a9c536d130…` | copy |
| `assets/audio/sfx-whoosh-out.wav` | `promo-assets: audio/sfx/whoosh-out.wav` | `0a5df7665220d907…` | `0a5df7665220d907…` | copy |
| `assets/audio/sfx-swish.wav` | `promo-assets: audio/sfx/swish.wav` | `d6a2b93aa347f6fa…` | `d6a2b93aa347f6fa…` | copy |
| `assets/audio/sfx-pop.wav` | `promo-assets: audio/sfx/pop.wav` | `6ece084f6a1e3328…` | `6ece084f6a1e3328…` | copy |
| `assets/audio/sfx-chime.wav` | `promo-assets: audio/sfx/chime.wav` | `d72ea74deb9f1fb3…` | `d72ea74deb9f1fb3…` | copy |
| `assets/audio/sfx-confirm.wav` | `promo-assets: audio/sfx/confirm.wav` | `68964a886f20bcc2…` | `68964a886f20bcc2…` | copy |
| `assets/audio/sfx-text-blip-1.wav` | `promo-assets: audio/sfx/text-blip-1.wav` | `eaf1b1b02bf7cecf…` | `eaf1b1b02bf7cecf…` | copy |
| `assets/audio/sfx-text-blip-2.wav` | `promo-assets: audio/sfx/text-blip-2.wav` | `8115066ca78018dd…` | `8115066ca78018dd…` | copy |
| `assets/audio/sfx-text-blip-3.wav` | `promo-assets: audio/sfx/text-blip-3.wav` | `c5cc27aff03357de…` | `c5cc27aff03357de…` | copy |
| `assets/audio/sfx-text-blip-4.wav` | `promo-assets: audio/sfx/text-blip-4.wav` | `2c2b42903a3369e9…` | `2c2b42903a3369e9…` | copy |
| `assets/audio/sfx-text-blip-5.wav` | `promo-assets: audio/sfx/text-blip-5.wav` | `5de3904dce5bc249…` | `5de3904dce5bc249…` | copy |
| `assets/icons/PROMO0001.png` | `promo-assets: app/icons/PROMO0001-yuunagi-step.png` | `fe0704a4efa31a88…` | `fe0704a4efa31a88…` | copy |
| `assets/icons/PROMO0002.png` | `promo-assets: app/icons/PROMO0002-neon-drift.png` | `59824a9d0a1385d1…` | `59824a9d0a1385d1…` | copy |
| `assets/icons/PROMO0003.png` | `promo-assets: app/icons/PROMO0003-courier-run.png` | `fc55b29b7e99219a…` | `fc55b29b7e99219a…` | copy |
| `assets/icons/PROMO0004.png` | `promo-assets: app/icons/PROMO0004-ember-garden.png` | `82f0eb485a649251…` | `82f0eb485a649251…` | copy |
| `assets/icons/PROMO0005.png` | `promo-assets: app/icons/PROMO0005-hoshizora-diary.png` | `fcb55687a1f7f98e…` | `fcb55687a1f7f98e…` | copy |
| `src/release.json` | `capture: release/sizes.json` | `361f474216256e01…` | `88d4f091b6c03ad4…` | the sizes of capture/run.sh's package_variants.py release |
| `assets/device/vita-white.svg` | `committed` | `c462a6a69017bdbb…` | `c462a6a69017bdbb…` | the console: an original drawing from its published dimensions, no photo traced, no logos (#134's; its power button added for the final cut); CC0-1.0 |
| `assets/icons/vitashell.svg` | `committed` | `9554feeefe5f499e…` | `9554feeefe5f499e…` | VitaShell's bubble on the home screen: a generic file-manager icon, original, drawn for promo v2; GPL-3.0-only |
