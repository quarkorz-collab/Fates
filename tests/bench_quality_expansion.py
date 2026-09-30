#!/usr/bin/env python3
"""Predeclared, non-cherry-picked old/new quality comparison (requires mpmath).

Writes a frozen manifest before searching, raw JSON for every run, 80-decimal
re-evaluations of all returned expressions, and resumable progress. Search runs
are sequential and A/B order alternates. No production source is modified.
"""

from __future__ import annotations

import argparse
import ast
from collections import Counter
from functools import lru_cache
import hashlib
import json
import math
from pathlib import Path
import random
import statistics
import subprocess
import time

import mpmath as mp

from bench_search_stages import fingerprint

mp.mp.dps = 80
OPS = "+,-,*,/,^,neg,inv,sqrt,ln,exp,sin,tan,atan,asin"
ORIGINAL = "1.49700762325366447336826357056303398521376068430762187966875392253715"


@lru_cache(maxsize=100000)
def evaluate(expression: str):
    """Evaluate only a small arithmetic AST, never Python eval or arbitrary code."""
    source = expression.replace("×", "*").replace("^", "**")
    names = {"pi": mp.pi, "e": mp.e, "phi": (1 + mp.sqrt(5)) / 2,
             "gamma": mp.euler}
    functions = {"ln": mp.log, "exp": mp.exp, "sqrt": mp.sqrt,
                 "sin": mp.sin, "cos": mp.cos, "tan": mp.tan,
                 "asin": mp.asin, "acos": mp.acos, "atan": mp.atan,
                 "inv": lambda x: 1 / x, "neg": lambda x: -x,
                 "cbrt": lambda x: mp.sign(x) * mp.root(abs(x), 3)}

    def visit(node):
        if isinstance(node, ast.Constant) and type(node.value) in (int, float):
            return mp.mpf(ast.get_source_segment(source, node))
        if isinstance(node, ast.Name) and node.id in names:
            return names[node.id]
        if isinstance(node, ast.UnaryOp):
            if isinstance(node.op, ast.USub):
                return -visit(node.operand)
            if isinstance(node.op, ast.UAdd):
                return visit(node.operand)
        if isinstance(node, ast.BinOp):
            a, b = visit(node.left), visit(node.right)
            if isinstance(node.op, ast.Add):
                return a + b
            if isinstance(node.op, ast.Sub):
                return a - b
            if isinstance(node.op, ast.Mult):
                return a * b
            if isinstance(node.op, ast.Div):
                return a / b
            if isinstance(node.op, ast.Pow):
                if abs(b) > 1000000:
                    raise ValueError("exponent exceeds independent evaluator safety bound")
                return mp.power(a, b)
        if (isinstance(node, ast.Call) and isinstance(node.func, ast.Name)
                and node.func.id in functions and len(node.args) == 1 and not node.keywords):
            argument = visit(node.args[0])
            if node.func.id == "exp" and abs(argument) > 1000000:
                raise ValueError("exp exceeds independent evaluator safety bound")
            return functions[node.func.id](argument)
        raise ValueError(f"unsupported syntax: {ast.dump(node)}")

    value = visit(ast.parse(source, mode="eval").body)
    if isinstance(value, mp.mpc) or not mp.isfinite(value):
        raise ValueError("expression is not finite and real at independent precision")
    return value


def decimal(value):
    return mp.nstr(value, 75)


