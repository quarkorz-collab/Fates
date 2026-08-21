# Contributing

## Reporting a problem

Please include the Fates version, operating system, CPU model, complete command
line, expected behavior, actual output, and whether the issue is reproducible
with a fixed `--threads` value.

For performance reports, include at least three runs and compare medians. Keep
the target and every search parameter identical.

## Building and testing

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build --build-config Release --output-on-failure
```

On Linux or macOS, run the command-line regression suite as well:

```bash
tests/smoke.sh ./build/fates
```

Windows PGO builds can be reproduced with:

```powershell
.\scripts\build-pgo-windows.ps1 -EnableAVX2 -Training balanced
```

Linux GCC PGO builds use the same workload profile:

```bash
bash scripts/build-pgo-linux.sh --enable-avx2 --training balanced
```

Both scripts read `scripts/pgo-workloads.json`. Release PGO builds must use the
`release-balanced-v3` profile with `balanced` training; `quick` is only for
local build-script checks and covers the `base` set alone.

When you add or change a training workload, bump the profile name and dry-run the
set first — the build scripts abort on the first workload that exits non-zero,
after the instrumented build is already done:

```bash
python tests/check_pgo_workloads.py --bin build/fates
```

Size a training workload like the runs it is meant to represent, and treat a
wider training set as a coverage change rather than an optimization: going from
six to sixteen workloads left the benchmark suite unchanged (every workload
within 5%, most within 2%) while giving real profile data to stages that were
previously left to static heuristics. Measure before and after either way — a
profile change can move an individual path by a few percent in either direction.

## Performance changes

Search optimizations must preserve the result list and the `attempted`,
`valid`, and `kept` counters for the same deterministic configuration. Changes
that intentionally alter the search space should document that behavior and
add a focused regression test.

Every configuration must still produce identical output for any `--threads`
value. The archive merge is sharded by state key with a compile-time shard
count and each shard consumes task output in task order, so keep new parallel
stages independent of the worker count in the same way.

Report search performance and missed solutions with the two harnesses in
`tests/`, not with single wall-clock samples:

```bash
python tests/bench_search.py --bin build/fates --compare <baseline binary>
python tests/bench_search.py --bin build/fates --only recall \
  --reference-cache artifacts/recall-reference.json
```

`bench_search.py --compare` alternates the two binaries and reports the fastest
run of each, which is what makes the numbers usable on a machine with
background load. The `recall` mode measures the bounded search against the same
binary's full layered search, so a change that trades quality for speed shows
up as a larger error ratio rather than as a faster benchmark.
