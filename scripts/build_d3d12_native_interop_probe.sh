#!/usr/bin/env bash
# Builds build/windows/d4r-interop-probe.exe (tools/d3d12_native_interop_probe.cpp): whether AMD's Windows HIP
# shares VRAM and GPU-side synchronisation with D3D12, through the native nvcuda bridge. See docs/windows.md.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MINGW_CXX="${MINGW_CXX:-x86_64-w64-mingw32-g++}"
mkdir -p "$ROOT/build/windows"
"$MINGW_CXX" -std=c++20 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers -static -static-libgcc \
  -static-libstdc++ "$ROOT/tools/d3d12_native_interop_probe.cpp" -ld3d12 -ldxgi \
  -o "$ROOT/build/windows/d4r-interop-probe.exe"
printf 'Built %s\n' "$ROOT/build/windows/d4r-interop-probe.exe"
