#!/usr/bin/env bash
# Builds the native DLSS 4 and 4.5 network-layer kernels (kernels/build.sh k and m) for every release target,
# fast and accuracy sets, in the layout the bridge selects from (docs/native-kernels.md):
#   OUT_DIR/<target>/, OUT_DIR/<gfx12 target>-fp8/, OUT_DIR/accuracy/<same folders>/
# The texture kernels are built from NVIDIA's PTX and are not part of this; a folder serves only once its
# d4r-kernels.txt manifest exists (kernels/tools/kernel_manifest.py, or d4r-manifest.exe on Windows).
#
# usage: scripts/build_network_kernels.sh OUT_DIR
#   D4R_ROCM_DIR      ROCm with clang and the HIP device libraries (default /opt/rocm)
#   D4R_GPU_ARCHS     targets (default gfx1100 gfx1101 gfx1102 gfx1103 gfx1200 gfx1201)
#   D4R_KERNEL_JOBS   folders built at once (default: the number of CPUs)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$(realpath -m "${1:?usage: scripts/build_network_kernels.sh OUT_DIR}")"
ARCHS="${D4R_GPU_ARCHS:-gfx1100 gfx1101 gfx1102 gfx1103 gfx1200 gfx1201}"
JOBS="${D4R_KERNEL_JOBS:-$(nproc)}"

build_folder() {
  local folder="$1" arch="${1%-fp8}" fp8=0 accuracy
  [[ "$folder" == *-fp8 ]] && fp8=1
  for accuracy in 0 1; do
    local dir="$OUT/$folder"
    [[ "$accuracy" == 1 ]] && dir="$OUT/accuracy/$folder"
    for family in k m; do
      D4R_PREFER_ACCURACY="$accuracy" D4R_GPU_ARCH="$arch" D4R_NATIVE_FP8="$fp8" \
        "$ROOT/kernels/build.sh" "$family" "$dir" > "$OUT/logs/$folder-$accuracy-$family.log" 2>&1 ||
        { echo "kernels for $folder (accuracy $accuracy, $family) failed:" >&2; cat "$OUT/logs/$folder-$accuracy-$family.log" >&2; return 1; }
    done
    rm -f "$dir"/*.resolution.txt
  done
  printf '%s: %s fast, %s accuracy kernels\n' "$folder" "$(ls "$OUT/$folder"/*.hsaco | wc -l)" \
    "$(ls "$OUT/accuracy/$folder"/*.hsaco | wc -l)"
}

mkdir -p "$OUT/logs"
folders=()
for arch in $ARCHS; do
  folders+=("$arch")
  [[ "$arch" == gfx12* ]] && folders+=("$arch-fp8")
done
pending=()
failed=0
for folder in "${folders[@]}"; do
  build_folder "$folder" &
  pending+=("$!")
  if (( ${#pending[@]} >= JOBS )); then
    wait "${pending[0]}" || failed=1
    pending=("${pending[@]:1}")
  fi
done
for pid in "${pending[@]}"; do wait "$pid" || failed=1; done
[[ "$failed" == 0 ]] || { echo "network kernel build failed" >&2; exit 1; }
