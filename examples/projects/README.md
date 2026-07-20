# Test and benchmark projects

`large-synthetic.ntpacker_project` is a deterministic, user-openable stress
project with 100 atlases, 10 file sources per atlas (1,000 memberships), and
one JSON export target per atlas. It deliberately reuses ten CC0 images from
`../showcase/animals/round/`; it does not duplicate binary assets.

The file is committed so GUI startup, project load/save, recovery, and manual
profiling all use the same realistic shape. Its structural and byte-level
contract is checked by `tp_large_project_contract`.

Regenerate it after an intentional canonical project-format change:

```bash
cmake --build --preset native-debug --target tp_generate_large_project
build/_cmake/native-debug/packer/tests/tp_generate_large_project \
  --write examples/projects/large-synthetic.ntpacker_project
ctest --preset native-debug -R tp_large_project_contract --output-on-failure
```

Do not edit the generated JSON by hand. Open it from this directory so its
relative showcase asset paths remain valid.

Run the production-path benchmark against the committed file (the last two
arguments keep a quick local run short):

```bash
cmake --build --preset native-release --target tp_bench_foundation
build/_cmake/native-release/packer/tests/tp_bench_foundation \
  --project examples/projects/large-synthetic.ntpacker_project \
  build/benchmark-scratch 5 20
```

The report includes p50/p95/p99/max for an in-memory transaction and for the
same transaction with the current file-backed journal attached. Timing is
advisory; correctness and byte accounting fail closed.

Profile atomic source-import batches separately (32 is the current GUI picker
limit; 256 and 1,000 exercise future machine-client workloads):

```bash
build/_cmake/native-release/packer/tests/tp_bench_foundation \
  --batch-scaling examples/projects/large-synthetic.ntpacker_project 3
```

This mode creates no project or image outputs. Each sample starts from a clone
of the committed project and reports the complete production transaction path.
