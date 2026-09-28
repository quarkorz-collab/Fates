#!/usr/bin/env python3
"""CLI integration checks for total occurrences across multiple named constants."""

import argparse
import json
from pathlib import Path
import re
import subprocess


NAME_PATTERN = re.compile(r"(?<![A-Za-z0-9_])(pi|e|phi|G)(?![A-Za-z0-9_])")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin", type=Path, required=True)
    executable = str(parser.parse_args().bin.resolve(strict=True))

    def run(*arguments, success=True):
        completed = subprocess.run([executable, *arguments], capture_output=True,
                                   encoding="utf-8", timeout=30)
        if success:
            assert completed.returncode == 0, (arguments, completed.stderr)
            return json.loads(completed.stdout)
        assert completed.returncode == 2 and "--constant-count" in completed.stderr, (
            arguments, completed.returncode, completed.stderr)

    def count(expression, names):
        return sum(name in names for name in NAME_PATTERN.findall(expression))

    def verify_results(arguments, groups, equations=False):
        payload = run(*arguments, "--json", "--no-stats", "--threads", "1", "--no-stop")
        assert payload["results"], (arguments, payload)
        for result in payload["results"]:
            expression = result["equation" if equations else "expression"]
            for names, minimum, maximum in groups:
                found = count(expression, names)
                assert found >= minimum and (maximum is None or found <= maximum), (
                    arguments, expression, names, found, minimum, maximum)
        return payload

    base = ["5.859874482048838", "--digits=", "--constants", "pi,e,phi",
            "--ops", "+,-,*,/,inv", "--max-cost", "7", "--beam", "300",
            "--pairs", "10000", "--results", "60", "--value-bits", "52"]
    grouped = [*base, "--constant-count", "pi,e=2:3",
               "--constant-count", "pi,phi=0:1", "--symbol-count", "pi=1"]
    groups = [({"pi", "e"}, 2, 3), ({"pi", "phi"}, 0, 1)]
    for exhaustive in ([], ["--no-bidirectional"]):
        payload = verify_results([*grouped, *exhaustive], groups)
        assert any(count(row["expression"], {"pi", "e"}) == 2 and
                   count(row["expression"], {"pi"}) == 1 for row in payload["results"]), payload
    one = run(*grouped, "--json", "--no-stats", "--threads", "1", "--no-stop")
    four = run(*grouped, "--json", "--no-stats", "--threads", "4", "--no-stop")
    assert one["results"] == four["results"], "Constant group results changed with thread count"
    assert {key: value for key, value in one.items() if key not in ("results", "stats")} == {
        key: value for key, value in four.items() if key not in ("results", "stats")
    }, "Constant group search metadata changed with thread count"
    print("PASS grouped totals in layered/MITM output, overlapping groups, individual limits and threads")

    no_restriction = run(*base, "--json", "--no-stats", "--threads", "1", "--no-stop")
    open_restriction = run(*base, "--constant-count", "pi,e=0:inf", "--json",
                           "--no-stats", "--threads", "1", "--no-stop")
    assert no_restriction["results"] == open_restriction["results"], (
        "Unrestricted group changed search output"
    )
    dry = run(*grouped, "--dry-run", "--json")
    assert dry["constant_count_rules"] == 2 and dry["symbol_count_rules"] == 1, dry
    print("PASS unrestricted group preserves results and dry-run exposes active group count")

    for option in ("pi,e=0", "pi,e=1:inf", "pi,e=2"):
        payload = verify_results([*base, "--constant-count", option],
                                 [({"pi", "e"}, 0 if option.endswith("=0") else
                                   1 if option.endswith("=1:inf") else 2,
                                   0 if option.endswith("=0") else
                                   2 if option.endswith("=2") else None)])
        assert payload["results"], option
    print("PASS zero upper bound, exact total and infinite upper bound")

    equation = ["-0.423310825130748", "--digits=", "--constants", "pi,e",
                "--ops", "+,-", "--max-cost", "6", "--beam", "300",
                "--pairs", "10000", "--equations", "--results", "40",
                "--constant-count", "pi,e=2:2"]
    equations = verify_results(equation, [({"pi", "e"}, 2, 2)], equations=True)
    assert any(count(row["equation"], {"pi"}) and count(row["equation"], {"e"})
               for row in equations["results"]), equations
    print("PASS equation search counts constants on both sides")

    custom = ["5.141592653589793", "--digits=", "--constants", "pi", "--constant", "G=2",
              "--ops", "+,-,*,/", "--max-cost", "5", "--constant-count", "pi,G=2"]
    custom_results = verify_results(custom, [({"pi", "G"}, 2, 2)])
    assert any(count(row["expression"], {"pi"}) and count(row["expression"], {"G"})
               for row in custom_results["results"]), custom_results
    print("PASS custom constants included in grouped total")

    for specs in (["pi=1"], ["pi,e=inf"], ["pi,e=2:1"], ["pi,pi=1"],
                  ["pi,e=1:2", "e,pi=3:4"], ["pi,e=9:inf"], ["pi,unknown=1"]):
        arguments = [*base]
        for spec in specs:
            arguments.extend(("--constant-count", spec))
        run(*arguments, "--dry-run", success=False)
    run(*base, "--constants", "pi", "--constant-count", "pi,e=1", "--dry-run", success=False)
    run(*base, "--symbol-order", "pi,e", "--constant-count", "pi,e=0:1",
        "--dry-run", success=False)
    print("PASS invalid groups, unenabled constants, impossible minima and order conflicts rejected")


if __name__ == "__main__":
    main()
