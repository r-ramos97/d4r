#!/bin/sh
# Checks an d4r install: run it from the game folder (the one holding the game's .exe and d4r/),
# or give that folder as the argument. It only reads files and prints what it finds.
# usage: sh d4r/d4r-check.sh [GAME_FOLDER] [ROCM_DIR]
GAME="${1:-.}"
D4R="$GAME/d4r"
# A d4r.ini setting as the shim reads it: the last "Key = value" (key in any case, inline ; or # comment and
# CRLF removed); empty when it is unset or auto.
ini_get() {
  awk -v key="$1" '
    { sub(/\r$/, ""); line = $0; sub(/^[ \t]+/, "", line) }
    line ~ /^[;#]/ || substr(line, 1, 1) == "[" { next }
    { eq = index(line, "="); if (!eq) next
      k = substr(line, 1, eq - 1); sub(/[ \t]+$/, "", k); if (tolower(k) != tolower(key)) next
      v = substr(line, eq + 1); sub(/[ \t]+[;#].*$/, "", v); gsub(/^[ \t]+|[ \t]+$/, "", v); value = v }
    END { if (tolower(value) != "auto") print value }' "$D4R/d4r.ini" 2>/dev/null
}
# 1 or 0 for a boolean setting, $2 when it is unset or not a boolean
ini_flag() {
  case "$(ini_get "$1" | tr '[:upper:]' '[:lower:]')" in
    1|true|yes|on) echo 1 ;; 0|false|no|off) echo 0 ;; *) echo "$2" ;;
  esac
}
# ROCm: the argument, else d4r.ini's RocmDir, else the bundled d4r/rocm, else D4R_ROCM_DIR, else /opt/rocm
ini_rocm=$(ini_get RocmDir)
case "$ini_rocm" in "~/"*) ini_rocm="$HOME/${ini_rocm#\~/}" ;; esac
[ -z "$ini_rocm" ] && [ -d "$D4R/rocm/lib" ] && ini_rocm="$D4R/rocm"
ROCM="${2:-${ini_rocm:-${D4R_ROCM_DIR:-/opt/rocm}}}"
problems=0
ok() { printf '  ok       %s\n' "$1"; }
bad() { printf '  MISSING  %s\n' "$1"; problems=$((problems + 1)); }
note() { printf '  note     %s\n' "$1"; }

printf 'd4r install in %s\n' "$(cd "$GAME" 2>/dev/null && pwd || echo "$GAME")"
[ -d "$D4R" ] || { printf 'no d4r folder here; run this from the folder with the game .exe\n'; exit 1; }
ls "$GAME"/*.exe >/dev/null 2>&1 && ok "game executable next to d4r/" || note "no .exe next to d4r/ (it must be the game's main executable folder)"
for f in dxgi.dll OptiScaler.ini d3d12.dll d3d12core.dll d4r/nvngx.dll d4r/nvcuda.dll d4r/zluda/libcuda.so d4r/d4r.ini; do
  [ -f "$GAME/$f" ] && ok "$f" || bad "$f (re-extract the d4r zip)"
done
[ -f "$D4R/nvngx_dlss.dll" ] && ok "d4r/nvngx_dlss.dll (NVIDIA DLSS library)" || bad "d4r/nvngx_dlss.dll: copy NVIDIA's DLSS library here"
[ -f "$D4R/ngx/_nvngx.dll" ] && ok "d4r/ngx/_nvngx.dll (NVIDIA NGX runtime)" || bad "d4r/ngx/_nvngx.dll: copy NVIDIA's NGX runtime here"
if [ -f "$D4R/nvngx_dlss.dll" ] && command -v strings >/dev/null 2>&1; then
  v=$(strings -el "$D4R/nvngx_dlss.dll" | grep -A1 '^FileVersion$' | sed -n 2p | tr ',' '.')
  case "$v" in
    310.7.*|310.9.*) ok "DLSS version $v (native kernels verified for 310.7 and 310.9)" ;;
    "") note "cannot read the DLSS version" ;;
    *) note "DLSS version $v: kernels whose code changed run without native kernels (slower)" ;;
  esac
fi

found=
for dir in "$ROCM/lib" /opt/rocm/lib /usr/lib /usr/lib64 /usr/lib/x86_64-linux-gnu; do
  [ -e "$dir/libamdhip64.so.7" ] && { found="$dir"; break; }
done
[ -n "$found" ] && ok "ROCm HIP runtime ($found/libamdhip64.so.7)" || bad "ROCm HIP runtime 7.x (libamdhip64.so.7); re-extract the d4r zip, which includes it in d4r/rocm"
[ -e /dev/kfd ] && ok "/dev/kfd (ROCm compute device)" || bad "/dev/kfd: the amdgpu compute interface is not available"
# The GPU the bridge uses: D4R_GPU_ARCH, else the KFD GPU with the most SIMDs (a discrete GPU over an iGPU).
# D4R_CHECK_KFD_NODES replaces the KFD topology directory (for tests).
target=""; best=0; gpus=0
for props in "${D4R_CHECK_KFD_NODES:-/sys/class/kfd/kfd/topology/nodes}"/*/properties; do
  s=$(sed -n 's/^simd_count //p' "$props" 2>/dev/null); t=$(sed -n 's/^gfx_target_version //p' "$props" 2>/dev/null)
  [ -n "$s" ] && [ "$s" != 0 ] && [ -n "$t" ] && [ "$t" != 0 ] || continue
  gpus=$((gpus + 1))
  [ "$s" -gt "$best" ] && { best="$s"; target="$t"; }