def make_cases():
    groups = {
        "trig": [
            "cos(sin(1))+tan(ln(2))", "cos(sin(2))+tan(ln(2))",
            "cos(sin(3))+tan(ln(2))", "cos(sin(1))+tan(ln(3))",
            "cos(ln(2))+tan(ln(2))", "cos(sqrt(2))+tan(ln(2))",
            "cos(atan(2))+sin(ln(3))", "cos(sin(1))*tan(ln(2))",
            "cos(sin(1))/tan(ln(2))", "sin(cos(1))+atan(ln(2))",
            "sin(sqrt(2))+tan(ln(3))", "atan(sin(2))+asin(1/3)",
        ],
        "other_formula": [
            "exp(sqrt(2))+ln(3)", "ln(2+sqrt(3))+atan(2)",
            "sqrt(2+sqrt(3))+ln(2)", "exp(sin(1))+sqrt(ln(3))",
            "ln(2+exp(1))*sqrt(3)", "sqrt(2+ln(3))/atan(2)",
            "asin(1/3)+ln(2+sqrt(5))", "(2+sqrt(3))^(1/3)",
            "sqrt(ln(2+sqrt(3)))+exp(1/3)", "exp(atan(1/2))-sqrt(ln(2))",
            "ln(1+sin(2))+atan(sqrt(3))", "sin(ln(2+sqrt(3)))*exp(1/2)",
        ],
    }
    cases = []
    for group, expressions in groups.items():
        for index, expression in enumerate(expressions):
            cases.append({"id": f"{group}-{index+1:02}", "group": group,
                          "source": expression, "target": decimal(evaluate(expression))})
    cases[0]["target"] = ORIGINAL
    general = [
        ("zeta(3)", mp.zeta(3)), ("Catalan", mp.catalan),
        ("Khinchin", mp.khinchin), ("Euler gamma", mp.euler),
        ("plastic constant", mp.root((9 + mp.sqrt(69)) / 18, 3)
         + mp.root((9 - mp.sqrt(69)) / 18, 3)),
        ("zeta(5)", mp.zeta(5)), ("pi^pi", mp.pi ** mp.pi),
        ("e^e", mp.e ** mp.e), ("pi+e^sqrt(2)", mp.pi + mp.exp(mp.sqrt(2))),
        ("ln(pi)/ln(2)", mp.log(mp.pi) / mp.log(2)),
        ("ln(2)/ln(3)", mp.log(2) / mp.log(3)),
        ("sqrt(2)+sqrt(3)+sqrt(5)", mp.sqrt(2) + mp.sqrt(3) + mp.sqrt(5)),
    ]
    for index, (source, value) in enumerate(general):
        cases.append({"id": f"general-{index+1:02}", "group": "general",
                      "source": source, "target": decimal(value)})
    rng = random.Random(20260929)
    for index in range(12):
        value = mp.mpf(rng.randrange(10**16, 9 * 10**16)) / 10**16
        value *= mp.mpf(10) ** (index % 4 - 1)
        if index % 3 == 0:
            value = -value
        cases.append({"id": f"random-{index+1:02}", "group": "random",
                      "source": "fixed-seed decimal, seed=20260929", "target": decimal(value)})
    return cases


def profile_args(profile, threads):
    large = profile == "original-budget"
    return ["--max-integer", "5", "--ops", OPS, "--max-cost", "20" if large else "16",
            "--beam", "20000" if large else "4000", "--pairs", "50000000" if large else "4000000",
            "--value-bits", "48", "--value-prune", "exact", "--explore-pairs", "4",
            "--no-stop", "--pareto-slots", "2", "--inverse-depth", "2",
            "--inverse-beam", "96" if large else "64",
            "--inverse-budget", "3000000" if large else "300000", "--deep-rounds", "1",
            "--threads", str(threads), "--results", "100", "--json"]


def make_manifest(old, new, threads):
    # Frozen selection, unrelated to measured search outcomes.
    full_ids = {"trig-01", "trig-02", "trig-04", "trig-05",
                "other_formula-01", "other_formula-04", "other_formula-07", "other_formula-10",
                "general-01", "general-05", "random-03", "random-10"}
    cases = make_cases()
    workloads = []
    for profile in ("screen", "original-budget"):
        for case in cases:
            if profile == "original-budget" and case["id"] not in full_ids:
                continue
            workloads.append({**case, "key": f"{profile}--{case['id']}", "profile": profile,
                              "args": [case["target"], *profile_args(profile, threads)]})
    return {"schema": 1, "precision_digits": 80, "seed": 20260929, "threads": threads,
            "binaries": {"old": str(old), "new": str(new)},
            "sha256": {label: hashlib.sha256(binary.read_bytes()).hexdigest()
                       for label, binary in [("old", old), ("new", new)]},
            "quality_rule": "80-digit normalized error; material change >=2x when worse error >1e-14; hit <=1e-14",
            "repeat_rule": "Two extra A/B repeats for ALL material changes, plus screen general-02/random-01 and original-budget general-01/random-03 controls",
            "workloads": workloads}


