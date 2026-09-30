#!/usr/bin/env python3
"""Time search stages and verify identical results/counters in interleaved A/B runs.

Example:
    python tests/bench_search_stages.py --bin build-new/fates.exe \
        --compare build-base/fates.exe --workload web-deep --threads 16 \
        --repeats 3 --json-out artifacts/deep-comparison.json

Uses the standard benchmark workloads, plus the frontend's full deep preset.
Unlike timing alone, a changed search result or counter makes this check fail.
Use --allow-search-changes only for intentional coverage changes: each binary
must still be deterministic across repeats, and quality is reported alongside time.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import statistics
import subprocess
import time
from pathlib import Path

from bench_search import SPEED_WORKLOADS


WORKLOADS = dict(SPEED_WORKLOADS)
WORKLOADS["web-deep"] = [
    "520.82418", "--max-cost", "16", "--beam", "20000", "--pairs", "50000000",
    "--value-bits", "48", "--pareto-slots", "2", "--inverse-depth", "2",
    "--inverse-beam", "96", "--inverse-budget", "3000000", "--deep-rounds", "1",
    "--value-prune", "exact", "--explore-pairs", "4", "--no-stop",
]
WORKLOADS["sin-cos-completion"] = [
    "1.49700762325366447336826357056303398521376068430762187966875392253715",
    "--max-integer", "5", "--ops", "+,-,*,/,^,neg,inv,sqrt,ln,exp,sin,tan,atan,asin",
    "--max-cost", "20", "--beam", "20000", "--pairs", "50000000", "--value-bits", "48",
    "--value-prune", "exact", "--explore-pairs", "4", "--no-stop", "--pareto-slots", "2",
    "--inverse-depth", "2", "--inverse-beam", "96", "--inverse-budget", "3000000",
    "--deep-rounds", "1",
]
WORKLOADS["constants-deep-live"] = [
    "777777", "--error-range", "(0,inf)", "--digits=", "--constants", "pi,e,phi,gamma",
    "--ops", "+,-,*,/,^,neg,inv,sqrt,exp,sin,tan", "--max-cost", "16",
    "--beam", "20000", "--pairs", "50000000", "--value-bits", "48",
    "--value-prune", "exact", "--explore-pairs", "4", "--no-stop",
    "--pareto-slots", "2", "--inverse-depth", "2", "--inverse-beam", "96",
    "--inverse-budget", "3000000", "--deep-rounds", "1", "--live", "--live-json",
]
# Small differential regressions for round boundaries, constrained state keys,
# sparse layers, aggressive pruning and derivative-sensitive extra candidates.
WORKLOADS.update({
    "deep-multiround": ["123.456", "--max-cost", "11", "--beam", "384", "--pairs", "30000",
                        "--deep-rounds", "3", "--deep-beam", "128", "--deep-frontier", "256",
                        "--pareto-slots", "3", "--pareto-extra", "64", "--value-prune", "exact",
                        "--explore-pairs", "2", "--no-stop"],
    "deep-constraints": ["24", "--digits", "1234", "--max-literal-len", "1", "--constants", "none",
                         "--ops", "+,-,*,/,sqrt", "--max-cost", "9", "--beam", "300",
                         "--pairs", "20000", "--symbol-count", "1=1", "--symbol-count", "2=1",
                         "--symbol-count", "3=1", "--symbol-count", "4=1",
                         "--deep-rounds", "2", "--pareto-slots", "2", "--no-stop"],
    "deep-low-chunks": ["-1.234", "--max-cost", "10", "--beam", "64", "--pairs", "5000",
                        "--deep-rounds", "2", "--deep-beam", "8", "--inverse-neighbors", "1",
                        "--explore-pairs", "2", "--pareto-slots", "3", "--task-chunks", "1", "--no-stop"],
    "deep-sparse": ["17", "--digits", "2", "--constants", "none", "--ops", "+", "--max-cost", "12",
                    "--beam", "32", "--pairs", "1000", "--deep-rounds", "3", "--deep-beam", "1",
                    "--deep-frontier", "1", "--no-stop"],
    "equation-pareto": ["1.4142135623730951", "--equations", "--max-cost", "7", "--beam", "300",
                        "--pairs", "20000", "--pareto-slots", "3", "--no-stop"],
    "full-pareto": ["0.731", "--no-bidirectional", "--max-cost", "7", "--beam", "300",
                    "--pairs", "20000", "--pareto-slots", "3", "--value-prune", "exact",
                    "--explore-pairs", "1", "--no-stop"],
})


def stable_payload(payload: dict) -> dict:
    """Exclude only timing and worker count; all search content must match."""
    return {
        **payload,
        "stats": {key: value for key, value in payload["stats"].items()
                  if key not in {"seconds", "stage_seconds", "threads"}},
    }


def fingerprint(payload: dict) -> str:
    return hashlib.sha256(json.dumps(stable_payload(payload), sort_keys=True,
                                     ensure_ascii=True).encode("utf-8")).hexdigest()


def live_summary(stderr: str) -> dict:
    """Validate JSON frames and compare the final live state, not its timing."""
    prefix = "[fates-live] "
    frames = [json.loads(line[len(prefix):]) for line in stderr.splitlines()
              if line.startswith(prefix)]
    if not frames:
        raise RuntimeError("--live-json produced no live frames")
    previous_elapsed = -1.0
    for frame in frames:
        if (frame.get("type") != "top" or not isinstance(frame.get("results"), list)
                or not isinstance(frame.get("elapsed"), (int, float))
                or frame["elapsed"] < previous_elapsed):
            raise RuntimeError("invalid or out-of-order live JSON frame")
        previous_elapsed = frame["elapsed"]
    final = {key: value for key, value in frames[-1].items() if key != "elapsed"}
    digest = hashlib.sha256(json.dumps(final, sort_keys=True,
                                      ensure_ascii=True).encode("utf-8")).hexdigest()
    return {"frame_count": len(frames), "final": final, "fingerprint": digest}


def run(binary: Path, args: list[str], timeout: float, log_path: Path | None) -> dict:
    start = time.perf_counter()
    # When saving a report, stream verbose progress to a per-run log. This also
    # leaves useful evidence when a large search fails or reaches its timeout.
    command = [str(binary), *args, "--json"]
    if log_path is not None:
        command.append("--verbose")
    log = log_path.open("w", encoding="utf-8") if log_path is not None else None
    try:
        proc = subprocess.run(command, stdout=subprocess.PIPE,
                              stderr=log if log is not None else subprocess.PIPE,
                              text=True, encoding="utf-8", errors="replace", timeout=timeout)
    finally:
        if log is not None:
            log.close()
    elapsed = time.perf_counter() - start
    if proc.returncode:
        raise RuntimeError(f"search exited {proc.returncode}: {log_path or proc.stderr}")
    payload = json.loads(proc.stdout)
    if "results" not in payload or "stats" not in payload:
        raise RuntimeError("search did not return results and statistics")
    sample = {"wall_seconds": elapsed, "fingerprint": fingerprint(payload), "payload": payload}
    if "--live-json" in args:
        stderr = log_path.read_text(encoding="utf-8") if log_path is not None else proc.stderr
        sample["live"] = live_summary(stderr)
    return sample


def summarize(samples: list[dict]) -> dict:
    stages = samples[0]["payload"]["stats"]["stage_seconds"]
    return {
        "best_absolute_error": min((result.get("absolute_error", float("inf"))
                                    for result in samples[0]["payload"]["results"]), default=float("inf")),
        "median_wall_seconds": statistics.median(s["wall_seconds"] for s in samples),
        "min_wall_seconds": min(s["wall_seconds"] for s in samples),
        "median_stage_seconds": {
            stage: statistics.median(s["payload"]["stats"]["stage_seconds"][stage]
                                     for s in samples) for stage in stages
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin", required=True, type=Path)
    parser.add_argument("--compare", type=Path)
    parser.add_argument("--variant", action="append", default=[], metavar="LABEL=PATH",
                        help="additional binary for a round-robin comparison")
    parser.add_argument("--workload", action="append", choices=sorted(WORKLOADS))
    parser.add_argument("--threads", type=int, default=16)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--timeout", type=float, default=900)
    parser.add_argument("--json-out", type=Path)
    parser.add_argument("--allow-search-changes", action="store_true",
                        help="allow intentional A/B coverage changes; still require repeat determinism")
    args = parser.parse_args()
    if args.repeats < 1 or args.threads < 1 or args.timeout <= 0:
        parser.error("repeats, threads and timeout must be positive")
    binaries = {"new": args.bin.resolve(strict=True)}
    if args.compare:
        binaries["old"] = args.compare.resolve(strict=True)
    for variant in args.variant:
        label, separator, path = variant.partition("=")
        if not separator or not label.isidentifier() or label in binaries:
            parser.error("variants must have a unique identifier LABEL and a PATH: LABEL=PATH")
        binaries[label] = Path(path).resolve(strict=True)
    report = {"binaries": {key: str(value) for key, value in binaries.items()},
              "binary_sha256": {key: hashlib.sha256(value.read_bytes()).hexdigest()
                                for key, value in binaries.items()},
              "threads": args.threads, "repeats": args.repeats, "workloads": {}}
    if args.json_out:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)

    def save() -> None:
        if args.json_out:
            args.json_out.write_text(json.dumps(report, indent=2, ensure_ascii=False),
                                     encoding="utf-8")

    for name in args.workload or ["web-deep"]:
        command_args = [*WORKLOADS[name], "--threads", str(args.threads), "--results", "20"]
        entry = {"args": command_args, "samples": {key: [] for key in binaries}}
        report["workloads"][name] = entry
        expected = {}
        expected_live = {}
        for repeat in range(args.repeats):
            order = list(binaries)
            offset = repeat % len(order)
            order = order[offset:] + order[:offset]
            for label in order:
                print(f"{name}: {label} run {repeat + 1}/{args.repeats}", flush=True)
                log_path = (args.json_out.with_name(
                    f"{args.json_out.stem}-{name}-{label}-{repeat + 1}.log")
                    if args.json_out else None)
                try:
                    sample = run(binaries[label], command_args, args.timeout, log_path)
                except (RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
                    entry["error"] = str(error)
                    save()
                    print(f"FAIL: {error}", flush=True)
                    return 1
                entry["samples"][label].append(sample)
                expectation_key = label if args.allow_search_changes else "all"
                expected.setdefault(expectation_key, sample["fingerprint"])
                expected_live.setdefault(expectation_key, sample.get("live", {}).get("fingerprint"))
                if sample["fingerprint"] != expected[expectation_key]:
                    entry["error"] = f"results or counters changed: {label} run {repeat + 1}"
                    save()
                    print(f"FAIL: {entry['error']}", flush=True)
                    return 1
                if sample.get("live", {}).get("fingerprint") != expected_live[expectation_key]:
                    entry["error"] = f"final live results changed: {label} run {repeat + 1}"
                    save()
                    print(f"FAIL: {entry['error']}", flush=True)
                    return 1
                save()
                stages = sample["payload"]["stats"]["stage_seconds"]
                stage_text = ", ".join(f"{key}={value:.3f}s" for key, value in stages.items() if value)
                print(f"  {sample['wall_seconds']:.3f}s; {stage_text}", flush=True)
        entry["summary"] = {label: summarize(samples) for label, samples in entry["samples"].items()}
        entry["equivalent"] = len({sample["fingerprint"] for samples in entry["samples"].values()
                                   for sample in samples}) == 1
        if "old" in entry["summary"]:
            entry["speedup"] = (entry["summary"]["old"]["median_wall_seconds"] /
                                entry["summary"]["new"]["median_wall_seconds"])
            status = "results/counters identical" if entry["equivalent"] else "intentional search change"
            print(f"  median speedup: {entry['speedup']:.3f}x; {status}", flush=True)
            print("  best errors: " + ", ".join(f"{label}={summary['best_absolute_error']:.3g}"
                                               for label, summary in entry["summary"].items()), flush=True)
        save()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
