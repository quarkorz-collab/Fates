#!/usr/bin/env python3
"""Search benchmark and recall harness for Fates.

Two independent measurements:

* ``speed``  - wall-clock medians for a fixed set of representative searches.
* ``recall`` - how much of a strong reference search's quality the default
  (bounded) search reproduces.  The reference is the same binary run with the
  full layered strategy and generous budgets, so this measures the bounded
  strategy's missed solutions rather than absolute completeness.

Usage
-----
    python tests/bench_search.py --bin build/fates.exe
    python tests/bench_search.py --bin build-new/fates.exe --compare build-base/fates.exe
    python tests/bench_search.py --bin build/fates.exe --only recall
"""

from __future__ import annotations

import argparse
import json
import shlex
import statistics
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

# --------------------------------------------------------------------------- #
# Workload definitions
# --------------------------------------------------------------------------- #

# Speed workloads: names map to argument lists appended after the target.
SPEED_WORKLOADS: list[tuple[str, list[str]]] = [
    ("mc10-default", ["520.82418", "--max-cost", "10", "--no-stop"]),
    ("mc12-default", ["520.82418", "--max-cost", "12", "--no-stop"]),
    ("mc14-default", ["520.82418", "--max-cost", "14", "--no-stop"]),
    ("mc16-default", ["520.82418", "--max-cost", "16", "--no-stop"]),
    ("mc14-beam8000", ["520.82418", "--max-cost", "14", "--beam", "8000", "--no-stop"]),
    ("mc12-wide-ops", ["1.2020569031595943", "--max-cost", "12", "--no-stop",
                       "--ops", "+,-,*,/,^,neg,inv,sqrt,cbrt,ln,exp,sin,cos,tan"]),
    ("mc11-pareto", ["2.5063", "--max-cost", "11", "--mode", "pareto",
                     "--pareto-slots", "3", "--no-stop"]),
    ("mc10-equations", ["21", "--max-cost", "10", "--equations",
                        "--equation-search", "wide", "--no-stop"]),
    ("mc12-deep", ["2.5063", "--max-cost", "12", "--deep-rounds", "1", "--no-stop"]),
    ("mc12-portfolio", ["520.82418", "--max-cost", "12", "--search-mode", "portfolio",
                        "--pslq-basis", "64", "--pslq-pairs", "256",
                        "--egraph-seeds", "128", "--egraph-rounds", "1",
                        "--egraph-nodes", "256", "--mcts-iterations", "512",
                        "--mcts-depth", "3", "--mcts-branching", "8", "--no-stop"]),
]

# Recall workloads. Each entry is (name, shared-args).  The candidate run uses
# the arguments as-is; the reference run adds REFERENCE_EXTRA.
RECALL_WORKLOADS: list[tuple[str, list[str]]] = [
    # Digit-only puzzles: exact hits are common, so a missed hit is unambiguous.
    ("sqrt-sum-6", ["3.7416573867739413", "--digits", "2345", "--max-literal-len", "1",
                    "--constants", "none", "--ops", "+,sqrt", "--max-cost", "10"]),
    ("ln-product", ["1.0986122886681098", "--digits", "123", "--max-literal-len", "1",
                    "--constants", "none", "--ops", "+,*,ln", "--max-cost", "10"]),
    ("exp-chain", ["4.113250378782927", "--digits", "12", "--max-literal-len", "1",
                   "--constants", "none", "--ops", "+,*,exp,sqrt", "--max-cost", "10"]),
    ("nested-sqrt", ["1.5537739740300374", "--digits", "23", "--max-literal-len", "1",
                     "--constants", "none", "--ops", "+,*,sqrt", "--max-cost", "11"]),
    ("inv-sum", ["0.6166666666666667", "--digits", "23456", "--max-literal-len", "1",
                 "--constants", "none", "--ops", "+,/,inv", "--max-cost", "11"]),
    # General searches: compare best achievable error.
    ("zeta3-mc10", ["1.2020569031595943", "--max-cost", "10"]),
    ("zeta3-mc12", ["1.2020569031595943", "--max-cost", "12"]),
    ("catalan-mc12", ["0.915965594177219", "--max-cost", "12"]),
    ("khinchin-mc12", ["2.6854520010653064", "--max-cost", "12"]),
    ("target-520-mc12", ["520.82418", "--max-cost", "12"]),
    ("gamma-mc12", ["0.5772156649015329", "--max-cost", "12"]),
    ("feig-mc12", ["4.669201609102990", "--max-cost", "12"]),
    ("trig-mc11", ["0.8414709848078965", "--max-cost", "11",
                   "--ops", "+,-,*,/,^,neg,inv,sqrt,ln,exp,sin,cos,tan"]),
    ("plastic-mc12", ["1.3247179572447460", "--max-cost", "12"]),
    ("golden-pow-mc12", ["11.09016994374947", "--max-cost", "12"]),
]

