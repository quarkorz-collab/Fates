#!/usr/bin/env python3
"""Regress the pruned sine/cosine path with the originally reported budgets."""

import argparse
import json
import subprocess
from pathlib import Path

from bench_search_stages import fingerprint


TARGET = "1.49700762325366447336826357056303398521376068430762187966875392253715"
ARGUMENTS = [
    TARGET, "--max-integer", "5", "--ops", "+,-,*,/,^,neg,inv,sqrt,ln,exp,sin,tan,atan,asin",
    "--max-cost", "20", "--beam", "20000", "--pairs", "50000000", "--value-bits", "48",
    "--value-prune", "exact", "--explore-pairs", "4", "--no-stop", "--pareto-slots", "2",
    "--inverse-depth", "2", "--inverse-beam", "96", "--inverse-budget", "3000000",
    "--deep-rounds", "1", "--json",
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin", required=True, type=Path)
    parser.add_argument("--threads", type=int, nargs="+", default=[1, 4, 16])
    parser.add_argument("--json-out", type=Path)
    args = parser.parse_args()
    binary = args.bin.resolve(strict=True)
    samples = []
    expected = None
    for threads in args.threads:
        process = subprocess.run([str(binary), *ARGUMENTS, "--threads", str(threads)],
                                 capture_output=True, text=True, encoding="utf-8", timeout=600)
        if process.returncode:
            raise RuntimeError(process.stderr)
        payload = json.loads(process.stdout)
        samples.append(payload)
        if args.json_out:
            args.json_out.parent.mkdir(parents=True, exist_ok=True)
            args.json_out.write_text(json.dumps(samples, indent=2, ensure_ascii=False), encoding="utf-8")
        best = payload["results"][0]
        if best["absolute_error"] > 1e-14:
            raise AssertionError(f"missed unary completion: {best}")
        if "cos(" in best["expression"] or best["cost"] > 15:
            raise AssertionError(f"used a disabled operator or missed the cost-15 path: {best}")
        current = fingerprint(payload)
        if expected is not None and expected != current:
            raise AssertionError("results or search counters depend on thread count")
        expected = current
        print(f"threads={threads}: error={best['absolute_error']:.3g} cost={best['cost']} "
              f"time={payload['stats']['seconds']:.3f}s {best['expression']}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
