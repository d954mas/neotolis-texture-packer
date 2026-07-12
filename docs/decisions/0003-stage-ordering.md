# 0003 — Порядок стадий: ROADMAP авторитетен, имя стадии спайков — C0

**Дата:** 2026-07-12
**Статус:** accepted
**Принял:** lead (делегировано owner)

## Решение

1. Стадия контрактных спайков называется `C0` везде (ROADMAP ранее называл её
   `F0`); ROADMAP переименован.
2. При расхождении в допустимом параллелизме между
   `docs/plans/master-spec-implementation-plan.md` и `docs/ROADMAP.md`
   действует более строгий порядок ROADMAP. Конкретно: B2 стартует после
   принятия gate F3 (соответствует master spec §54 Phase 2 → Phase 3 и
   corrected critical path ревью), а не параллельно F3.
3. Pack supersession policy решается спайком C0-03 (добавлена задача), а не
   впервые в F3-03.
4. Color-management policy (§60 item 7) получила собственный пакет C0-04 и
   является prerequisite B0-03/B1-01/F3-03.
