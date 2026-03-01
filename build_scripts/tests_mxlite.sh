#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${repo_root}/_usd_install/build/OpenUSD"
lib_dir="${repo_root}/_usd_install/lib"

if [[ ! -d "${build_dir}" ]]; then
    echo "Error: build directory not found: ${build_dir}" >&2
    exit 1
fi

cd "${build_dir}"

cmake --build . --target hdEmbree_headerfiles -- -j"$(nproc)"
cmake --build . --target testMxLite -- -j"$(nproc)"

LD_LIBRARY_PATH="${lib_dir}:${LD_LIBRARY_PATH:-}" \
    ./pxr/imaging/plugin/hdEmbree/testMxLite
