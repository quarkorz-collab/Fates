#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
project_dir=$(cd -- "$script_dir/.." && pwd -P)
output_dir="$project_dir"
python=${PYTHON:-python3}

usage() {
  printf '%s\n' \
    'Usage: bash frontend/build_web.sh [options]' \
    '' \
    'Build a standalone Linux fates-web executable (build on the target architecture).' \
    '' \
    'Options:' \
    '  --output DIR    Output directory (default: project root)' \
    '  --python PATH   Python with pip (default: $PYTHON or python3)' \
    '  -h, --help      Show this help'
}

while (($#)); do
  case "$1" in
    --output|--python)
      if (($# < 2)) || [[ -z "$2" || "$2" == --* ]]; then
        printf '%s requires a value\n' "$1" >&2
        exit 2
      fi
      if [[ "$1" == --output ]]; then output_dir=$2; else python=$2; fi
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      printf 'Unknown option: %s\n' "$1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ $(uname -s) != Linux ]]; then
  printf 'This script builds Linux binaries; run it on Linux (or WSL). Use build_web.ps1 on Windows.\n' >&2
  exit 2
fi
for tool in "$python" objdump sha256sum; do
  command -v "$tool" >/dev/null 2>&1 || { printf '%s was not found\n' "$tool" >&2; exit 1; }
done
"$python" -m pip --version

temporary_root=$(cd -- "${TMPDIR:-/tmp}" && pwd -P)
build_dir=$(mktemp -d "$temporary_root/fates-web.XXXXXX")
cleanup() {
  # Only remove the unique directory created above, never the caller's output.
  if [[ -n "$build_dir" && "$build_dir" == "$temporary_root"/fates-web.* && -d "$build_dir" ]]; then
    rm -rf -- "$build_dir"
  fi
}
trap cleanup EXIT

package_dir="$build_dir/packages"
distribution_dir="$build_dir/dist"
mkdir -p -- "$package_dir" "$distribution_dir" "$build_dir/work"
pip_args=(
  -m pip install --disable-pip-version-check --no-warn-script-location
  --target "$package_dir" --requirement "$script_dir/requirements-build.txt"
)
if ! "$python" "${pip_args[@]}"; then
  printf 'The configured pip index failed; retrying official PyPI.\n' >&2
  "$python" "${pip_args[@]}" --index-url 'https://pypi.org/simple'
fi

PYINSTALLER_CONFIG_DIR="$build_dir/cache" \
PYTHONPATH="$package_dir${PYTHONPATH:+:$PYTHONPATH}" "$python" -m PyInstaller \
  --noconfirm --clean --onefile --console \
  --name fates-web \
  --distpath "$distribution_dir" \
  --workpath "$build_dir/work" \
  --specpath "$build_dir" \
  --add-data "$script_dir/static:frontend_static" \
  "$script_dir/fates_web.py"

mkdir -p -- "$output_dir"
install -m 0755 -- "$distribution_dir/fates-web" "$output_dir/fates-web"
printf 'Web executable built: %s/fates-web\n' "$output_dir"
sha256sum -- "$output_dir/fates-web"