# The reference configuration: full layered enumeration plus every deterministic
# widening the program offers, with no early stop.
REFERENCE_EXTRA = [
    "--no-bidirectional", "--no-stop", "--beam", "20000", "--pairs", "40000000",
    "--results", "40", "--json",
]

CANDIDATE_EXTRA = ["--no-stop", "--results", "40", "--json"]


# --------------------------------------------------------------------------- #
# Runner helpers
# --------------------------------------------------------------------------- #


@dataclass
class RunResult:
    seconds: float
    payload: dict | None = None
    ok: bool = True
    stderr: str = ""


def run_fates(binary: Path, args: list[str], want_json: bool,
              timeout: float = 1800.0) -> RunResult:
    start = time.perf_counter()
    try:
        proc = subprocess.run([str(binary), *args], capture_output=True,
                              text=True, timeout=timeout, encoding="utf-8",
                              errors="replace")
    except subprocess.TimeoutExpired:
        return RunResult(timeout, None, False, "timeout")
    elapsed = time.perf_counter() - start
    if proc.returncode != 0:
        return RunResult(elapsed, None, False, proc.stderr.strip()[:400])
    payload = None
    if want_json:
        text = proc.stdout
        brace = text.find("{")
        if brace >= 0:
            try:
                payload = json.loads(text[brace:])
            except json.JSONDecodeError as error:
                return RunResult(elapsed, None, False, f"json: {error}")
    return RunResult(elapsed, payload, True, "")


def median_seconds(binary: Path, args: list[str], repeats: int) -> tuple[float, list[float]]:
    samples = []
    for _ in range(repeats):
        result = run_fates(binary, args, want_json=False)
        if not result.ok:
            return float("nan"), []
        samples.append(result.seconds)
    return statistics.median(samples), samples


# --------------------------------------------------------------------------- #
# Speed benchmark
# --------------------------------------------------------------------------- #


def bench_speed(binary: Path, repeats: int) -> dict[str, float]:
    out: dict[str, float] = {}
    for name, args in SPEED_WORKLOADS:
        seconds, _ = median_seconds(binary, [*args, "--no-stats", "--results", "5"], repeats)
        out[name] = seconds
        print(f"  {name:<22} {seconds:8.3f}s", flush=True)
    return out


def bench_speed_ab(primary: Path, other: Path,
                   repeats: int) -> tuple[dict[str, float], dict[str, float]]:
    """Interleaved A/B timing.

    Background load on a developer machine easily doubles the wall time of a
    parallel search, so the two binaries are timed alternately and compared on
    the fastest run of each: the minimum is the least contaminated sample
    available without a quiet machine.
    """
    left: dict[str, float] = {}
    right: dict[str, float] = {}
    print(f"  {'workload':<22} {'new':>9}   {'old':>9}  speedup")
    for name, args in SPEED_WORKLOADS:
        full = [*args, "--no-stats", "--results", "5"]
        left_samples: list[float] = []
        right_samples: list[float] = []
        for _ in range(repeats):
            for binary, samples in ((primary, left_samples), (other, right_samples)):
                result = run_fates(binary, full, want_json=False)
                if result.ok:
                    samples.append(result.seconds)
        left[name] = min(left_samples) if left_samples else float("nan")
        right[name] = min(right_samples) if right_samples else float("nan")
        speedup = right[name] / left[name] if left[name] else float("nan")
        print(f"  {name:<22} {left[name]:8.3f}s vs {right[name]:8.3f}s  {speedup:5.2f}x",
              flush=True)
    return left, right


# --------------------------------------------------------------------------- #
# Recall benchmark
# --------------------------------------------------------------------------- #


@dataclass
class RecallCase:
    name: str
    target: float = 0.0
    reference_error: float = float("inf")
    reference_cost: int = 0
    candidate_error: float = float("inf")
    candidate_cost: int = 0
    reference_seconds: float = 0.0
    candidate_seconds: float = 0.0
    notes: list[str] = field(default_factory=list)

    @property
    def missed(self) -> bool:
        # An exact reference hit may be reproduced through a different rounding
        # order, so compare against a few units in the last place rather than
        # against zero.  Non-exact references get a small relative slack.
        if self.reference_error == 0.0:
            return self.candidate_error > max(1e-15, 8.0 * 2.220446049250313e-16 * abs(self.target))
        return self.candidate_error > self.reference_error * 1.0000001

    @property
    def ratio(self) -> float:
        if self.candidate_error == 0.0:
            return 1.0
        if self.reference_error == 0.0:
            return float("inf")
        return self.candidate_error / self.reference_error


