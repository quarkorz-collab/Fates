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
`release-balanced-v1` profile with `balanced` training; `quick` is only for
local build-script checks.

## Performance changes

Search optimizations must preserve the result list and the `attempted`,
`valid`, and `kept` counters for the same deterministic configuration. Changes
that intentionally alter the search space should document that behavior and
add a focused regression test.
