# 0004 — Pack supersession: один running Pack + latest requested intent

**Дата:** 2026-07-12
**Статус:** initial-policy; реализуется и закрепляется production-тестами в F3-03
**Принял:** lead (делегировано owner); совпадает с рекомендацией
`docs/reviews/master-spec-review-2026-07-12.md` (finding 5)

## Решение

v1 session допускает одну running Pack job на session плюс один latest
requested intent (запрос во время работы Pack замещает предыдущий intent, а
не порождает параллельный job).

- superseded/более ранний job не может сам стать authoritative preview после
  завершения более нового запроса (master spec §10.3);
- все успешные результаты попадают в memory cache; explicit selection и
  Undo cache hit выбирают результат по `pack_input_hash`, не по времени
  завершения;
- ownership transfer отменяет только running Pack (§59 item 24).