def best_of(payload: dict | None) -> tuple[float, int]:
    if not payload:
        return float("inf"), 0
    best_error = float("inf")
    best_cost = 0
    for row in payload.get("results", []):
        error = abs(float(row["absolute_error"]))
        cost = int(row["cost"])
        if error < best_error or (error == best_error and cost < best_cost):
            best_error = error
            best_cost = cost
    return best_error, best_cost


def bench_recall(binary: Path, reference_binary: Path,
                 cache: dict[str, tuple[float, int]] | None) -> list[RecallCase]:
    cases: list[RecallCase] = []
    for name, args in RECALL_WORKLOADS:
        case = RecallCase(name, float(args[0]))
        if cache is not None and name in cache:
            case.reference_error, case.reference_cost = cache[name]
        else:
            reference = run_fates(reference_binary, [*args, *REFERENCE_EXTRA], want_json=True)
            case.reference_seconds = reference.seconds
            if not reference.ok:
                case.notes.append(f"reference failed: {reference.stderr}")
            case.reference_error, case.reference_cost = best_of(reference.payload)
            if cache is not None:
                cache[name] = (case.reference_error, case.reference_cost)

        candidate = run_fates(binary, [*args, *CANDIDATE_EXTRA], want_json=True)
        case.candidate_seconds = candidate.seconds
        if not candidate.ok:
            case.notes.append(f"candidate failed: {candidate.stderr}")
        case.candidate_error, case.candidate_cost = best_of(candidate.payload)
        cases.append(case)
        flag = "MISS" if case.missed else "ok  "
        print(f"  {flag} {name:<20} ref={case.reference_error:.3e}@{case.reference_cost:<3}"
              f" got={case.candidate_error:.3e}@{case.candidate_cost:<3}"
              f" ({case.candidate_seconds:.2f}s vs ref {case.reference_seconds:.2f}s)",
              flush=True)
    return cases


# --------------------------------------------------------------------------- #
# Entry point
# --------------------------------------------------------------------------- #


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--bin", required=True, type=Path, help="binary under test")
    parser.add_argument("--compare", type=Path, default=None,
                        help="optional second binary for a speed A/B")
    parser.add_argument("--reference", type=Path, default=None,
                        help="binary used for the recall reference run (default: --bin)")
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--only", choices=["speed", "recall", "all"], default="all")
    parser.add_argument("--json-out", type=Path, default=None)
    parser.add_argument("--reference-cache", type=Path, default=None,
                        help="JSON file caching reference errors between runs")
    arguments = parser.parse_args()

    report: dict[str, object] = {}

    if arguments.only in ("speed", "all"):
        if arguments.compare:
            print(f"speed A/B: {arguments.bin} (new) vs {arguments.compare} (old)")
            primary, other = bench_speed_ab(arguments.bin, arguments.compare, arguments.repeats)
            report["speed"] = primary
            report["speed_compare"] = other
            ratios = [other[name] / primary[name] for name in primary
                      if primary[name] and primary[name] == primary[name]]
            if ratios:
                print(f"\n  geometric-mean speedup: "
                      f"{statistics.geometric_mean(ratios):.2f}x")
        else:
            print(f"speed: {arguments.bin}")
            report["speed"] = bench_speed(arguments.bin, arguments.repeats)

    if arguments.only in ("recall", "all"):
        reference_binary = arguments.reference or arguments.bin
        cache: dict[str, tuple[float, int]] | None = None
        if arguments.reference_cache:
            cache = {}
            if arguments.reference_cache.exists():
                raw = json.loads(arguments.reference_cache.read_text(encoding="utf-8"))
                cache = {key: (float(value[0]), int(value[1])) for key, value in raw.items()}
        print(f"\nrecall: {arguments.bin} (reference {reference_binary})")
        cases = bench_recall(arguments.bin, reference_binary, cache)
        if arguments.reference_cache and cache is not None:
            arguments.reference_cache.write_text(
                json.dumps({key: list(value) for key, value in cache.items()}, indent=2),
                encoding="utf-8")
        missed = [case for case in cases if case.missed]
        print(f"\nrecall: {len(cases) - len(missed)}/{len(cases)} matched the reference")
        for case in missed:
            print(f"  MISS {case.name}: reference {case.reference_error:.6e} "
                  f"(cost {case.reference_cost}) vs {case.candidate_error:.6e} "
                  f"(cost {case.candidate_cost})")
        report["recall"] = [
            {"name": case.name, "reference_error": case.reference_error,
             "reference_cost": case.reference_cost,
             "candidate_error": case.candidate_error,
             "candidate_cost": case.candidate_cost, "missed": case.missed}
            for case in cases
        ]

    if arguments.json_out:
        arguments.json_out.write_text(json.dumps(report, indent=2), encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
