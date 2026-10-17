# Where the composition's art comes from

Written by `tools/prep_assets.py` from `capture/run.sh`'s output for vita-jp-overlay `68fc2d9` (the real
captures: the overlay's states, the card, the app in Vita3K, the release's sizes), that checkout's
`settings/assets/`, and promo-assets' shared folder (the original mock game, the sound; its MANIFEST.md has each
file's licence and source).

| output | input | input sha256 | output sha256 | how |
| --- | --- | --- | --- | --- |
| `src/overlay-states.json` | `capture: overlay-states.json` | `f8968b89b236d75c…` | `f8968b89b236d75c…` | copy |
| `assets/vn/bg-s1.jpg` | `promo-assets: vn/s1/bg@2x.png` | `2aeb413bec240e47…` | `6b69c21fe305ef8b…` | JPEG q90 4:4:4 |
| `assets/vn/bg-s2.jpg` | `promo-assets: vn/s2/bg@2x.png` | `4e0a209a223e8716…` | `dc30783dfc06959a…` | JPEG q90 4:4:4 |
| `assets/vn/bg-s3.jpg` | `promo-assets: vn/s3/bg@2x.png` | `a517433c1f074d0e…` | `f94d818c5d6ba76c…` | JPEG q90 4:4:4 |
| `assets/vn/hinata-smile.png` | `promo-assets: vn/sprite/hinata-smile.png` | `738fcbb0a9cdf2bb…` | `738fcbb0a9cdf2bb…` | copy |
| `assets/vn/hinata-surprised.png` | `promo-assets: vn/sprite/hinata-surprised.png` | `9c456346a5f1297b…` | `9c456346a5f1297b…` | copy |
| `assets/vn/hinata-happy.png` | `promo-assets: vn/sprite/hinata-happy.png` | `3a317e40da848713…` | `3a317e40da848713…` | copy |
| `assets/vn/shot-s1-m2.jpg` | `capture: anki/shot-s1-m2-q70.jpg` | `8088ef7224a9c853…` | `8088ef7224a9c853…` | copy (core/jpegsw.c at quality 70, host vjo-jpeg) |
| `assets/anki/card-1-front.png` | `capture: anki/png/card-1-front-desktop-light.png` | `39a5c568c60439a6…` | `39a5c568c60439a6…` | copy (Anki 26.09.3, Lapis 1.7.0) |
| `assets/anki/card-1-back.png` | `capture: anki/png/card-1-back-desktop-light.png` | `50c007688d04e6ac…` | `50c007688d04e6ac…` | copy (Anki 26.09.3, Lapis 1.7.0) |
| `assets/shots/install-1-plan.png` | `capture: app/run/holds-1/hold-04.png` | `6dedd7f3923651a4…` | `6dedd7f3923651a4…` | PNG (colours checked against settings/main.c) |
| `assets/shots/install-2-checking.png` | `capture: app/run/progress/f-010.png` | `6c2fb0dbea893e7b…` | `08f2773b74784ecb…` | PNG (colours checked against settings/main.c) |
| `assets/shots/install-3-checking.png` | `capture: app/run/progress/f-027.png` | `aa40916dc640df52…` | `b536f8714175e260…` | PNG (colours checked against settings/main.c) |
| `assets/shots/install-4-copying.png` | `capture: app/run/progress/f-037.png` | `97d72c8a1082baee…` | `f88f9a59a3a8125e…` | PNG (colours checked against settings/main.c) |
| `assets/shots/install-5-copying.png` | `capture: app/run/progress/f-040.png` | `2942cfaadc08cace…` | `58b525412338072a…` | PNG (colours checked against settings/main.c) |
| `assets/shots/install-6-done.png` | `capture: app/run/holds-1/hold-05.png` | `4062c140658112c6…` | `4062c140658112c6…` | PNG (colours checked against settings/main.c) |
| `assets/shots/games-1-top.png` | `capture: app/run/holds-2/hold-03.png` | `42a764f7fab89bb1…` | `42a764f7fab89bb1…` | PNG (colours checked against settings/main.c) |
| `assets/shots/games-2-down.png` | `capture: app/run/holds-2/hold-04.png` | `df4327e2c27734d5…` | `df4327e2c27734d5…` | PNG (colours checked against settings/main.c) |
| `assets/shots/games-3-down.png` | `capture: app/run/holds-2/hold-05.png` | `c7a96bacf319cbc3…` | `c7a96bacf319cbc3…` | PNG (colours checked against settings/main.c) |
| `assets/shots/games-4-off.png` | `capture: app/run/holds-2/hold-06.png` | `7067097fce6bbaa3…` | `7067097fce6bbaa3…` | PNG (colours checked against settings/main.c) |
| `assets/shots/read-1-auto.png` | `capture: app/run/holds-2/hold-17.png` | `a1bfc831eeeaa04a…` | `a1bfc831eeeaa04a…` | PNG (colours checked against settings/main.c) |
| `assets/shots/read-2-on-press.png` | `capture: app/run/holds-2/hold-18.png` | `e822602ad4ae195e…` | `e822602ad4ae195e…` | PNG (colours checked against settings/main.c) |
| `assets/app/icon0.png` | `checkout: settings/assets/icon0.png` | `63ac81a4dcd7a500…` | `63ac81a4dcd7a500…` | copy |
| `assets/app/bg.png` | `checkout: settings/assets/bg.png` | `a8c702621b6284ed…` | `a8c702621b6284ed…` | copy |
| `assets/app/startup.png` | `checkout: settings/assets/startup.png` | `c397e22b59deaca8…` | `c397e22b59deaca8…` | copy |
| `assets/audio/bed-45s.m4a` | `promo-assets: audio/music/bed-44s.wav` | `fd1a20837b13b8e7…` | `19744aa13662554c…` | tools/make_bed.py: 1.0 s more breakdown at 29.5 s, AAC |
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
| `src/release.json` | `capture: release/sizes.json` | `a8e9630ad350e482…` | `c6d32da67974002e…` | the sizes of capture/run.sh's package_variants.py release |
| `assets/device/vita-white.svg` | `committed` | `c462a6a69017bdbb…` | `c462a6a69017bdbb…` | the console: an original drawing from its published dimensions, no photo traced, no logos (#134's; its power button added for the final cut); CC0-1.0 |
| `assets/icons/vitashell.svg` | `committed` | `9554feeefe5f499e…` | `9554feeefe5f499e…` | VitaShell's bubble on the home screen: a generic file-manager icon, original, drawn for promo v2; GPL-3.0-only |
