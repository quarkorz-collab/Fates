#!/usr/bin/env sh
set -eu

if [ "$#" -gt 0 ]; then
  BIN="$1"
elif [ -x ./fates ]; then
  BIN=./fates
elif [ -x ./fates.exe ]; then
  BIN=./fates.exe
else
  echo "Fates executable not found (expected ./fates or ./fates.exe)" >&2
  exit 1
fi

version_output=$("$BIN" --version)
printf '%s\n' "$version_output" | grep -F 'Fates ' >/dev/null
printf '%s\n' "$version_output" | grep -F 'Finding Algebraic Targets via Expression Search' >/dev/null

compat_name=pries
case "$BIN" in
  *.exe) compat_name=pries.exe ;;
esac
compat_bin="$(dirname -- "$BIN")/$compat_name"
if [ -x "$compat_bin" ] && [ "$compat_bin" != "$BIN" ]; then
  "$compat_bin" --version | grep -F 'Fates ' >/dev/null
fi

"$BIN" --self-test
"$BIN" --args-file examples/24-point.args --no-stats \
  | grep -F '3×2/(1/4)' >/dev/null
"$BIN" --args-file examples/24-point.args --dry-run --json \
  | python3 -c 'import json, sys; cfg = json.load(sys.stdin); assert cfg["program"] == "Fates" and cfg["configuration_valid"] is True and cfg["target"] == 24 and cfg["symbol_count_rules"] == 4'
"$BIN" 1.4142135623730951 --digits 2 --constants none --ops sqrt --max-cost 2 --no-stats \
  | grep -F 'sqrt(2)'
"$BIN" 1.1752011936438014 --digits 1 --constants none --ops sinh --max-cost 3 --no-stats \
  | grep -F 'sinh(1)'
"$BIN" 1.772453850905516 --digits 2 --constants none --ops 'inv,gamma' --max-cost 4 --no-stats \
  | grep -F 'gamma(inv(2))'
"$BIN" 3 --digits 2 --constants none --ops gamma --max-cost 4 --equations \
  --mode nearest --results 10 --no-stats | grep -F 'gamma(x) = 2'
"$BIN" 3.141592653589793 --constants none --digits 123456789 --max-literal-len 3 \
  --max-integer 999 --ops / --max-cost 7 --no-stats | grep -F '355/113'
"$BIN" 0 --digits 1 --constants none --ops - --max-cost 3 --json --no-stats \
  | python3 -m json.tool >/dev/null
"$BIN" 625 --digits 25 --constants none --ops '^' --max-cost 5 --no-stats \
  | grep -F '25^2'
"$BIN" 1.4142135623730951 --digits 2 --constants none --ops sqrt --max-cost 2 \
  --live-top 2 --live-interval 0 --no-stats 2>&1 | grep -F '[live]' >/dev/null
"$BIN" 1.4142135623730951 --digits 2 --constants none --ops '^' --max-cost 5 \
  --equations --mode nearest --results 5 --no-stats | grep -F 'x^2 = 2'
"$BIN" 2 --digits '' --constants pi --ops '+,/' --max-cost 7 --mode nearest \
  --symbol-count pi=4 --no-stats | grep -F 'pi/pi+pi/pi'
"$BIN" -1 --digits 12 --max-literal-len 1 --constants none --ops - --max-cost 3 \
  --mode nearest --symbol-order 1,2 --no-stats | grep -F '1-2'
"$BIN" 1.4142135623730951 --digits 2 --constants none --ops sqrt --max-cost 2 \
  --mode nearest --error-range '(0,1)' --no-stats | grep -F '5.857864e-01' >/dev/null
"$BIN" 1.4142135623730951 --digits 2 --constants none --ops sqrt --max-cost 2 \
  --mode nearest --error-range 0 1 --no-stats | grep -F '5.857864e-01' >/dev/null
"$BIN" 1.5 --digits 12 --constants none --ops none --max-cost 1 \
  --mode nearest --error-range '(0,1)' --json --no-stats \
  | python3 -c 'import json, sys; rows = json.load(sys.stdin)["results"]; assert rows and all(0 < row["signed_error"] < 1 for row in rows)'
"$BIN" 1.5 --digits 12 --constants none --ops none --max-cost 1 \
  --mode nearest --error-range '(-1,0)' --json --no-stats \
  | python3 -c 'import json, sys; rows = json.load(sys.stdin)["results"]; assert rows and all(-1 < row["signed_error"] < 0 for row in rows)'
"$BIN" 1.4142135623730951 --digits 2 --constants none --ops '^' --max-cost 5 \
  --equations --json --no-stats | python3 -m json.tool >/dev/null
"$BIN" 9.89897948556636 --digits 23 --constants none --ops '+,sqrt,^' \
  --max-cost 8 --side-cost 4 --beam 50 --pairs 1000 --value-bits 48 \
  --deep-rounds 1 --deep-beam 100 --no-stats | grep -F '(sqrt(3)+sqrt(2))^2' >/dev/null
inverse_output=$("$BIN" 1.0218971486541166 --digits 2 --constants none --ops '+:5,sqrt' \
  --max-cost 6 --side-cost 4 --beam 50 --pairs 1000 \
  --inverse-depth 2 --inverse-beam 32 --inverse-budget 10000 --results 20 2>&1)
printf '%s\n' "$inverse_output" | grep -F 'sqrt(sqrt(sqrt(sqrt(sqrt(2)))))' >/dev/null
printf '%s\n' "$inverse_output" | grep -F 'strategy=mitm+inverse completed_cost=6 generated_cost=4' >/dev/null
genetic_output=$("$BIN" 9.89897948556636 --digits 23 --constants none --ops '+,sqrt,^' \
  --max-cost 8 --side-cost 4 --beam 50 --pairs 1000 --value-bits 48 \
  --genetic --genetic-population 4096 --genetic-generations 64 \
  --genetic-repair 0.5 --genetic-repair-depth 3 --results 20 2>&1)
printf '%s\n' "$genetic_output" | grep -F '(sqrt(3)+sqrt(2))^2' >/dev/null
printf '%s\n' "$genetic_output" | grep -E 'genetic_repairs_kept=[1-9][0-9]*' >/dev/null
