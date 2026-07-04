# Galmuri bitmap font

The generated firmware bitmap fonts are derived from Galmuri.

- Source: https://github.com/quiple/galmuri
- Release used for generation: Galmuri 2.40.3 (npm package `galmuri`)
- License: SIL Open Font License 1.1

Generation reads the original BDF bitmap data directly (no rasterization,
no thresholding), so glyphs are pixel-identical to the source font and
carry per-glyph advance widths.

The BDF files are not checked in. Download them (e.g. from the npm tarball
`https://registry.npmjs.org/galmuri/-/galmuri-2.40.3.tgz`, files under
`package/dist/*.bdf`) and regenerate the firmware headers with:

```sh
python3 tools/fonts/generate_bitmap_font.py \
  --bdf /path/to/Galmuri7.bdf \
  --output include/generated/galmuri_7_bitmap.h \
  --symbol-prefix GALMURI7 \
  --source-name Galmuri7

python3 tools/fonts/generate_bitmap_font.py \
  --bdf /path/to/Galmuri9.bdf \
  --output include/generated/galmuri_9_bitmap.h \
  --symbol-prefix GALMURI9 \
  --source-name Galmuri9

python3 tools/fonts/generate_bitmap_font.py \
  --bdf /path/to/Galmuri11.bdf \
  --output include/generated/galmuri_11_bitmap.h \
  --symbol-prefix GALMURI11 \
  --source-name Galmuri11

python3 tools/fonts/generate_bitmap_font.py \
  --bdf /path/to/Galmuri11-Bold.bdf \
  --output include/generated/galmuri_11_bold_bitmap.h \
  --symbol-prefix GALMURI11BOLD \
  --source-name Galmuri11-Bold

python3 tools/fonts/generate_bitmap_font.py \
  --bdf /path/to/Galmuri14.bdf \
  --output include/generated/galmuri_14_bitmap.h \
  --symbol-prefix GALMURI14 \
  --source-name Galmuri14
```
