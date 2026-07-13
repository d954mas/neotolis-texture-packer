# Decision log

Записи решений, принятых вне master spec (открытые контракты §60 и
implementation-policy выборы). Формат: контекст, решение, статус, кто принял.

Статусы:

- `accepted` — действует; пересмотр только явным новым решением.
- `initial-policy` — первая политика по master spec §60/§52; закрепляется
  executable fixtures в указанном пакете и может быть уточнена без смены
  продуктовой модели.

| ID | Решение | Статус |
|---|---|---|
| [0001](0001-lua-5-5.md) | PUC Lua 5.5 для sandboxed handlers | accepted |
| [0002](0002-utf8proc-nfc.md) | utf8proc (vendored) для Unicode NFC | accepted |
| [0003](0003-stage-ordering.md) | Порядок стадий: ROADMAP строже плана; C0 = F0 | accepted |
| [0004](0004-pack-supersession.md) | Один running Pack + latest intent | initial-policy (C0-03) |
| [0005](0005-raster-color-policy.md) | sRGB byte-preserving decode, без ICC transforms | initial-policy (C0-04) |
| [0006](0006-windows-device-paths.md) | `\\?\` — прозрачный алиас (drive/UNC), прочие `\\?\`/`\\.\` → `path_device` | accepted |
| [0007](0007-f1-01-id-scope.md) | F1-01: ID на atlas/anim/target (source → F1-02), anim id/name split, fill-nil promote | accepted |
| [0008](0008-f1-02-source-kind-migration.md) | F1-02: tagged source schema v3 + `kind` при миграции v2 bare-string | accepted |
| [0009](0009-f1-03-sprite-id-selectors.md) | F1-03: derived sprite_id, `{source,key}`-keyed records + frames, lazy v3→v4 re-key, canonical selectors | accepted |
| [0010](0010-f2-01-typed-operation-engine.md) | F2-01: типизированный operation engine (catalog / apply / validate / encode / builders) | accepted |
| [0011](0011-f2-02-transactions-revision-dirty.md) | F2-02: атомарные транзакции (clone), revision, semantic dirty, idempotency, transaction JSON contract | accepted |
