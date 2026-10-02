#!/usr/bin/env bash
set -euo pipefail

# Builds build/windows/nvcuda.dll: the nvcuda bridge (tools/wine_nvcuda_bridge.c) as a native Windows DLL
# for Windows games. It forwards the CUDA driver API to ZLUDA's Windows build (zluda/zluda_nvcuda.dll next
# to it, or D4R_ZLUDA_LIBCUDA) and adds the same d4r helpers as the Wine builtin. See docs/windows.md.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CC="${MINGW_CC:-x86_64-w64-mingw32-gcc}"
OUT="$ROOT/build/windows"

mkdir -p "$OUT"
# the exports the Wine spec file lists
{
  printf 'LIBRARY nvcuda.dll\nEXPORTS\n'
  sed -n 's/^@ stdcall \([A-Za-z0-9_]*\)(.*/  \1/p' "$ROOT/tools/wine_nvcuda_bridge.spec"
} > "$OUT/nvcuda.def"
"$CC" -std=gnu11 -O2 -Wall -Wextra -Wno-format-truncation -DD4R_NATIVE_WINDOWS -shared \
  -static -static-libgcc "$ROOT/tools/wine_nvcuda_bridge.c" "$OUT/nvcuda.def" -o "$OUT/nvcuda.dll"
printf 'Built %s\n' "$OUT/nvcuda.dll"
