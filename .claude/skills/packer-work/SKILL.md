---
name: packer-work
description: Run Texture Packer implementation, debugging, refactoring, or documentation work with an adaptive trivial/normal/large workflow. Use the full approval-gated local-state workflow for large, ambiguous, architecture-sensitive, or long autonomous tasks.
---

# Packer work

Follow `AGENTS.md`. Scale ceremony to the task.

## Select the scale

- **Trivial:** one obvious local change with low blast radius. Work directly,
  skip `.context/`, run a focused check, and report evidence.
- **Normal:** clear bounded scope spanning a small surface. Orient, resolve
  material questions, state a short plan, implement, validate, and hand off.
  Use `.context/` only if resumability adds value.
- **Large/ambiguous:** uncertain product or architecture choices, several
  dependent packets, broad blast radius, or long autonomous execution. Use the
  full workflow below and do not edit production code before explicit approval.

Non-triviality alone does not require a subagent.

## Full workflow

1. **Orient**
   - Record branch, HEAD, status, user changes, and recent relevant history.
   - Read `AGENTS.md`, route through `docs/README.md`, then load only the
     task-relevant code, tests, and documentation.

2. **Research**
   - Investigate only questions that can change the solution.
   - Keep production code unchanged.
   - Use `deep-reasoner` only for a concrete independent question or when raw
     exploration would pollute lead context.

3. **Ask**
   - Ask all known material product, architecture, compatibility, scope,
     non-goal, and acceptance questions in one grouped block.

4. **Write the local change spec**
   - Put intent, current/expected behavior, invariants, acceptance criteria,
     risks, and non-goals in `.context/current.md`.
   - Put goal, phase, last verified SHA, next step, and blockers in
     `.context/STATE.md`.

5. **Plan and obtain approval**
   - Define a few bounded result-oriented packets, dependencies, checks, and
     possible stop points.
   - Show the exact file/change plan and wait for explicit user approval.

6. **Implement autonomously**
   - Produce one bounded result per packet with the smallest viable diff.
   - Use at most one `implementer` in an overlapping ownership area.
   - Parallelize only independent packets.
   - Run focused checks and leave a clean resumable point after each packet.

7. **Handle user absence**
   - Stop only the affected packet for a blocking, irreversible, or
     product-significant question; continue independent approved work.
   - For a reversible choice, take the conservative assumption and record it in
     `.context/questions.md`.
   - Record out-of-scope ideas as deferred; do not implement them.
   - Never treat absence as broader authority.

8. **Validate**
   - Run focused tests/static checks after each packet.
   - Verify acceptance criteria and review the changed surface.
   - Give confirmed defects regression protection when practical.
   - Do not launch the full pre-merge swarm during ordinary work.

9. **Request acceptance**
   - Report the result, validation evidence, assumptions/questions, remaining
     risks, and any durable documentation changes.
   - Wait for acceptance or corrections.

10. **Clean up**
    - Move only durable contract/architecture knowledge into `docs/`.
    - Encode repeatable defects in tests or validators.
    - Remove temporary current/research/review state and verify `git status`.

## Local state

Use `.context/` for one active task only:

```text
.context/
  STATE.md
  current.md
  questions.md
  research/
  review/
```

It is not a backlog or archive. Keep plans, research logs, review findings, and
session state out of Git. Use `questions.md` for blockers, reversible
assumptions, and deferred ideas; `research/` only for current-task research;
and `review/` only for current-review findings and coverage. Delete the task
contents after user acceptance.

Never invoke `/packer-premerge-review` automatically.
