# Where the Overlay app's art comes from

The mascot in `icon0.png`, `startup.png` and `bg.png` is an original character made
for this project: a chibi girl with a peach-pink bob, a star hair clip and round
glasses, reading a book, with a speech bubble that says あ. No character, artist,
studio or franchise is named in any prompt. The pick was reviewed by eye for a
resemblance to well-known characters and none close was found; that is a judgment,
not a reverse-image search.

`icon-source.png` is the unedited model output (sha256
`3bc8d4250b2ec35766e0b8ca4bf2431d8f199b699426a2d47ad49b7a4ea79991`, 512 x 512 RGB;
its `parameters` text chunk holds the prompt and settings below; the chunk is
Latin-1, so あ appears there as its three UTF-8 bytes).
`tools/make_settings_assets.py` builds the three LiveArea images from it. The
images ship as part of this GPL-3.0 project.

## Model and licences

| Part | File (sha256) | Licence |
| --- | --- | --- |
| FLUX.1 [schnell], Black Forest Labs | `flux1-schnell.safetensors` (`9403429e0052277ac2a87ad800adece5481eecefd9ed334e1f348723621d2a0a`) and `ae.safetensors` (`afc8e28272cd15db3919bacdb6918ce9c1ed22e96cb12c4d5ed0fba823529e38`), the same bytes as [black-forest-labs/FLUX.1-schnell@741f7c3](https://huggingface.co/black-forest-labs/FLUX.1-schnell/tree/741f7c3ce8b383c54771c7003378a50191e9efe9) | Apache-2.0; the model card says it "can be used for personal, scientific, and commercial purposes" |
| CLIP ViT-L/14 text encoder, OpenAI | `clip_l.safetensors` (`660c6f5b1abae9dc498ac2d21e1347d2abdb0cf6c0c0c8576cd796491d9a6cdd`) | MIT |
| T5 v1.1 XXL encoder, Google | `t5xxl_fp16.safetensors` (`6e480b09fae049a72d2a8c5fbccb8d3e92febeb233bbe9dfe7256958a9167635`) | Apache-2.0 |
| [stable-diffusion.cpp](https://github.com/leejet/stable-diffusion.cpp) `master-959-d637cfb` (ggml `d25b121`) | built from source, `GGML_NATIVE=ON`, CPU only | MIT |

Apache-2.0 and MIT are compatible with this project's GPL-3.0. All four model files
were downloaded from the ungated mirror
[second-state/FLUX.1-schnell-GGUF@8c45a2b](https://huggingface.co/second-state/FLUX.1-schnell-GGUF/tree/8c45a2ba25e2d02bd34230989fb54983f39e44ec)
and checked against the sha256 values above. The transformer and T5 were converted
once to `q8_0` with `sd-cli -M convert --type q8_0`, giving `flux1-schnell-q8_0.gguf`
(`41bcd16457c0faf714700147384e3270e26d40e411a5b2ad9db0a775ab7c07d8`) and
`t5xxl-q8_0.gguf` (`fc07757bf7ad40eaf612acc7ed0c0a7ab71189979e0b8b4d14601017baca22de`).

## Generation

On an AMD EPYC 9R14 with 5 threads, 344 s per image:

```sh
sd-cli --diffusion-model flux1-schnell-q8_0.gguf --vae ae.safetensors \
  --clip_l clip_l.safetensors --t5xxl t5xxl-q8_0.gguf \
  -p "$(cat prompt.txt)" --cfg-scale 1.0 --sampling-method euler --steps 4 \
  -W 512 -H 512 -s 1001 -t 5 -o icon-source.png
```

No negative prompt; every other option, including scheduler `flux` and RNG `cuda`,
is at its default (the image's metadata lists them). The prompt (`prompt.txt`,
candidate A, seed 1001):

```text
Kawaii chibi anime girl mascot for an app icon. Head and shoulders only, her big round face fills most of the frame. Huge round sparkling brown eyes, a small happy smile, pink blush on her cheeks. Short fluffy peach-pink bob haircut with straight bangs and a small white star hair clip. Big round thin-rimmed glasses. She hugs an open book under her chin with both hands. A white speech bubble next to her head holds a single bold black Japanese hiragana character "あ". Flat vector illustration, thick dark-brown outlines, simple clean shapes, soft cel shading, pastel color palette, plain soft lavender background, centered composition, wholesome children's sticker style.
```

Floating-point results can differ between CPUs and ggml builds, so a rerun elsewhere
may not give the same bytes; `icon-source.png` is the reference.

## Edits after the model

The model drew an unreadable glyph in the speech bubble. `make_settings_assets.py`
paints the bubble's inside white and draws あ from hand-made Bezier strokes in the
outline colour. It writes `icon0.png` (the whole picture, 128 x 128), `startup.png`
(the mascot cut out of her background, with an "Overlay" wordmark drawn from round
strokes, no font) and `bg.png` (a soft gradient with faint あ bubbles, sparkles, the
mascot and the wordmark; `pic0.png`, the 960 x 544 launch splash, is the same
picture scaled to the full height with its plain edges extended), each quantized to at most 256 colours as an opaque 8-bit
palette PNG.

## The other candidates

Four candidates were compared at 128 and 64 px
([contact sheet](../../docs/img/overlay-icon-candidates.png)). B to D used the same
command with the seeds and prompts below. Their bubble or book was asked to be blank,
and the sheet shows them with the same hand-drawn あ added.

| | Seed | Character | Not picked because |
| --- | --- | --- | --- |
| A | 1001 | peach-pink bob, star clip, round glasses, reads a book; あ bubble | picked: the whole face, glasses, book and bubble still read at 64 px |
| B | 2002 | lavender hair, mint ribbon, peeks over a big あ speech bubble | the sign hides her face; at 64 px it reads as a sign more than a girl |
| C | 1001 | honey-blonde hair, big pink bow, round glasses, peeks over a book with あ | the book hides the lower face |
| D | 1001 | chestnut bob with two buns, yellow hoodie; あ bubble | at 64 px the buns read as bear ears; no reading cue besides the bubble |

```text
B, seed 2002: Kawaii chibi anime girl mascot for an app icon. Head and shoulders only, her round face fills most of the frame. Huge sparkling violet eyes, an open happy smile, rosy blush. Short wavy lavender hair with a small side ponytail tied with a mint-green ribbon. She holds up a big blank white rounded speech bubble sign with both hands, with nothing written on it. Flat vector illustration, thick dark outlines, simple clean shapes, soft cel shading, pastel color palette, plain pale mint background, centered composition, wholesome children's sticker style.

C, seed 1001: Kawaii chibi anime girl mascot for an app icon. Her big round head peeks over the top edge of a closed pastel blue book that she holds up in front of her face with both hands, so only her eyes and hair are visible above the book. Huge round sparkling blue eyes behind big round glasses, pink blush. Fluffy short honey-blonde hair with a large pastel pink bow on top. The book cover is plain with no text. Flat vector illustration, thick dark outlines, simple clean shapes, soft cel shading, pastel color palette, plain soft peach background, centered composition, wholesome children's sticker style.

D, seed 1001: Kawaii chibi anime girl mascot for an app icon. Head and shoulders only, her big round face fills most of the frame. Huge sparkling green eyes, a cheerful smile, pink blush. Short chestnut-brown bob hair with two tiny round buns on top. She wears a cozy pastel yellow hoodie. A small blank white speech bubble with nothing written inside floats beside her head, with little sparkles around it. Flat vector illustration, thick dark outlines, simple clean shapes, soft cel shading, pastel color palette, plain soft sky-blue background, centered composition, wholesome children's sticker style.
```

Seed 2002 of A, C and D and seed 1001 of B were also generated and set aside after
a look: A's was close to the pick but less tidy, C's and D's were close variations of
the same designs, and B's showed her whole body, small, behind the bubble.
