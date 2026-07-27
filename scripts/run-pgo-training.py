#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys
import time


def main() -> int:
    parser = argparse.ArgumentParser(description="Run the shared Fates PGO workload set")
    parser.add_argument("--executable")
    parser.add_argument("--workloads", required=True)
    parser.add_argument("--training", choices=("quick", "balanced"), default="balanced")
    parser.add_argument(
        "--print-profile",
        action="store_true",
        help="print the workload profile name without running training",
    )
    args = parser.parse_args()

    definition = json.loads(Path(args.workloads).read_text(encoding="utf-8"))
    if args.print_profile:
        print(definition["profile"])
        return 0
    if not args.executable:
        parser.error("--executable is required unless --print-profile is used")

    executable = Path(args.executable).resolve()
    workloads = list(definition["base"])
    if args.training == "balanced":
        workloads.extend(definition["balanced_extra"])

    print(f"PGO profile: {definition['profile']} ({args.training})", flush=True)
    for workload in workloads:
        name = str(workload["name"])
        command = [str(executable), *(str(value) for value in workload["arguments"])]
        started = time.perf_counter()
        result = subprocess.run(
            command,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
        elapsed = time.perf_counter() - started
        if result.returncode != 0:
            print(f"PGO workload failed: {name} (exit {result.returncode})", file=sys.stderr)
            return result.returncode or 1
        print(f"  {name}: {elapsed:.2f}s", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
