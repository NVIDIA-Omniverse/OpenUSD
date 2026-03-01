#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
install_dir="${1:-${repo_root}/_usd_install}"

python3 "${repo_root}/build_scripts/build_usd.py" "${install_dir}" \
  --embree \
  --openimageio \
  --usdview \
  --build-args USD,-DPYSIDEUICBINARY=/usr/bin/uic
