#!/usr/bin/env python3
"""Sweep search-shape parameters against the recall corpus.

Prints, for every parameter combination, how many corpus cases reach the
reference quality and how long the corpus takes.  Used to pick defaults that
trade beam width against window width at constant cost.
"""

from __future__ import annotations

import argparse
import itertools
import json
import statistics
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from bench_search import RECALL_WORKLOADS, best_of, run_fates  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin", required=True, type=Path)
    parser.add_argument("--reference-cache", required=True, type=Path)
    parser.add_argument("--beam", type=int, nargs="+", default=[3000])
    parser.add_argument("--neighbors", type=int, nargs="+", default=[5])
    parser.add_argument("--pairs", type=int, nargs="+", default=[0],
                        help="0 keeps the binary's default pair budget")
    parser.add_argument("--near", type=float, nargs="+", default=[])
    arguments = parser.parse_args()

    raw = json.loads(arguments.reference_cache.read_text(encoding="utf-8"))
    reference = {key: (float(value[0]), int(value[1])) for key, value in raw.items()}

    near_values = arguments.near or [None]
    floor = 1e-16
    print(f"{'beam':>6} {'nb':>4} {'pairs':>10} {'near':>6} {'ok':>7} {'x10':>5} "
          f"{'ratio':>8} {'seconds':>8}   worst")
    for beam, neighbors, pairs, near in itertools.product(
            arguments.beam, arguments.neighbors, arguments.pairs, near_values):
        extra = ["--beam", str(beam), "--inverse-neighbors", str(neighbors)]
        if pairs:
            extra += ["--pairs", str(pairs)]
        if near is not None:
            extra += ["--near-fraction", str(near)]
        matched = 0
        within_ten = 0
        ratios = []
        worst = ("", 0.0)
        started = time.perf_counter()
        for name, args in RECALL_WORKLOADS:
            if name not in reference:
                continue
            reference_error, _ = reference[name]
            result = run_fates(arguments.bin,
                               [*args, "--no-stop", "--results", "40", "--json", *extra],
                               want_json=True)
            error, _ = best_of(result.payload)
            target = abs(float(args[0]))
            tolerance = max(1e-15, 8.0 * 2.220446049250313e-16 * target)
            if reference_error == 0.0:
                ok = error <= tolerance
                ratio = 1.0 if ok else error / tolerance
            else:
                ok = error <= reference_error * 1.0000001
                ratio = max(error, floor) / max(reference_error, floor)
            matched += int(ok)
            within_ten += int(ratio <= 10.0)
            ratios.append(max(ratio, 1.0))
            if ratio > worst[1]:
                worst = (name, ratio)
        elapsed = time.perf_counter() - started
        near_text = "-" if near is None else f"{near:.2f}"
        geo = statistics.geometric_mean(ratios) if ratios else float("nan")
        print(f"{beam:>6} {neighbors:>4} {pairs:>10} {near_text:>6} {matched:>3}/{len(reference):<3} "
              f"{within_ten:>5} {geo:>8.2f} {elapsed:>8.2f}   {worst[0]}:{worst[1]:.0f}x",
              flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
