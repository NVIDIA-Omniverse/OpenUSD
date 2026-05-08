#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
install_root="${repo_root}/_usd_install"
mode="smoke"
renderer="Embree"
camera=""
image_width=""
samples=""
disable_camera_light=1
allow_nonpersistent_output=0
extra_usdrecord_args=()

usage() {
    cat >&2 <<'EOF'
Usage: render_hdembree.sh [smoke|final-srgb] [options] SCENE.usd[a|c] [OUTPUT.png]

Modes:
  smoke       Sandbox-friendly linear smoke render. Uses --disableGpu.
  final-srgb  Persistent sRGB render. Run outside the sandbox when needed.

Options:
  --install DIR                    Use a non-default USD install root
  --renderer NAME                  usdrecord renderer name (default: Embree)
  --camera PATH                    Camera prim path
  --image-width N                  Output width (default: 64 smoke, 512 final)
  --samples N                      HDEMBREE_SAMPLES_TO_CONVERGENCE override
  --camera-light                   Keep usdrecord camera light enabled
  --disable-camera-light           Disable usdrecord camera light (default)
  --render-settings PATH           Pass --renderSettingsPrimPath
  --session-layer PATH             Pass --sessionLayer
  --enable-dome-light-visibility   Pass --enableDomeLightVisibility
  --allow-nonpersistent-output     Allow final-srgb outside dev-OpenUSD
  -h, --help                       Show this help

Examples:
  render_hdembree.sh smoke scene.usda /tmp/smoke.png
  render_hdembree.sh final-srgb --camera /World/Camera scene.usda \
    /home/masuo/Pictures/dev-OpenUSD/my-ticket/final.png
EOF
}

if [[ $# -gt 0 ]]; then
    case "$1" in
        smoke|final-srgb)
            mode="$1"
            shift
            ;;
    esac
fi

positional=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --install)
            install_root="$2"
            shift 2
            ;;
        --renderer)
            renderer="$2"
            shift 2
            ;;
        --camera)
            camera="$2"
            shift 2
            ;;
        --image-width)
            image_width="$2"
            shift 2
            ;;
        --samples)
            samples="$2"
            shift 2
            ;;
        --camera-light)
            disable_camera_light=0
            shift
            ;;
        --disable-camera-light)
            disable_camera_light=1
            shift
            ;;
        --render-settings)
            extra_usdrecord_args+=(--renderSettingsPrimPath "$2")
            shift 2
            ;;
        --session-layer)
            extra_usdrecord_args+=(--sessionLayer "$2")
            shift 2
            ;;
        --enable-dome-light-visibility)
            extra_usdrecord_args+=(--enableDomeLightVisibility)
            shift
            ;;
        --allow-nonpersistent-output)
            allow_nonpersistent_output=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            positional+=("$@")
            break
            ;;
        -*)
            echo "Error: unknown option: $1" >&2
            usage
            exit 2
            ;;
        *)
            positional+=("$1")
            shift
            ;;
    esac
done

if [[ "${#positional[@]}" -lt 1 || "${#positional[@]}" -gt 2 ]]; then
    echo "Error: expected SCENE and optional OUTPUT." >&2
    usage
    exit 2
fi

scene="${positional[0]}"
output="${positional[1]:-}"

if [[ ! -f "${scene}" ]]; then
    echo "Error: scene file not found: ${scene}" >&2
    exit 1
fi

case "${mode}" in
    smoke)
        image_width="${image_width:-64}"
        samples="${samples:-1}"
        output="${output:-/tmp/hdembree-smoke.png}"
        color_mode="disabled"
        use_disable_gpu=1
        export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}"
        ;;
    final-srgb)
        image_width="${image_width:-512}"
        samples="${samples:-64}"
        color_mode="sRGB"
        use_disable_gpu=0
        if [[ -z "${output}" ]]; then
            echo "Error: final-srgb requires an explicit persistent output path." >&2
            exit 2
        fi
        if [[ "${allow_nonpersistent_output}" -eq 0 && "${output}" != /home/masuo/Pictures/dev-OpenUSD/* ]]; then
            echo "Error: final-srgb output must be under /home/masuo/Pictures/dev-OpenUSD/." >&2
            echo "Use --allow-nonpersistent-output only for temporary diagnostics." >&2
            exit 2
        fi
        if [[ "${QT_QPA_PLATFORM:-}" == "offscreen" ]]; then
            echo "Warning: QT_QPA_PLATFORM=offscreen may break sRGB GPU color correction." >&2
        fi
        ;;
    *)
        echo "Error: unknown mode: ${mode}" >&2
        usage
        exit 2
        ;;
esac

mkdir -p "$(dirname -- "${output}")"

export PATH="${install_root}/bin${PATH:+:${PATH}}"
export PYTHONPATH="${install_root}/lib/python${PYTHONPATH:+:${PYTHONPATH}}"
export LD_LIBRARY_PATH="${install_root}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
export PXR_PLUGINPATH_NAME="${install_root}/lib/usd:${install_root}/plugin/usd${PXR_PLUGINPATH_NAME:+:${PXR_PLUGINPATH_NAME}}"
export HDEMBREE_SAMPLES_TO_CONVERGENCE="${samples}"

cmd=("${install_root}/bin/usdrecord" --renderer "${renderer}")

if [[ "${use_disable_gpu}" -eq 1 ]]; then
    cmd+=(--disableGpu)
fi

if [[ "${disable_camera_light}" -eq 1 ]]; then
    cmd+=(--disableCameraLight)
fi

if [[ -n "${camera}" ]]; then
    cmd+=(--camera "${camera}")
fi

cmd+=(--colorCorrectionMode "${color_mode}" --imageWidth "${image_width}")
cmd+=("${extra_usdrecord_args[@]}")
cmd+=("${scene}" "${output}")

printf 'Running:'
printf ' %q' "${cmd[@]}"
printf '\n'

"${cmd[@]}"
