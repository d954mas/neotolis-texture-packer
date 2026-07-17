# Decision log

Записи решений, принятых вне master spec (открытые контракты §60 и
implementation-policy выборы). Формат: контекст, решение, статус, кто принял.

`docs/ntpacker-master-spec.md` остаётся единственным нормативным источником.
Решения 0010–0015 сохранены только как история реализации: они не создают
текущих требований и не должны исполняться как план. Для действующих правил
нужно обращаться к master spec и, для project schema, к решению 0016.

Статусы:

- `accepted` — действующее узкое решение, если master spec не заменяет его.
- `initial-policy` — первая политика по master spec §60/§52; закрепляется
  executable fixtures в указанном пакете и может быть уточнена без смены
  продуктовой модели.
- `historical` — запись о пройденной реализации; не является источником
  текущего контракта или плана работ.

| ID | Решение | Статус |
|---|---|---|
| [0001](0001-lua-5-5.md) | PUC Lua 5.5 для sandboxed handlers | accepted |
| [0002](0002-utf8proc-nfc.md) | utf8proc (vendored) для Unicode NFC | accepted |
| [0004](0004-pack-supersession.md) | Один running Pack + latest intent | initial-policy |
| [0005](0005-raster-color-policy.md) | sRGB byte-preserving decode, без ICC transforms | initial-policy |
| [0006](0006-windows-device-paths.md) | `\\?\` — прозрачный алиас (drive/UNC), прочие `\\?\`/`\\.\` → `path_device` | accepted |
| [0010](0010-f2-01-typed-operation-engine.md) | F2-01 implementation history: typed operation engine | historical |
| [0011](0011-f2-02-transactions-revision-dirty.md) | F2-02 implementation history: transactions, revision and dirty state | historical |
| [0012](0012-f2-03-semantic-diff-inverse.md) | F2-03 implementation history: semantic diff and snapshot oracle | historical |
| [0013](0013-f2-04-recovery-journal.md) | F2-04 implementation history: recovery journal | historical |
| [0014](0014-f2-05a-cli-transaction-cutover.md) | F2-05a implementation history: CLI transaction routing | historical |
| [0015](0015-f2-05b-gui-transaction-journal-cutover.md) | F2-05b implementation history: GUI/session cutover | historical |
| [0016](0016-canonical-project-schema-v5.md) | Только canonical project schema v5; legacy migrations и pending project states удалены | accepted |
| [0017](0017-durable-project-save-publication.md) | Durable sibling-temp project Save; post-publication uncertainty is a structured success notice | accepted |
| [0018](0018-fallible-builder-containment.md) | Private worker contains aborting/narrow-path engine builder until a fallible sink API exists | accepted |