def quality(payload, target_text):
    target = mp.mpf(target_text)
    scale = max(mp.mpf(1), abs(target))
    verified, failures = [], []
    for rank, result in enumerate(payload["results"], 1):
        try:
            value = evaluate(result["expression"])
            error = abs(value - target)
            verified.append({"expression": result["expression"], "cost": result["cost"],
                             "rank": rank, "engine_error": result["absolute_error"],
                             "mp_error": decimal(error), "mp_normalized_error": decimal(error / scale)})
        except (ValueError, ZeroDivisionError, OverflowError, SyntaxError) as exc:
            failures.append({"rank": rank, "expression": result["expression"], "error": str(exc)})
    if not verified:
        raise RuntimeError("no independently evaluable results")
    verified.sort(key=lambda item: (mp.mpf(item["mp_error"]), item["cost"]))
    return {"best": verified[0], "engine_best": min(payload["results"], key=lambda x: x["absolute_error"]),
            "verified": verified, "unverified": failures,
            "hits": {threshold: sum(mp.mpf(v["mp_normalized_error"]) <= mp.mpf(threshold)
                                    for v in verified) for threshold in ("1e-8", "1e-10", "1e-12", "1e-14", "1e-50")}}


def compare(old, new):
    a = mp.mpf(old["best"]["mp_normalized_error"])
    b = mp.mpf(new["best"]["mp_normalized_error"])
    status = "similar"
    if max(a, b) > mp.mpf("1e-14"):
        if b * 2 <= a:
            status = "improved"
        elif a * 2 <= b:
            status = "regressed"
    prior_expressions = {v["expression"] for v in old["verified"]}
    novel = [v for v in new["verified"] if v["expression"] not in prior_expressions]
    better = [v for v in novel if a > mp.mpf("1e-14")
              and mp.mpf(v["mp_normalized_error"]) * 2 <= a]
    return {"status": status, "old_hit": a <= mp.mpf("1e-14"), "new_hit": b <= mp.mpf("1e-14"),
            "old_best": old["best"], "new_best": new["best"],
            "novel_expressions_in_top100": len(novel),
            "novel_at_least_2x_better_than_old_best": better,
            "hit_count_changes": {key: new["hits"][key] - old["hits"][key] for key in old["hits"]}}


def run(binary, workload, sample_path, timeout):
    raw_path = sample_path.with_suffix(".raw.json")
    log_path = sample_path.with_suffix(".stderr.txt")
    if sample_path.exists():
        return json.loads(sample_path.read_text(encoding="utf-8"))
    started = time.perf_counter()
    process = subprocess.run([str(binary), *workload["args"]], capture_output=True,
                             encoding="utf-8", errors="strict", timeout=timeout)
    wall = time.perf_counter() - started
    raw_path.write_text(process.stdout, encoding="utf-8")
    log_path.write_text(process.stderr, encoding="utf-8")
    if process.returncode:
        raise RuntimeError(f"search returned {process.returncode}; see {log_path}")
    payload = json.loads(process.stdout)
    sample = {"wall_seconds": wall, "fingerprint": fingerprint(payload),
              "quality": quality(payload, workload["target"]), "stats": payload["stats"],
              "raw_json": str(raw_path)}
    sample_path.write_text(json.dumps(sample, indent=2, ensure_ascii=False), encoding="utf-8")
    return sample


