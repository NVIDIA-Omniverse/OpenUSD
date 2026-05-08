#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
install_dir="${repo_root}/_usd_install"
extra_args=()

usage() {
  cat >&2 <<'EOF'
Usage: build_usd_embree.sh [install_dir] [build_usd.py options...]

Builds OpenUSD with hdEmbree, OpenImageIO, and usdview enabled.

Common options:
  --tests     Enable OpenUSD test targets (required for authoritative tests)
  -vv         Pass verbose build output to build_usd.py
  -h, --help  Show this help

Examples:
  ./build_scripts/build_usd_embree.sh
  ./build_scripts/build_usd_embree.sh --tests
  ./build_scripts/build_usd_embree.sh /path/to/install --tests -vv
EOF
}

if (($# > 0)); then
  if [[ "$1" == "-h" || "$1" == "--help" ]]; then
    usage
    exit 0
  fi
  if [[ "$1" != -* ]]; then
    install_dir="$1"
    shift
  fi
  extra_args=("$@")
fi

python3 "${repo_root}/build_scripts/build_usd.py" "${install_dir}" \
  --embree \
  --openimageio \
  --usdview \
  --build-args USD,-DPYSIDEUICBINARY=/usr/bin/uic \
  "${extra_args[@]}"
