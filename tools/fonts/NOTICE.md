# NanumGothicCoding bitmap font

The generated firmware bitmap font is derived from NAVER NanumGothicCoding.

- Source: https://github.com/naver/nanumfont
- Release used for generation: NanumGothicCoding 2.5
- License: Open Font License

The original TTF is not checked in. Regenerate the firmware header with:

```sh
python3 tools/fonts/generate_nanum_bitmap_font.py \
  --font /path/to/NanumGothicCoding.ttf \
  --output include/generated/nanum_gothic_coding_bitmap.h
```
