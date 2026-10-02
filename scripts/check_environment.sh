#!/usr/bin/env bash
set -euo pipefail

section() {
  printf '\n## %s\n' "$1"
}

show_if_available() {
  local tool="$1"
  shift
  if command -v "$tool" >/dev/null 2>&1; then
    "$tool" "$@" 2>&1 || true
  else
    printf '%s: not found\n' "$tool"
  fi
}

section 'Operating system'
uname -a

section 'Display adapters and kernel drivers'
show_if_available lspci -nnk | grep -E -i -A4 'vga|3d|display' || true

section 'Compute GPUs (KFD)'
# d4r's bridge picks the GPU with the most SIMDs (D4R_GPU_ARCH overrides) and its native kernel folder
best=0
best_arch=''
for props in /sys/class/kfd/kfd/topology/nodes/*/properties; do
  [[ -r "$props" ]] || continue
  simds="$(sed -n 's/^simd_count //p' "$props")"
  target="$(sed -n 's/^gfx_target_version //p' "$props")"
  [[ -n "$simds" && "$simds" != 0 && -n "$target" && "$target" != 0 ]] || continue
  arch="$(printf 'gfx%d%d%x' $((target / 10000)) $(((target / 100) % 100)) $((target % 100)))"
  printf '%-10s %4s SIMDs  (%s)\n' "$arch" "$simds" "$(dirname "$props")"
  if (( simds > best )); then
    best="$simds"
    best_arch="$arch"
  fi
done
if [[ -n "$best_arch" ]]; then
  printf 'd4r uses: %s%s\n' "${D4R_GPU_ARCH:-$best_arch}" "${D4R_GPU_ARCH:+ (D4R_GPU_ARCH)}"
else
  printf 'no KFD GPU found (/dev/kfd missing or no amdgpu compute support)\n'
fi

section 'ROCm / HIP'
show_if_available rocminfo | head -80 || true
show_if_available hipcc --version | head -20 || true
if command -v rocm-smi >/dev/null 2>&1; then
  rocm-smi --showproductname --showdriverversion 2>&1 | head -50 || true
elif [[ -x /opt/rocm/bin/rocm-smi ]]; then
  /opt/rocm/bin/rocm-smi --showproductname --showdriverversion 2>&1 | head -50 || true
fi

section 'Vulkan'
if command -v vulkaninfo >/dev/null 2>&1; then
  vulkaninfo --summary 2>&1 | grep -E -i 'VULKANINFO|Instance Version|GPU[0-9]|apiVersion|driverVersion|vendorID|deviceID|deviceName|driverID|driverName|driverInfo' || true
else
  printf 'vulkaninfo: not found\n'
fi

section 'Wine / Proton'
show_if_available wine --version
show_if_available protontricks --version
find "$HOME/.local/share/Steam/compatibilitytools.d" -maxdepth 3 -type f -name proton -print 2>/dev/null | head -20 || true

section 'Build and PE inspection tools'
for tool in x86_64-w64-mingw32-g++ cmake ninja llvm-objdump llvm-readobj objdump strings; do
  if command -v "$tool" >/dev/null 2>&1; then
    printf '%-28s %s\n' "$tool" "$(command -v "$tool")"
  else
    printf '%-28s %s\n' "$tool" 'not found'
  fi
done

section 'Installed relevant packages'
packages='(^| )(rocm|hip|vulkan|wine|mingw|clang|mesa)'
if command -v pacman >/dev/null 2>&1; then
  pacman -Q 2>/dev/null | grep -E -i "$packages(-| )" || true
elif command -v dpkg-query >/dev/null 2>&1; then
  dpkg-query -W -f '${Package} ${Version}\n' 2>/dev/null | grep -E -i "$packages" || true
elif command -v rpm >/dev/null 2>&1; then
  rpm -qa --qf '%{NAME} %{VERSION}\n' 2>/dev/null | grep -E -i "$packages" || true
else
  printf 'no pacman, dpkg or rpm found\n'
fi
