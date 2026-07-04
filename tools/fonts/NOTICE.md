# NanumGothicCoding bitmap font

The generated firmware bitmap font is derived from NAVER NanumGothicCoding.

- Source: https://github.com/naver/nanumfont
- Release used for generation: NanumGothicCoding 2.5
- License: Open Font License

The original TTF is not checked in. Regenerate the firmware headers with:

```sh
python3 tools/fonts/generate_nanum_bitmap_font.py \
  --font /path/to/NanumGothicCoding.ttf \
  --output include/generated/nanum_gothic_coding_12_bitmap.h \
  --size 12 \
  --symbol-prefix NANUM12

python3 tools/fonts/generate_nanum_bitmap_font.py \
  --font /path/to/NanumGothicCoding.ttf \
  --output include/generated/nanum_gothic_coding_14_bitmap.h \
  --size 14 \
  --symbol-prefix NANUM14

python3 tools/fonts/generate_nanum_bitmap_font.py \
  --font /path/to/NanumGothicCoding.ttf \
  --output include/generated/nanum_gothic_coding_18_bitmap.h \
  --size 18 \
  --symbol-prefix NANUM18
```