def aggregate(entries):
    output = {}
    for profile in ("screen", "original-budget"):
        subset = [item for item in entries.values() if item["profile"] == profile]
        if not subset:
            continue
        summary = {"cases": len(subset), "status": dict(Counter(v["comparison"]["status"] for v in subset)),
                   "old_hits": sum(v["comparison"]["old_hit"] for v in subset),
                   "new_hits": sum(v["comparison"]["new_hit"] for v in subset),
                   "unverified": sum(len(v["samples"][label][0]["quality"]["unverified"])
                                     for v in subset for label in ("old", "new"))}
        ratios = []
        for item in subset:
            medians = {label: statistics.median(v["wall_seconds"] for v in item["samples"][label])
                       for label in ("old", "new")}
            ratios.append(medians["new"] / medians["old"])
        summary["geomean_time_ratio_new_over_old"] = statistics.geometric_mean(ratios)
        summary["by_group"] = {group: dict(Counter(v["comparison"]["status"] for v in subset if v["group"] == group))
                               for group in ("trig", "other_formula", "general", "random")}
        output[profile] = summary
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--old", type=Path, required=True)
    parser.add_argument("--new", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--threads", type=int, default=16)
    parser.add_argument("--timeout", type=float, default=240)
    parser.add_argument("--manifest-only", action="store_true")
    args = parser.parse_args()
    old, new = args.old.resolve(strict=True), args.new.resolve(strict=True)
    directory = args.out.resolve()
    directory.mkdir(parents=True, exist_ok=True)
    manifest = make_manifest(old, new, args.threads)
    manifest_path = directory / "manifest.json"
    if manifest_path.exists():
        if json.loads(manifest_path.read_text(encoding="utf-8")) != manifest:
            raise RuntimeError("manifest changed: use a new output directory")
    else:
        manifest_path.write_text(json.dumps(manifest, indent=2, ensure_ascii=False), encoding="utf-8")
    print(f"Frozen: {len(manifest['workloads'])} workload configurations, 48 unique targets", flush=True)
    if args.manifest_only:
        return 0
    entries = {}
    report = {"manifest": str(manifest_path), "complete": False, "entries": entries}

    def save():
        report["aggregate"] = aggregate(entries)
        (directory / "results.json").write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")

    for index, workload in enumerate(manifest["workloads"]):
        entry = {**workload, "samples": {"old": [], "new": []}}
        for label in (("old", "new") if index % 2 == 0 else ("new", "old")):
            print(f"[{index+1}/60] {workload['key']} {label} initial", flush=True)
            sample = run(old if label == "old" else new, workload,
                         directory / f"{workload['key']}--{label}--1.json", args.timeout)
            entry["samples"][label].append(sample)
        entry["comparison"] = compare(entry["samples"]["old"][0]["quality"], entry["samples"]["new"][0]["quality"])
        entries[workload["key"]] = entry
        comparison = entry["comparison"]
        print(f"  {comparison['status']}: {float(comparison['old_best']['mp_error']):.4g} -> "
              f"{float(comparison['new_best']['mp_error']):.4g}; "
              f"new better expressions={len(comparison['novel_at_least_2x_better_than_old_best'])}", flush=True)
        save()
    controls = {"screen--general-02", "screen--random-01", "original-budget--general-01", "original-budget--random-03"}
    selected = [entry for key, entry in entries.items()
                if entry["comparison"]["status"] != "similar" or key in controls]
    for repeat in (2, 3):
        for entry in selected:
            for label in (("new", "old") if repeat == 2 else ("old", "new")):
                print(f"REPEAT {repeat}/3 {entry['key']} {label}", flush=True)
                sample = run(old if label == "old" else new, entry,
                             directory / f"{entry['key']}--{label}--{repeat}.json", args.timeout)
                if sample["fingerprint"] != entry["samples"][label][0]["fingerprint"]:
                    raise RuntimeError(f"non-deterministic repeat: {entry['key']} {label}")
                entry["samples"][label].append(sample)
                save()
    report["complete"] = True
    report["repeated_configurations"] = len(selected)
    save()
    print(json.dumps(report["aggregate"], indent=2), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
