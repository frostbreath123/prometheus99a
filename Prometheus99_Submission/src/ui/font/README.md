# The font of the user interface

`p99_font.c` is the only font in the program. It is generated, not written by hand:

```
npm install lv_font_conv
node_modules/.bin/lv_font_conv --font third_party/lvgl/scripts/built_in_font/DejaVuSans.ttf \
    -r 0x20-0x7E --size 14 --bpp 1 --format lvgl --no-compress --no-kerning --lv-font-name p99_font -o p99_font.c
```

then the include block at the top was replaced by `#include "lvgl.h"` and a comment, as in the file.

Why this font and these options

- **ASCII only (0x20 to 0x7E).** Every text the program shows is ASCII: status words, numbers, units (`%`, `C`, `kPa`,
  `l/min`), alarm words. There is no symbol glyph and no accented letter. The library's Montserrat 14 carries the
  FontAwesome symbols and kerning tables and its data was 13.4 KB in the first build (bitmaps 8832, glyph tables 1264,
  kerning 3305 bytes, from `nm -S`); this one is 1.5 KB (bitmaps 742, glyph tables 768).
- **1 bit per pixel.** No anti-aliasing, so text adds no blended colours: a screen shows only the colours of the
  palette in `src/ui/ui_priv.h`. The screenshot test counts them.
- **DejaVu Sans, 14 px.** It is hinted, so at one bit per pixel the strokes stay clean. Montserrat at one bit per pixel
  was not tried on screen. LVGL's built-in `unscii` font is 8 by 8 pixels doubled to 16 and is 16 pixels wide per
  character, which broke the layout of the header and of the tables.
- **No kerning.** About 3.3 KB of tables saved; the spacing is a little looser.

If a text needs a character outside ASCII, generate the font again with a larger range and check the size gate in
`bench/gate.sh`. Licence: see `docs/THIRD_PARTY.md` (Bitstream Vera licence with the DejaVu changes).
