#!/usr/bin/env bash
# Build the external display DDK target against the same kernel/configuration.
set -euo pipefail
workspace=$(realpath "$1")
platform=${2:?platform required}
variant=${3:?variant required}
fast=${4:-false}
case "$fast" in true|false) ;; *) echo "Invalid FAST_BUILD: $fast" >&2; exit 2;; esac
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
analysis_only=${AL1S_ANALYSIS_ONLY:-false}
if [[ "$analysis_only" == true && "$fast" != true ]]; then exit 2; fi
prepare_options=()
if [[ "$analysis_only" == true ]]; then prepare_options+=(--analysis-only); fi
if [[ "$fast" == true ]]; then
  "$workspace/kernel_platform/oplus/bazel/oplus_modules_variant.sh" "$platform" "$variant"
  python3 "$script_dir/al1s_fast_prebuilt.py" "$workspace" "${prepare_options[@]}"
fi
cd "$workspace/kernel_platform"
target="//vendor/qcom/opensource/display-drivers:${platform}_${variant}_display_drivers_dist"
if [[ ! -e vendor && ! -L vendor ]]; then
  ln -s ../vendor vendor
fi
test -f vendor/qcom/opensource/display-drivers/oplus/SM8750/al1s_originos.c
# Keep vendor builds sandboxed: native Oplus relative includes depend on it.
export TEST_TMPDIR="$workspace/bazel-cache"
options=(
  --//msm-kernel:skip_abi=true --//msm-kernel:skip_abl=true
  --config=stamp
  --user_kmi_symbol_lists=//msm-kernel:android/abi_gki_aarch64_qcom
  --ignore_missing_projects --incompatible_sandbox_hermetic_tmp=false
  --nozstd_dwarf_compression
)
if [[ "$fast" == true ]]; then
  ./tools/bazel --output_user_root="$workspace/bazel-cache" aquery \
    "${options[@]}" --output=jsonproto \
    "mnemonic(\"KernelBuild\", deps($target))" > "$workspace/al1s-module-actions.json"
  python3 - "$workspace/al1s-module-actions.json" <<'PY'
import json, sys
graph = json.load(open(sys.argv[1]))
targets = {str(x['id']): x['label'] for x in graph.get('targets', [])}
owners = sorted({targets[str(x['targetId'])] for x in graph.get('actions', [])})
if not owners or any('//common:' in name for name in owners):
    raise SystemExit('Unexpected module build graph; refusing a second GKI build: ' + repr(owners))
print('AL1S module-only kernel actions:', owners)
PY
fi
if [[ "$analysis_only" == true ]]; then
  echo "AL1S: complete vendor action graph validated before kernel compilation."
  exit 0
fi
./tools/bazel --output_user_root="$workspace/bazel-cache" run "${options[@]}" \
  "$target" -- --dist_dir="$PWD/out/al1s-display"
test -s out/al1s-display/msm_drm.ko
