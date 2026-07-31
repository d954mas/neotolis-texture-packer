---
name: packer-premerge-review
description: Run the expensive user-requested Texture Packer pre-merge gate with a full suite, six isolated Opus review lenses, verified findings, fixes, and an independent clean second pass.
disable-model-invocation: true
---

# Packer pre-merge review

Run only when the user invokes `/packer-premerge-review`. Remain the lead; keep
all reviewer work independent and evidence-based.

1. **Freeze the candidate**
   - Record merge base, candidate SHA, branch, and working-tree state.
   - Stop on unexpected tracked or untracked changes.
   - Store temporary coverage/findings only in `.context/review/`.

2. **Run the supported gate**

   ```bash
   cmake --preset native-debug
   cmake --build --preset native-debug
   ctest --preset native-debug

   cmake --preset native-release
   cmake --build --preset native-release
   ctest --preset native-release

   cmake --preset native-tests-debug
   cmake --build --preset native-tests-debug
   ctest --preset native-tests-debug
   ```

   Include other repository-supported static or boundary checks not already
   registered in ctest. Also cover candidate-applicable CI-only hard gates,
   including the deterministic pack-hash and GUI bench probes, locally or via
   their exact-candidate CI result. Record exact results against the frozen SHA.

3. **Launch fresh Opus reviewers**

   Give each `reviewer` the frozen base/candidate, scope, and exactly one lens:

   1. packing correctness, determinism, contracts, boundary cases;
   2. state ownership, resources, lifetime, error paths;
   3. GUI, session, Undo, async, persistence;
   4. memory, concurrency, portability, engine boundary;
   5. tests, CI, negative paths, regression protection;
   6. adversarial integration.

   Run independent lenses in available waves. Reviewers must not receive worker
   rationale, prior findings, other reviewer output, review history, or a claim
   that the candidate is nearly ready.

4. **Require finding evidence**

   Every defect must include `file:line`, a reachable failure scenario,
   evidence, `P0`–`P3` severity, and a verification method. Treat speculation as
   a question. Record reviewed surfaces and coverage gaps.

5. **Verify and synthesize**

   Check every finding against the code. Deduplicate symptoms, merge shared root
   causes, reject unproven claims, and build a six-lens coverage matrix.

6. **Resolve findings**

   - Fix P0/P1 or explicitly hand them to the user as merge blockers.
   - Fix P2 when it does not expand approved product scope.
   - Handle P3 according to risk and scope.
   - Add regression protection for confirmed defects when practical.

7. **Validate the delta**

   Review fixes through affected lenses, run focused checks, then rerun the full
   relevant suite. Freeze the new candidate SHA.

8. **Run a clean second pass**

   Launch fresh reviewers on the new SHA with no first-pass findings or history.
   Rebuild the coverage matrix independently.

9. **Converge by risk**

   If the clean pass finds a new P0/P1, fix it, determine why prior coverage
   missed it, repeat affected lenses, and repeat the full pass when the fix has
   broad blast radius. Do not impose an arbitrary iteration limit; decide from
   severity, root cause, and blast radius.

10. **Hand off and clean**

    Report frozen SHA/base, suite results, verified findings and dispositions,
    coverage gaps, residual risks, and merge blockers. After user acceptance,
    remove `.context/review/` artifacts.
