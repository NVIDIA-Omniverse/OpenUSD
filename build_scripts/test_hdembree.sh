#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
install_root="${repo_root}/_usd_install"
mode="materialxcpp"
allow_stale=0
build_first=1
ctest_filter="^testUsdImagingGLHdEmbree$"
smoke_output="/tmp/hdembree-basicdrawing-smoke.png"

usage() {
    cat >&2 <<'EOF'
Usage: test_hdembree.sh [mode] [options]

Modes:
  materialxcpp  Build and run the CPU MaterialXCpp tests (default)
  smoke         Run a low-sample testUsdImagingGLBasicDrawing smoke render
  ctest         Run hdEmbree CTest image tests selected by --ctest-filter

Options:
  --install DIR          Use a non-default USD install root
  --allow-stale          Allow running an existing test binary when tests are off
  --no-build             Do not build before running materialxcpp
  --ctest-filter REGEX   CTest regex for ctest mode
  --output PATH          Output PNG path for smoke mode
  -h, --help             Show this help

Notes:
  Rebuild with ./build_scripts/build_usd_embree.sh --tests before using
  materialxcpp or CTest as authoritative validation.
  Graphics-based smoke/CTest modes may need to run outside the Codex sandbox.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        materialxcpp|smoke|ctest)
            mode="$1"
            shift
            ;;
        --install)
            install_root="$2"
            shift 2
            ;;
        --allow-stale)
            allow_stale=1
            shift
            ;;
        --no-build)
            build_first=0
            shift
            ;;
        --ctest-filter)
            ctest_filter="$2"
            shift 2
            ;;
        --output)
            smoke_output="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Error: unknown argument: $1" >&2
            usage
            exit 2
            ;;
    esac
done

build_dir="${install_root}/build/OpenUSD"
cache_file="${build_dir}/CMakeCache.txt"

if [[ ! -d "${build_dir}" ]]; then
    echo "Error: build directory not found: ${build_dir}" >&2
    echo "Run ./build_scripts/build_usd_embree.sh --tests first." >&2
    exit 1
fi

export PATH="${install_root}/bin${PATH:+:${PATH}}"
export PYTHONPATH="${install_root}/lib/python${PYTHONPATH:+:${PYTHONPATH}}"
export LD_LIBRARY_PATH="${install_root}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
export PXR_PLUGINPATH_NAME="${install_root}/lib/usd:${install_root}/plugin/usd${PXR_PLUGINPATH_NAME:+:${PXR_PLUGINPATH_NAME}}"

jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)"

cache_value() {
    local key="$1"
    [[ -f "${cache_file}" ]] || return 0
    sed -n "s/^${key}:[^=]*=//p" "${cache_file}" | tail -n 1
}

require_tests_enabled() {
    local build_tests
    build_tests="$(cache_value PXR_BUILD_TESTS)"
    if [[ "${build_tests}" != "ON" && "${allow_stale}" -eq 0 ]]; then
        echo "Error: PXR_BUILD_TESTS is '${build_tests:-unset}' in ${cache_file}." >&2
        echo "Rebuild with: ./build_scripts/build_usd_embree.sh --tests" >&2
        echo "Use --allow-stale only for non-authoritative local checks." >&2
        exit 1
    fi
}

run_materialxcpp() {
    require_tests_enabled

    local exe="${build_dir}/pxr/imaging/plugin/hdEmbree/testMaterialXCpp"
    local build_tests
    build_tests="$(cache_value PXR_BUILD_TESTS)"

    if [[ "${build_first}" -eq 1 && "${build_tests}" == "ON" ]]; then
        cmake --build "${build_dir}" --target hdEmbree_headerfiles -- -j"${jobs}"
        if ! cmake --build "${build_dir}" --target testMaterialXCpp -- -j"${jobs}"; then
            echo "Error: could not build testMaterialXCpp." >&2
            echo "Check that this install was configured with --tests." >&2
            exit 1
        fi
    elif [[ "${build_first}" -eq 1 && "${allow_stale}" -eq 1 ]]; then
        echo "Warning: PXR_BUILD_TESTS is not ON; running existing testMaterialXCpp without rebuilding." >&2
    fi

    if [[ ! -x "${exe}" ]]; then
        echo "Error: test binary not found: ${exe}" >&2
        exit 1
    fi

    "${exe}"
}

run_smoke() {
    local exe="${install_root}/tests/testUsdImagingGLBasicDrawing"
    local stage="${repo_root}/pxr/usdImaging/usdImagingGL/testenv/testUsdImagingGLHdEmbree/primitives.usda"

    if [[ ! -x "${exe}" ]]; then
        echo "Error: smoke test binary not found: ${exe}" >&2
        echo "Rebuild with: ./build_scripts/build_usd_embree.sh --tests" >&2
        exit 1
    fi

    mkdir -p "$(dirname -- "${smoke_output}")"
    HDEMBREE_SAMPLES_TO_CONVERGENCE="${HDEMBREE_SAMPLES_TO_CONVERGENCE:-1}" \
    HDEMBREE_JITTER_CAMERA="${HDEMBREE_JITTER_CAMERA:-0}" \
        "${exe}" \
        -offscreen \
        -lighting \
        -shading smooth \
        -frameAll \
        -renderer HdEmbreeRendererPlugin \
        -stage "${stage}" \
        -write "${smoke_output}"
}

run_ctest() {
    require_tests_enabled
    echo "Running CTest filter: ${ctest_filter}" >&2
    echo "Note: image-diff CTests require graphics support and may fail on baseline differences." >&2
    ctest --test-dir "${build_dir}" -R "${ctest_filter}" --output-on-failure
}

case "${mode}" in
    materialxcpp)
        run_materialxcpp
        ;;
    smoke)
        run_smoke
        ;;
    ctest)
        run_ctest
        ;;
esac
