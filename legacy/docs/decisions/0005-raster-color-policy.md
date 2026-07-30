# 0005 — Raster decode: byte-preserving sRGB, без ICC transforms

**Дата:** 2026-07-12
**Статус:** initial-policy; byte-exact research fixtures сохранены в
`docs/research/raster-normalization-goldens.md`
**Принял:** lead (делегировано owner)

## Решение

Canonical RGBA8 decode (master spec §11.1, §60 item 7) в v1:

- пиксельные байты сохраняются как есть; содержимое трактуется как sRGB;
- ICC-профили не применяются (наличие/повреждение профиля не меняет пиксели;
  повреждённый профиль даёт structured notice);
- EXIF/orientation применяется детерминированно до анализа/packing
  («orientation already applied», §11.1);
- grayscale/palette разворачиваются в RGBA8; 16-bit → 8-bit с фиксированным
  правилом округления; форматы без alpha получают A=255;
- любое отклонение от этой политики — новое решение, меняющее semantic image
  hash, и требует versioned hash algorithm tag (F1-03/F3-03).

Полноценный color management (ICC transforms, wide gamut) — вне v1.
