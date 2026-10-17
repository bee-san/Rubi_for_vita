# short_line

`textbox.pgm` (852×135, 8-bit grey) is the dialogue box of #194 as test.4's console run read it: CLANNAD's
region (852×135), a full-width line and its two-glyph last line, white on the box's dark blue, with the
page arrow at the bottom right. GRAD, TEXTBOX and TEXTBOX_RECOVERY find only the first line in it, as
they did on the console. `host/tests/test_vocr_policy.c` (`a_short_last_line_is_read`) reads it.

Rendered with ImageMagick and Noto Sans CJK JP Medium (SIL Open Font License 1.1); no game pixels:

```sh
magick -size 852x135 xc:'rgb(28,30,62)' -font NotoSansCJK-Medium.ttc -pointsize 29 -fill white \
  -annotate +12+44 '近づこうとして、彼女が熱心に本を読んでいるのに気づい' -annotate +12+86 'た。' \
  -fill 'rgb(230,120,40)' -draw 'polygon 818,112 832,112 825,121' -colorspace gray -depth 8 textbox.pgm
```