done
arch=""
[ -n "$target" ] && arch=$(printf 'gfx%d%d%x' $((target / 10000)) $(((target / 100) % 100)) $((target % 100)))
case "${D4R_GPU_ARCH:-}" in
  gfx*) arch="$D4R_GPU_ARCH"; note "GPU $arch set by D4R_GPU_ARCH" ;;
  *) [ "$gpus" -gt 1 ] && note "$gpus GPUs; d4r uses the one with the most SIMDs ($arch); D4R_GPU_ARCH overrides" ;;
esac

# The native kernel set the bridge serves for that GPU (tools/d4r_native_selection.h, release layout):
# PreferAccuracy uses kernels/accuracy, NativeFp8 on RDNA4 the <target>-fp8 folder, else <target>.
# The environment wins over d4r.ini, as in the shim.
accuracy="${D4R_PREFER_ACCURACY:-$(ini_flag PreferAccuracy 0)}"
fp8="${D4R_ZLUDA_WMMA_FP8_NATIVE:-$(ini_flag NativeFp8 1)}"
accuracy_marker() {
  [ "$(head -n 1 "$1/d4r-accuracy.txt" 2>/dev/null)" = 1 ] && [ "$(head -n 1 "$1/d4r-accuracy.txt" | wc -c)" -eq 2 ]
}
kernel_set() {
  if [ "$accuracy" = 1 ]; then accuracy_marker "$1" || return 1; fi
  [ -f "$1/d4r-kernels.txt" ] || [ "$accuracy" = 1 ]
}
kernel_dir() {
  base="$D4R/kernels"
  if [ "$accuracy" = 1 ] && ! accuracy_marker "$base"; then base="$base/accuracy"; fi
  case "$fp8:$arch" in 1:gfx12*) kernel_set "$base/$arch-fp8" && { echo "$base/$arch-fp8"; return; } ;; esac
  kernel_set "$base/$arch" && { echo "$base/$arch"; return; }
  kernel_set "$base" && echo "$base"
}
case "$(ini_get NativeKernels | tr '[:upper:]' '[:lower:]')" in ""|on|true|1|fast) native=1 ;; *) native=0 ;; esac
set_kind=""; [ "$accuracy" = 1 ] && set_kind="accuracy "
if [ "$native" = 0 ]; then
  note "NativeKernels = off: ZLUDA compiles every DLSS kernel from NVIDIA's code (much slower)"
elif [ -n "$arch" ]; then
  dir=$(kernel_dir)
  if [ -n "$dir" ]; then
    ok "GPU $arch: ${set_kind}native kernels in ${dir#"$GAME"/}"
    case "$fp8:$arch:$dir" in 1:gfx12*:*-fp8) ;; 1:gfx12*) note "no FP8 variant for $arch; using its 16-bit kernels" ;; esac
  elif [ "$accuracy" = 1 ]; then
    note "GPU $arch: PreferAccuracy = true but no accuracy kernels for it here; ZLUDA compiles NVIDIA's code (much slower)"
  else
    note "GPU $arch: no native kernels for it in this release (DLSS runs, much slower)"
  fi
fi

printf '\nSteam launch options for this game:\n  PROTON_FORCE_NVAPI=1 DXVK_NVAPI_GPU_ARCH=AD100 %%command%%\n'
[ "$problems" -eq 0 ] && printf '\nEverything d4r needs is in place.\n' || printf '\n%d thing(s) to fix above.\n' "$problems"
