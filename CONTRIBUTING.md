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

WebUI changes must pass the standard-library server tests, command quoting tests,
and the HTTP smoke test with a native engine (use `build/Release/fates.exe` for
Visual Studio builds):

```bash
python -m unittest discover -s tests -p 'test_web*.py'
node tests/test_web_commands.cjs
node tests/test_web_numbers.cjs
node tests/test_web_constant_counts.cjs
python tests/check_web.py --bin build/fates
python tests/check_constant_count.py --bin build/fates
```

On Linux, build the standalone UI with
`bash frontend/build_web.sh --output build`, then run
`python tests/check_web.py --bin build/fates --web build/fates-web`.
On Windows, use `frontend/build_web.ps1 -OutputDirectory "$PWD/build/Release"` and pass
the corresponding `.exe` paths. The packaged check verifies bundled offline
assets, discovery from an unrelated working directory, CLI/Web result equality,
live JSON, cancellation, and POSIX shutdown. Linux release builds use Ubuntu
22.04 to avoid accidentally raising the glibc baseline; build on each target
architecture rather than attempting a PyInstaller cross-build.

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

For an intentional search-space change, compare stage medians and solution
quality while still checking each binary's repeat determinism:

```bash
python tests/bench_search_stages.py --bin build/fates --compare <baseline binary> \
  --allow-search-changes --workload sin-cos-completion --workload web-deep --repeats 3
python tests/check_unary_completion.py --bin build/fates --threads 1 4 16
python tests/check_completion_policy.py --bin build/fates
node tests/test_web_completion.cjs
```

Without `--allow-search-changes`, the stage harness still rejects any A/B
result or counter change. The unary-completion regression uses the full
reported operator set and budgets; it must find a near-exact result without
enabling `cos`, and results/counters must match across thread counts.

`check_completion_policy.py` covers auto/full/off at 1/4/16 threads, tiny and
automatic budgets, multiple deep rounds, sparse layers, custom operation
costs, symbol counts/order, and `--no-stop` after cheap hits or budget exhaustion.
For policy comparisons with offline 80-digit checking and Windows peak memory:

```bash
python tests/probe_completion_policy.py --bin build/fates --compare <baseline binary> \
  --manifest <frozen quality manifest> --modes auto full --repeats 3 --out <report.json>
```

Use identically optimized executables and interleaved repeated samples for
performance claims. Compare both the original pre-fix reference and full
enhancement, not merely the new off mode. Full is the compatibility policy;
auto intentionally changes extra candidate selection. When checking old/new
full output equivalence, strip only the newly added completion metadata and
diagnostic counters (in addition to timings and thread count), not old counters
or expression fields. Freeze any held-out targets before running them.

`bench_search.py --compare` alternates the two binaries and reports the fastest
run of each, which is what makes the numbers usable on a machine with
background load. The `recall` mode measures the bounded search against the same
binary's full layered search, so a change that trades quality for speed shows
up as a larger error ratio rather than as a faster benchmark.
