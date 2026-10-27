#!/usr/bin/env bash
# Builds every Dockerfile in this directory and reports PASS/FAIL per
# image. Desktop matrix images (gcc-*, clang-*) and wasm run the full
# ctest suite as part of `docker build`, each at BOTH C++20 (this
# project's documented floor) and C++23 explicitly, in two separate RUN
# layers -- not just a single "request 23, let CMake cap it down" pass,
# since ConstexprCbor::Expected<T, E> resolves to a different backend at
# each standard (a small fallback at C++20, real std::expected at C++23)
# and this is the only way to prove both actually work. Embedded targets
# (esp32, stm32, riscv32) compile-check every file in tests/ and
# examples/ against the real cross-compiler (no link -- see the comment
# at the top of each embedded Dockerfile for why), also at both
# standards. A Dockerfile's build succeeding means BOTH standards passed
# for that image; if only one fails, `docker build`'s own output (surfaced
# in the tail of the captured log on failure, below) shows which RUN
# layer it was.
#
# Usage:
#   docker/build-matrix.sh              # build + report on everything
#   docker/build-matrix.sh gcc-13 clang-17 esp32   # only these
#   docker/build-matrix.sh --list        # list available targets, do nothing
set -u -o pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "${script_dir}/.." && pwd)

mapfile -t all_targets < <(cd "${script_dir}" && ls *.Dockerfile | sed 's/\.Dockerfile$//' | sort -V)

if [[ "${1:-}" == "--list" ]]; then
    printf '%s\n' "${all_targets[@]}"
    exit 0
fi

targets=("$@")
if [[ ${#targets[@]} -eq 0 ]]; then
    targets=("${all_targets[@]}")
fi

declare -a results=()
overall_status=0

for name in "${targets[@]}"; do
    dockerfile="${script_dir}/${name}.Dockerfile"
    if [[ ! -f "${dockerfile}" ]]; then
        echo "skip: no such Dockerfile: docker/${name}.Dockerfile" >&2
        results+=("${name}: SKIP (not found)")
        overall_status=1
        continue
    fi

    echo "=== building ${name} ==="
    log_file=$(mktemp)
    if docker build -f "${dockerfile}" -t "cbor-${name}" "${repo_root}" >"${log_file}" 2>&1; then
        results+=("${name}: PASS")
    else
        results+=("${name}: FAIL (see ${log_file})")
        overall_status=1
        tail -n 60 "${log_file}"
    fi
done

echo
echo "=== summary ==="
printf '%s\n' "${results[@]}"

exit "${overall_status}"
