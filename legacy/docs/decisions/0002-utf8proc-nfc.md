# 0002 — utf8proc (vendored) для Unicode NFC нормализации source keys

**Дата:** 2026-07-12
**Статус:** accepted
**Принял:** lead (делегировано owner), после проверки engine
**Реализовано в:** `tp_srckey` (`packer/src/tp_srckey.c`; master spec §5.3, §59 item 8)

## Решение

Для NFC-нормализации persistent source-local keys вендорим utf8proc
(MIT, фиксированная версия) в наш репозиторий (`packer/deps/`);
точная версия и per-file checksums фиксируются в `packer/deps/utf8proc/VENDOR.md`.

## Контекст

Проверено: `external/neotolis-engine` умеет UTF-8 декодирование кодпоинтов
(font/text stack), но не содержит Unicode normalization (композиция/
декомпозиция требуют таблиц Unicode data). Системные API (WinAPI/ICU/
CoreFoundation) дают платформозависимое поведение и нарушают детерминизм
(§4.3). Собственная минимальная NFC-реализация — тихий риск некорректной
нормализации. utf8proc — маленькая проверенная C-библиотека (Julia,
PostgreSQL), MIT совместим.

Engine submodule не трогаем (hard invariant).
