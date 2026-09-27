#!/usr/bin/env python3
"""Full 77777/777777 regressions: root accuracy, live caching and determinism.

Independently checks every live/final equation at 80 decimal digits, using only
the standard library. Run with --bin path/to/fates[.exe].
"""
from __future__ import annotations

import argparse
import ast
from decimal import Decimal, localcontext
from functools import lru_cache
import json
import math
from pathlib import Path
import subprocess
import time


ARGUMENTS = [
    "77777", "--digits=", "--constants", "e,phi,gamma",
    "--ops", "+,-,*,/,^,neg,inv,sqrt,ln,exp,sin,tan,cos",
    "--max-cost", "16", "--beam", "20000", "--pairs", "50000000",
    "--value-bits", "48", "--value-prune", "exact", "--explore-pairs", "4",
    "--no-stop", "--pareto-slots", "2", "--inverse-beam", "96",
    "--inverse-budget", "3000000", "--equations", "--json",
]
SECOND_ARGUMENTS = [
    "777777", "--digits=", "--ops", "+,-,*,/,^,neg,inv,sqrt,ln,exp,sin,cos,tan",
    "--max-cost", "16", "--beam", "20000", "--pairs", "50000000",
    "--value-bits", "48", "--value-prune", "exact", "--explore-pairs", "4",
    "--no-stop", "--pareto-slots", "2", "--inverse-beam", "96",
    "--inverse-budget", "3000000", "--equations", "--json",
]
FALSE_PAIRS = {
    frozenset(("x+gamma/x^phi", "inv(gamma+inv(x)-gamma)")),
    frozenset(("x+inv(x)^sqrt(e)", "inv(gamma+inv(x)-gamma)")),
}

PI = Decimal("3.141592653589793238462643383279502884197169399375105820974944592307816406286208998628")
GAMMA = Decimal("0.577215664901532860606512090082402431042159335939923598805767234884867726777664670937")


def sincos(value: Decimal) -> tuple[Decimal, Decimal]:
    """Independent Taylor evaluation after high-precision range reduction."""
    value = value.remainder_near(2 * PI)
    square = -value * value
    sine, cosine = value, Decimal(1)
    sine_term, cosine_term = sine, cosine
    for n in range(1, 160):
        sine_term *= square / Decimal((2 * n) * (2 * n + 1))
        cosine_term *= square / Decimal((2 * n - 1) * (2 * n))
        sine += sine_term
        cosine += cosine_term
        if max(abs(sine_term), abs(cosine_term)) < Decimal("1e-78"):
            return sine, cosine
    raise AssertionError("Taylor evaluation did not converge")


def evaluate(node: ast.AST, variable: Decimal, constants: dict[str, Decimal]) -> tuple[Decimal, Decimal]:
    """Evaluate the rendered AST and its derivative; never execute source text."""
    if isinstance(node, ast.Expression):
        return evaluate(node.body, variable, constants)
    if isinstance(node, ast.Name):
        return (variable, Decimal(1)) if node.id == "x" else (constants[node.id], Decimal(0))
    if isinstance(node, ast.Constant) and type(node.value) in (int, float):
        return Decimal(str(node.value)), Decimal(0)
    if isinstance(node, ast.UnaryOp) and isinstance(node.op, (ast.UAdd, ast.USub)):
        value, derivative = evaluate(node.operand, variable, constants)
        return (-value, -derivative) if isinstance(node.op, ast.USub) else (value, derivative)
    if isinstance(node, ast.BinOp):
        a, da = evaluate(node.left, variable, constants)
        b, db = evaluate(node.right, variable, constants)
        if isinstance(node.op, ast.Add):
            return a + b, da + db
        if isinstance(node.op, ast.Sub):
            return a - b, da - db
        if isinstance(node.op, ast.Mult):
            return a * b, da * b + a * db
        if isinstance(node.op, ast.Div):
            return a / b, (da * b - a * db) / (b * b)
        if isinstance(node.op, ast.Pow):
            value = a ** b
            if a > 0:
                return value, value * (db * a.ln() + b * da / a)
            assert not db and b == b.to_integral_value(), (a, b)
            return value, b * a ** (b - 1) * da if b else Decimal(0)
    if isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and len(node.args) == 1 and not node.keywords:
        value, derivative = evaluate(node.args[0], variable, constants)
        name = node.func.id
        if name == "inv":
            return 1 / value, -derivative / (value * value)
        if name == "sqrt":
            root = value.sqrt()
            return root, Decimal(0) if not root and not derivative else derivative / (2 * root)
        if name == "ln":
            return value.ln(), derivative / value
        if name == "exp":
            result = value.exp()
            return result, result * derivative
        if name == "neg":
            return -value, -derivative
        if name in ("sin", "cos", "tan"):
            sine, cosine = sincos(value)
            if name == "sin":
                return sine, cosine * derivative
            if name == "cos":
                return cosine, -sine * derivative
            return sine / cosine, derivative / (cosine * cosine)
    raise AssertionError(f"Unsupported expression: {ast.dump(node)}")


