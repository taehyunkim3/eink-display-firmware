# Galmuri bitmap font

The generated firmware bitmap fonts are derived from Galmuri.

- Source: https://github.com/quiple/galmuri
- Release used for generation: Galmuri 2.40.3
- License: SIL Open Font License 1.1

The original TTF is not checked in. Regenerate the firmware headers with:

```sh
python3 tools/fonts/generate_bitmap_font.py \
  --font /path/to/Galmuri7.ttf \
  --output include/generated/galmuri_7_bitmap.h \
  --size 7 \
  --symbol-prefix GALMURI7 \
  --source-name Galmuri7

python3 tools/fonts/generate_bitmap_font.py \
  --font /path/to/Galmuri9.ttf \
  --output include/generated/galmuri_9_bitmap.h \
  --size 9 \
  --symbol-prefix GALMURI9 \
  --source-name Galmuri9

python3 tools/fonts/generate_bitmap_font.py \
  --font /path/to/Galmuri11.ttf \
  --output include/generated/galmuri_11_bitmap.h \
  --size 11 \
  --symbol-prefix GALMURI11 \
  --source-name Galmuri11

python3 tools/fonts/generate_bitmap_font.py \
  --font /path/to/Galmuri11-Bold.ttf \
  --output include/generated/galmuri_11_bold_bitmap.h \
  --size 11 \
  --symbol-prefix GALMURI11BOLD \
  --source-name Galmuri11-Bold

python3 tools/fonts/generate_bitmap_font.py \
  --font /path/to/Galmuri14.ttf \
  --output include/generated/galmuri_14_bitmap.h \
  --size 14 \
  --symbol-prefix GALMURI14 \
  --source-name Galmuri14
```
