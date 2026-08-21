#!/usr/bin/env python3
"""Dry-run the PGO workload profile against an optimized binary.

Every workload must exit zero, otherwise the PGO build script aborts halfway
through training.  The projected instrumented time uses the slowdown factor that
an instrumented `/GENPROFILE` build shows in practice.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import time
from pathlib import Path

INSTRUMENTED_SLOWDOWN = 60.0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin", required=True, type=Path)
    parser.add_argument("--workloads", type=Path,
                        default=Path(__file__).resolve().parent.parent / "scripts" / "pgo-workloads.json")
    parser.add_argument("--training", choices=("quick", "balanced"), default="balanced")
    arguments = parser.parse_args()

    definition = json.loads(arguments.workloads.read_text(encoding="utf-8"))
    workloads = list(definition["base"])
    if arguments.training == "balanced":
        workloads.extend(definition["balanced_extra"])

    print(f"profile {definition['profile']} ({arguments.training}), {len(workloads)} workloads")
    total = 0.0
    failures = 0
    for workload in workloads:
        command = [str(arguments.bin), *(str(value) for value in workload["arguments"])]
        started = time.perf_counter()
        result = subprocess.run(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        elapsed = time.perf_counter() - started
        total += elapsed
        status = "ok  " if result.returncode == 0 else "FAIL"
        if result.returncode != 0:
            failures += 1
        print(f"  {status} {workload['name']:<42} {elapsed:7.3f}s exit={result.returncode}")

    print(f"\n  optimized total {total:.2f}s"
          f" -> projected instrumented ~{total * INSTRUMENTED_SLOWDOWN / 60.0:.1f} min")
    if failures:
        print(f"  {failures} workload(s) would abort the PGO build")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
