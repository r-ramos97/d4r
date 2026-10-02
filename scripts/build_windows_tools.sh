#!/usr/bin/env bash
# Builds the small native Windows parts of d4r into build/windows/ (docs/windows.md):
#   nvapi64.dll       tools/d4r_nvapi_windows.c: the NVIDIA identity for OptiScaler and NGX
#   version.dll       tools/d4r_preload_version.c: loads that nvapi64.dll before OptiScaler starts
#   d4r-manifest.exe  tools/d4r_manifest.c: writes native kernel manifests
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CC="${MINGW_CC:-x86_64-w64-mingw32-gcc}"
OUT="$ROOT/build/windows"
mkdir -p "$OUT"
FLAGS=(-std=gnu11 -O2 -Wall -Wextra -Werror -I "$ROOT/tools")

"$CC" "${FLAGS[@]}" -shared -static-libgcc "$ROOT/tools/d4r_nvapi_windows.c" -o "$OUT/nvapi64.dll"
{
  printf 'LIBRARY version.dll\nEXPORTS\n'
  sed -n 's/^    X([0-9]*, \([A-Za-z]*\)).*/  \1/p' "$ROOT/tools/d4r_preload_version.c"
  printf '  d4r_preload_version\n'
} > "$OUT/version.def"
"$CC" "${FLAGS[@]}" -shared -static-libgcc "$ROOT/tools/d4r_preload_version.c" "$OUT/version.def" -o "$OUT/version.dll"
"$CC" "${FLAGS[@]}" -static "$ROOT/tools/d4r_manifest.c" -o "$OUT/d4r-manifest.exe"
printf 'Built %s/{nvapi64.dll,version.dll,d4r-manifest.exe}\n' "$OUT"
