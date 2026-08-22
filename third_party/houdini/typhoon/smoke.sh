#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
package_root="${script_dir}/build/package"
package_dir="${package_root}/typhoon"
output_path="${script_dir}/build/smoke.exr"
: "${HFS:?Set HFS to the Houdini installation used to build the package}"

test -f "${package_root}/typhoon.json"

# Validate only the package under build, even when another Typhoon package is
# enabled in the user's Houdini preferences.
export HOUDINI_PACKAGE_SKIP=1

"${package_dir}/husk-typhoon" --list-renderers 2>&1 \
    | grep -F HdEmbreeRendererPlugin

HOUDINI_PATH="${package_dir}:&" \
    "${HFS}/bin/hython" "${script_dir}/smoke_ui.py"

"${package_dir}/husk-typhoon" \
    -R HdEmbreeRendererPlugin \
    -o "${output_path}" \
    --headlight distant \
    "${script_dir}/smoke.usda"

test -s "${output_path}"
