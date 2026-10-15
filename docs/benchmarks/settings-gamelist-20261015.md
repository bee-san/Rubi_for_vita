# Games list benchmark, 2026-10-15 (host harness)

`vjo-settings-bench GAMES 5` (host/tests/test_settings_games.c, docs/settings-readability.md "Measurements (host)"): the real settings/main.c and settings/catalog.c over synthetic games, timed per frame on the fleet box (x86_64), not on a Vita. "before" is main 7ed16d8's settings/main.c (-DVJO_SETTINGS_BENCH_MAIN); "after" this change. "GPU rules" lists the frees libvita2d forbids (a texture freed after a draw with no GPU wait) and frees of its two uncleared fields.

## 100 games, before

100 games + Adrenaline's entry; icons 128x128 8-bit palette, zlib-compressed; frames after the first fill (idle: 30, others: 20); the run with the median mean of 5
| scenario | frames | mean ms | p50 ms | p95 ms | max ms | view moves | ms a move (mean) | ms a move (max) | loads on a move | glyphs/frame | max glyphs | loads | max loads/frame | icon reads | GPU waits | max icons | max CDRAM KiB | GPU rules |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| idle | 90 | 0.0045 | 0.0047 | 0.0062 | 0.0065 | 0 | 0.0000 | 0.0000 | 0.00 | 3323 | 3323 | 0 | 0 | 0 | 0 | 3 | 768 | freed in flight 0, while drawing 0; uncleared fields 3; drawn after free 0; alive at exit 0 |
| scroll | 440 | 0.0733 | 0.0064 | 0.5395 | 0.5809 | 58 | 0.5173 | 0.5809 | 2.90 | 4681 | 7214 | 168 | 3 | 2352 | 0 | 3 | 768 | freed in flight 168, while drawing 0; uncleared fields 171; drawn after free 0; alive at exit 0 |
| pages | 426 | 0.0265 | 0.0050 | 0.0098 | 0.5632 | 17 | 0.5296 | 0.5632 | 2.88 | 4293 | 7214 | 49 | 3 | 686 | 0 | 3 | 768 | freed in flight 49, while drawing 0; uncleared fields 52; drawn after free 0; alive at exit 0 |
| toggle | 156 | 0.0369 | 0.0031 | 0.5107 | 0.6806 | 2 | 0.4422 | 0.5291 | 2.50 | 2644 | 2663 | 26 | 3 | 364 | 0 | 3 | 768 | freed in flight 26, while drawing 0; uncleared fields 29; drawn after free 0; alive at exit 0 |

startup (the catalog scan of 100 games, before the first frame): 2.092 ms

## 100 games, after

100 games + Adrenaline's entry; icons 128x128 8-bit palette, zlib-compressed; frames after the first fill (idle: 30, others: 20); the run with the median mean of 5
| scenario | frames | mean ms | p50 ms | p95 ms | max ms | view moves | ms a move (mean) | ms a move (max) | loads on a move | glyphs/frame | max glyphs | loads | max loads/frame | icon reads | GPU waits | max icons | max CDRAM KiB | GPU rules |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| idle | 90 | 0.0010 | 0.0010 | 0.0011 | 0.0011 | 0 | 0.0000 | 0.0000 | 0.00 | 61 | 61 | 0 | 0 | 0 | 0 | 6 | 1536 | kept |
| scroll | 440 | 0.0230 | 0.0012 | 0.1800 | 0.2199 | 58 | 0.1657 | 0.2199 | 0.91 | 95 | 2777 | 53 | 1 | 106 | 50 | 9 | 2304 | kept |
| pages | 426 | 0.0467 | 0.0013 | 0.1804 | 0.2247 | 39 | 0.1714 | 0.2125 | 0.95 | 158 | 2779 | 108 | 1 | 216 | 105 | 9 | 2304 | kept |
| toggle | 156 | 0.0089 | 0.0010 | 0.1100 | 0.2193 | 2 | 0.1118 | 0.2181 | 0.50 | 69 | 1052 | 2 | 1 | 4 | 0 | 8 | 2048 | kept |

startup (the catalog scan of 100 games, before the first frame): 2.178 ms

## 512 games, before

512 games + Adrenaline's entry; icons 128x128 8-bit palette, zlib-compressed; frames after the first fill (idle: 30, others: 20); the run with the median mean of 5
| scenario | frames | mean ms | p50 ms | p95 ms | max ms | view moves | ms a move (mean) | ms a move (max) | loads on a move | glyphs/frame | max glyphs | loads | max loads/frame | icon reads | GPU waits | max icons | max CDRAM KiB | GPU rules |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| idle | 90 | 0.0088 | 0.0087 | 0.0108 | 0.0117 | 0 | 0.0000 | 0.0000 | 0.00 | 6049 | 6049 | 0 | 0 | 0 | 0 | 3 | 768 | freed in flight 0, while drawing 0; uncleared fields 3; drawn after free 0; alive at exit 0 |
| scroll | 440 | 0.0757 | 0.0063 | 0.5417 | 0.5612 | 58 | 0.5362 | 0.5612 | 3.00 | 4628 | 6177 | 174 | 3 | 2436 | 0 | 3 | 768 | freed in flight 174, while drawing 0; uncleared fields 177; drawn after free 0; alive at exit 0 |
| pages | 426 | 0.0277 | 0.0063 | 0.0091 | 0.5736 | 17 | 0.5485 | 0.5736 | 3.00 | 4357 | 6162 | 51 | 3 | 714 | 0 | 3 | 768 | freed in flight 51, while drawing 0; uncleared fields 54; drawn after free 0; alive at exit 0 |
| toggle | 156 | 0.0423 | 0.0080 | 0.5221 | 0.7414 | 2 | 0.4514 | 0.5334 | 2.50 | 5093 | 5389 | 26 | 3 | 364 | 0 | 3 | 768 | freed in flight 26, while drawing 0; uncleared fields 29; drawn after free 0; alive at exit 0 |

startup (the catalog scan of 512 games, before the first frame): 10.791 ms

## 512 games, after

512 games + Adrenaline's entry; icons 128x128 8-bit palette, zlib-compressed; frames after the first fill (idle: 30, others: 20); the run with the median mean of 5
| scenario | frames | mean ms | p50 ms | p95 ms | max ms | view moves | ms a move (mean) | ms a move (max) | loads on a move | glyphs/frame | max glyphs | loads | max loads/frame | icon reads | GPU waits | max icons | max CDRAM KiB | GPU rules |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| idle | 90 | 0.0011 | 0.0011 | 0.0012 | 0.0012 | 0 | 0.0000 | 0.0000 | 0.00 | 61 | 61 | 0 | 0 | 0 | 0 | 6 | 1536 | kept |
| scroll | 440 | 0.0235 | 0.0012 | 0.1776 | 0.2207 | 58 | 0.1701 | 0.2207 | 0.95 | 91 | 2777 | 55 | 1 | 110 | 52 | 9 | 2304 | kept |
| pages | 426 | 0.0480 | 0.0012 | 0.1806 | 0.2179 | 39 | 0.1715 | 0.2097 | 0.95 | 161 | 2777 | 111 | 1 | 222 | 108 | 9 | 2304 | kept |
| toggle | 156 | 0.0095 | 0.0011 | 0.1292 | 0.2278 | 2 | 0.1239 | 0.2278 | 0.50 | 79 | 1787 | 2 | 1 | 4 | 0 | 8 | 2048 | kept |

startup (the catalog scan of 512 games, before the first frame): 10.593 ms