@lru_cache(maxsize=None)
def verify_root(equation: str, reported: float) -> Decimal:
    """Prove a nearby numerical root, not just a small operand-scaled residual."""
    expressions = equation.replace("×", "*").replace("^", "**").split(" = ")
    assert len(expressions) == 2, equation
    left, right = (ast.parse(expression, mode="eval") for expression in expressions)
    with localcontext() as context:
        context.prec = 80
        constants = {"pi": PI, "e": Decimal(1).exp(), "phi": (1 + Decimal(5).sqrt()) / 2, "gamma": GAMMA}
        centre = Decimal.from_float(reported)
        root = centre
        tolerance = Decimal.from_float(math.ulp(reported)) * 64
        for _ in range(32):
            a, da = evaluate(left, root, constants)
            b, db = evaluate(right, root, constants)
            residual, derivative = a - b, da - db
            if residual == 0:
                break
            assert derivative != 0, (equation, "nonzero residual at a stationary point")
            correction = residual / derivative
            root -= correction
            assert abs(root - centre) <= tolerance, (equation, reported, root, "incorrect root position")
            if abs(correction) < Decimal("1e-60"):
                break
        else:
            raise AssertionError((equation, "high-precision Newton did not converge"))
        a, _ = evaluate(left, root, constants)
        b, _ = evaluate(right, root, constants)
        assert abs(a - b) <= Decimal("1e-60") * max(1, abs(a), abs(b)), (equation, a - b)
        assert abs(root - centre) <= tolerance, (equation, reported, root)
        for offset in (Decimal("-1e-5"), Decimal("1e-5")):
            a, _ = evaluate(left, root + offset, constants)
            b, _ = evaluate(right, root + offset, constants)
            assert a != b, (equation, "not an isolated root")
        return root


def check_rows(rows: list[dict], target: float) -> None:
    for row in rows:
        assert frozenset(row["equation"].split(" = ")) not in FALSE_PAIRS, row
        assert math.isfinite(row["estimated_root"]), row
        assert row["absolute_error"] == abs(row["estimated_root"] - target), row
        assert row["signed_error"] == row["estimated_root"] - target, row
        verify_root(row["equation"], row["estimated_root"])


def run(binary: Path, extra: list[str], arguments: list[str]) -> tuple[dict, list[dict], float]:
    start = time.perf_counter()
    process = subprocess.run([str(binary), *arguments, *extra],
                             capture_output=True, text=True, encoding="utf-8", timeout=180)
    wall = time.perf_counter() - start
    assert process.returncode == 0, process.stderr
    payload = json.loads(process.stdout)
    frames = [json.loads(line[len("[fates-live] "):]) for line in process.stderr.splitlines()
              if line.startswith("[fates-live] ")]
    check_rows(payload["results"], payload["target"])
    for frame in frames:
        check_rows(frame["results"], payload["target"])
        assert 0 <= frame["elapsed"] <= payload["stats"]["seconds"] + 1e-5
    assert payload["results"], "No valid equations left"
    assert 0 < payload["results"][0]["absolute_error"] < 2e-6
    stages = payload["stats"]["stage_seconds"]
    assert stages["equations"] > 0
    assert stages["deterministic"] + stages["equations"] <= payload["stats"]["seconds"] + 1e-5
    return payload, frames, wall


def stable(payload: dict) -> dict:
    return {**payload, "stats": {key: value for key, value in payload["stats"].items()
                                 if key not in ("threads", "seconds", "stage_seconds")}}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin", type=Path, required=True)
    parser.add_argument("--json-out", type=Path)
    options = parser.parse_args()
    binary = options.bin.resolve()
    reports = []
    for arguments in (ARGUMENTS, SECOND_ARGUMENTS):
        baseline, _, wall = run(binary, [], arguments)
        reports.append({"target": baseline["target"], "baseline": True, "wall_seconds": wall})
        for threads in (1, 4, 16):
            for live_top in (None, 3, 31):
                extra = ["--threads", str(threads)]
                if live_top is not None:
                    extra += ["--live", "--live-json", "--live-top", str(live_top)]
                payload, frames, wall = run(binary, extra, arguments)
                assert stable(payload) == stable(baseline), (threads, live_top)
                if live_top is not None:
                    assert frames and frames[-1]["cost"] == payload["generated_cost"]
                    assert len(frames[-1]["results"]) <= live_top
                else:
                    assert not frames
                reports.append({"target": payload["target"], "threads": threads, "live_top": live_top,
                                "wall_seconds": wall, "seconds": payload["stats"]["seconds"],
                                "results": len(payload["results"])})
                print(f"PASS target={payload['target']} threads={threads} live_top={live_top} wall={wall:.3f}s", flush=True)
        local, frames, wall = run(binary, ["--equation-quality", "local", "--live", "--live-json"], arguments)
        assert frames
        reports.append({"target": local["target"], "quality": "local", "wall_seconds": wall,
                        "results": len(local["results"])})
    result = {"passed": len(reports), "precision": 80, "roots_verified": verify_root.cache_info().currsize,
              "runs": reports}
    if options.json_out:
        options.json_out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"All {result['passed']} equation regressions passed.")


if __name__ == "__main__":
    main()
