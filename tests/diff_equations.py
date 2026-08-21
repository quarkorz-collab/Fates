#!/usr/bin/env python3
"""Report equation-mode results that one binary emits and another rejects.

Used to review the effect of an equation quality change: every row printed under
`only in A` is an equation the A binary accepted and B dropped.
"""

from __future__ import annotations

import argparse
import json
import subprocess
from pathlib import Path

CASES = [
    ("21", ["--max-cost", "12", "--equation-search", "wide"]),
    ("3", ["--max-cost", "12", "--equation-search", "wide"]),
    ("1.4142135623730951", ["--max-cost", "12", "--equation-search", "wide"]),
    ("400", ["--max-cost", "13", "--equation-search", "wide"]),
    ("10000", ["--max-cost", "13", "--equation-search", "wide"]),
    ("1000000", ["--max-cost", "13", "--equation-search", "wide"]),
    ("100000", ["--max-cost", "13", "--equation-search", "wide"]),
    ("2.5063", ["--max-cost", "12", "--equation-search", "wide"]),
    ("0.5", ["--max-cost", "12", "--equation-search", "wide"]),
    ("1e9", ["--max-cost", "13", "--equation-search", "wide"]),
    ("21", ["--max-cost", "11", "--equation-quality", "local"]),
    ("10000", ["--max-cost", "12", "--equation-search", "exhaustive"]),
]

OPS = "+,-,*,/,^,neg,inv,sqrt,ln,exp"


def equations(binary: Path, target: str, extra: list[str]) -> list[tuple[str, float, int]]:
    command = [str(binary), target, "--equations", "--ops", OPS, "--no-stop",
               "--results", "40", "--json", *extra]
    proc = subprocess.run(command, capture_output=True, text=True, encoding="utf-8",
                          errors="replace")
    text = proc.stdout
    brace = text.find("{")
    if proc.returncode != 0 or brace < 0:
        return []
    payload = json.loads(text[brace:])
    return [(row["equation"], float(row["absolute_error"]), int(row["cost"]))
            for row in payload.get("results", [])]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--a", required=True, type=Path, help="binary A (usually the old one)")
    parser.add_argument("--b", required=True, type=Path, help="binary B (usually the new one)")
    arguments = parser.parse_args()

    dropped_total = 0
    added_total = 0
    for target, extra in CASES:
        rows_a = equations(arguments.a, target, extra)
        rows_b = equations(arguments.b, target, extra)
        set_a = {row[0] for row in rows_a}
        set_b = {row[0] for row in rows_b}
        dropped = [row for row in rows_a if row[0] not in set_b]
        added = [row for row in rows_b if row[0] not in set_a]
        dropped_total += len(dropped)
        added_total += len(added)
        label = f"{target} {' '.join(extra)}"
        print(f"\n{label}: A={len(rows_a)} B={len(rows_b)} "
              f"dropped={len(dropped)} added={len(added)}")
        for equation, error, cost in dropped:
            print(f"  only in A  cost={cost:<3} err={error:.3e}  {equation}")
        for equation, error, cost in added:
            print(f"  only in B  cost={cost:<3} err={error:.3e}  {equation}")
    print(f"\ntotal dropped {dropped_total}, total added {added_total}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
