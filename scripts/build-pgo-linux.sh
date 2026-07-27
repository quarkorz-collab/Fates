#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project_dir=$(cd -- "$script_dir/.." && pwd)

output_dir=""
training="balanced"
enable_avx2=0
keep_build=0

usage() {
  cat <<'EOF'
Usage: scripts/build-pgo-linux.sh [options]

Options:
  --output DIR       Output directory
  --training MODE    quick or balanced (default: balanced)
  --enable-avx2      Require AVX2 and enable AVX2 code generation
  --keep-build       Keep the temporary profile directory
  -h, --help         Show this help
EOF
}

while (($#)); do
  case "$1" in
    --output)
      output_dir=$2
      shift 2
      ;;
    --training)
      training=$2
      shift 2
      ;;
    --enable-avx2)
      enable_avx2=1
      shift
      ;;
    --keep-build)
      keep_build=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ "$training" != "quick" && "$training" != "balanced" ]]; then
  echo "--training must be quick or balanced" >&2
  exit 2
fi

if [[ -z "$output_dir" ]]; then
  suffix="linux-x64-pgo"
  if ((enable_avx2)); then
    suffix+="-avx2"
  fi
  output_dir="$project_dir/artifacts/$suffix"
fi

cxx=${CXX:-g++}
python=${PYTHON:-python3}
workload_path="$script_dir/pgo-workloads.json"
command -v "$cxx" >/dev/null 2>&1 || { echo "$cxx was not found" >&2; exit 1; }
command -v "$python" >/dev/null 2>&1 || { echo "$python was not found" >&2; exit 1; }
command -v sha256sum >/dev/null 2>&1 || { echo "sha256sum was not found" >&2; exit 1; }

if ((enable_avx2)) && [[ $(uname -m) != "x86_64" ]]; then
  echo "--enable-avx2 requires an x86-64 build host" >&2
  exit 1
fi

temporary_root=${TMPDIR:-/tmp}
build_dir=$(mktemp -d "$temporary_root/fates-pgo.XXXXXX")
cleanup() {
  if ((keep_build)); then
    echo "PGO build directory kept at $build_dir"
  else
    rm -rf -- "$build_dir"
  fi
}
trap cleanup EXIT

profile_dir="$build_dir/profile"
object_path="$build_dir/fates.o"
training_executable="$build_dir/fates-train"
optimized_executable="$output_dir/fates"
mkdir -p -- "$profile_dir" "$output_dir/licenses"

common_flags=(
  -O3 -DNDEBUG -std=c++20 -Wall -Wextra -Wpedantic -flto -pthread
)
arch_flags=()
if ((enable_avx2)); then
  arch_flags+=(-mavx2 -mtune=generic)
fi
generate_flags=(-fprofile-generate="$profile_dir" -fprofile-update=atomic)
use_flags=(-fprofile-use="$profile_dir" -fprofile-correction -fprofile-partial-training)

echo "==> Compile instrumented object"
"$cxx" "${common_flags[@]}" "${arch_flags[@]}" "${generate_flags[@]}" \
  -c "$project_dir/src/fates.cpp" -o "$object_path"

echo "==> Link instrumented executable"
"$cxx" "${common_flags[@]}" "${arch_flags[@]}" "${generate_flags[@]}" \
  "$object_path" -o "$training_executable"

echo "==> Run shared PGO workloads"
"$python" "$script_dir/run-pgo-training.py" \
  --executable "$training_executable" \
  --workloads "$workload_path" \
  --training "$training"

if ! find "$profile_dir" -type f -name '*.gcda' -print -quit | grep -q .; then
  echo "No GCC profile data was generated" >&2
  exit 1
fi

echo "==> Compile profile-guided object"
"$cxx" "${common_flags[@]}" "${arch_flags[@]}" "${use_flags[@]}" \
  -c "$project_dir/src/fates.cpp" -o "$object_path"

echo "==> Link profile-guided executable"
"$cxx" "${common_flags[@]}" "${arch_flags[@]}" "${use_flags[@]}" \
  "$object_path" -o "$optimized_executable"

"$optimized_executable" --self-test >/dev/null 2>&1
cp -- "$optimized_executable" "$output_dir/pries"
cp -- "$project_dir/README.md" "$project_dir/LICENSE" \
  "$project_dir/THIRD_PARTY_NOTICES.md" "$output_dir/"
cp -- "$project_dir/third_party/unordered_dense/LICENSE" \
  "$output_dir/licenses/unordered_dense-LICENSE"
cp -- "$project_dir/frontend/static/vendor/katex/LICENSE" \
  "$output_dir/licenses/KaTeX-LICENSE"

if command -v strip >/dev/null 2>&1; then
  strip --strip-unneeded "$optimized_executable" "$output_dir/pries"
fi

compiler_version=$("$cxx" --version | head -n 1)
profile_name=$("$python" "$script_dir/run-pgo-training.py" \
  --workloads "$workload_path" --print-profile)
workload_hash=$(sha256sum "$workload_path" | awk '{print $1}')
cat >"$output_dir/build-info.txt" <<EOF
Fates 1.0
toolchain=$compiler_version
pgo_training=$training
pgo_profile=$profile_name
pgo_workloads_sha256=$workload_hash
avx2=$enable_avx2
EOF

(cd "$output_dir" && \
  find . -type f ! -name SHA256SUMS.txt -print | LC_ALL=C sort | \
  while IFS= read -r file; do sha256sum "$file"; done >SHA256SUMS.txt)
echo "PGO build completed: $output_dir"
