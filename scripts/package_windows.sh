#!/usr/bin/env bash
# Builds the d4r package for native Windows (docs/windows.md, packaging/windows/D4R_WINDOWS_README.txt): a zip
# whose contents go into the folder that holds a game's main .exe. It has no NVIDIA files, no OptiScaler and no
# HIP runtime; users add them, and d4r\setup.ps1 checks the result.
#
# usage: scripts/package_windows.sh [OUT_DIR]          (default: dist/)
#   D4R_WIN_ZLUDA    folder with ZLUDA's Windows build as zluda_nvcuda.dll and its license texts (windows.yml's
#                    zluda-windows artifact; required)
#   D4R_WIN_KERNELS  native network kernels in the bridge's layout (windows.yml's network-kernels artifact or
#                    scripts/build_network_kernels.sh); optional, without it DLSS runs on ZLUDA alone
#   MINGW_CC, MINGW_CXX, CLANG_CL  compilers (defaults as in the build scripts)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$(realpath -m "${1:-$ROOT/dist}")"
VERSION="$(cat "$ROOT/packaging/VERSION")-windows-preview"
NAME="d4r-$VERSION"
STAGE="$OUT/$NAME"
: "${D4R_WIN_ZLUDA:?set D4R_WIN_ZLUDA to the folder with zluda_nvcuda.dll}"
[[ -f "$D4R_WIN_ZLUDA/zluda_nvcuda.dll" ]] || { echo "no zluda_nvcuda.dll in $D4R_WIN_ZLUDA" >&2; exit 2; }

"$ROOT/scripts/build_d3d12_dlss_harness.sh" >/dev/null  # the shim, and the harness d4r\test-dlss.ps1 runs
"$ROOT/scripts/build_windows_nvcuda.sh" >/dev/null
"$ROOT/scripts/build_windows_tools.sh" >/dev/null  # nvapi64.dll, version.dll, d4r-manifest.exe

rm -rf "$STAGE"
mkdir -p "$STAGE/d4r/zluda" "$STAGE/d4r/ngx" "$STAGE/d4r/tools" "$STAGE/d4r/licenses" "$STAGE/d4r/source"
crlf() { sed 's/$/\r/' "$1" > "$2"; }
cp "$ROOT/build/windows/nvapi64.dll" "$ROOT/build/windows/version.dll" "$STAGE/"
sed "s/@VERSION@/$VERSION/g" "$ROOT/packaging/windows/D4R_WINDOWS_README.txt" | sed 's/$/\r/' > "$STAGE/D4R_WINDOWS_README.txt"
cp "$ROOT/build/d4r_nvngx.dll" "$STAGE/d4r/nvngx.dll"
cp "$ROOT/build/windows/nvcuda.dll" "$STAGE/d4r/nvcuda.dll"
cp "$ROOT/build/windows/d4r-manifest.exe" "$STAGE/d4r/d4r-manifest.exe"
crlf "$ROOT/packaging/windows/d4r.ini" "$STAGE/d4r/d4r.ini"
crlf "$ROOT/packaging/windows/setup.ps1" "$STAGE/d4r/setup.ps1"
crlf "$ROOT/packaging/windows/test-dlss.ps1" "$STAGE/d4r/test-dlss.ps1"
cp "$ROOT/build/d3d12_dlss_harness.exe" "$STAGE/d4r/tools/d4r-harness.exe"
crlf "$ROOT/packaging/windows/optiscaler.settings" "$STAGE/d4r/optiscaler-d4r.settings"
cp "$D4R_WIN_ZLUDA/zluda_nvcuda.dll" "$STAGE/d4r/zluda/zluda_nvcuda.dll"
printf 'Put NVIDIA'"'"'s NGX runtime, _nvngx.dll, in this folder (see D4R_WINDOWS_README.txt).\r\n' > "$STAGE/d4r/ngx/README.txt"
if [[ -n "${D4R_WIN_KERNELS:-}" ]]; then
  mkdir -p "$STAGE/d4r/kernels"
  (cd "$D4R_WIN_KERNELS" && find . -name '*.hsaco' -o -name 'd4r-accuracy.txt' | while read -r f; do
     mkdir -p "$STAGE/d4r/kernels/$(dirname "$f")"
     cp "$f" "$STAGE/d4r/kernels/$f"
   done)
fi

L="$STAGE/d4r/licenses"
cp "$ROOT/LICENSE" "$L/d4r-LICENSE.txt"
cp "$ROOT/NOTICE" "$L/d4r-NOTICE.txt"
for f in "$D4R_WIN_ZLUDA"/*LICENSE*; do [[ -f "$f" ]] && cp "$f" "$L/"; done
{
  printf 'd4r %s for native Windows\r\n\r\n' "$VERSION"
  printf 'd4r source: %s\r\n' "$(git -C "$ROOT" rev-parse HEAD 2>/dev/null || echo unknown)"
  printf 'ZLUDA: https://github.com/vosen/ZLUDA at %s with d4r'"'"'s patches/zluda\r\n' \
    "$(cat "$D4R_WIN_ZLUDA/ZLUDA_COMMIT.txt" 2>/dev/null || echo unknown)"
  printf 'nvapi64.dll, version.dll, d4r\\nvngx.dll, d4r\\nvcuda.dll, d4r\\d4r-manifest.exe, d4r\\tools\\d4r-harness.exe: d4r (tools/)\r\n'
  printf 'd4r\\kernels: d4r'"'"'s native network kernels (kernels/), built with ROCm\r\n'
} > "$STAGE/d4r/source/SOURCES.txt"

EPOCH="${SOURCE_DATE_EPOCH:-$(git -C "$ROOT" log -1 --format=%ct)}"
find "$STAGE" -exec touch -h -d "@$EPOCH" {} +
rm -f "$OUT/$NAME.zip"
(cd "$STAGE" && find . -type f | LC_ALL=C sort | sed 's|^\./||' | zip -q -X -9 "$OUT/$NAME.zip" -@)
(cd "$OUT" && sha256sum "$NAME.zip" > "$NAME.zip.sha256")
printf 'Built %s (%s)\n' "$OUT/$NAME.zip" "$(du -h "$OUT/$NAME.zip" | cut -f1)"
