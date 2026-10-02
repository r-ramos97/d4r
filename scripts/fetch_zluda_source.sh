#!/usr/bin/env bash
# Checks out the ZLUDA revision patches/zluda apply to into DIR and applies them (0002 onwards).
#
# usage: scripts/fetch_zluda_source.sh DIR
#   ZLUDA_COMMIT       revision (default: the tested ee2f25a, docs/building.md)
#   ZLUDA_SUBMODULES=1 also fetch the LLVM and HiGHS submodules and the OCKL device library from Git LFS, which
#                      a full build needs; without it only sources (e.g. for the helper bitcode) are fetched
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIR="${1:?usage: scripts/fetch_zluda_source.sh DIR}"
COMMIT="${ZLUDA_COMMIT:-ee2f25a180099fa42f36b2346732e1f2470a03ad}"

git init -q "$DIR"
git -C "$DIR" remote add origin https://github.com/vosen/ZLUDA
GIT_LFS_SKIP_SMUDGE=1 git -C "$DIR" fetch -q --depth 1 origin "$COMMIT"
GIT_LFS_SKIP_SMUDGE=1 git -C "$DIR" -c advice.detachedHead=false checkout -q FETCH_HEAD
if [[ "${ZLUDA_SUBMODULES:-0}" == 1 ]]; then
  git -C "$DIR" submodule update --init --recursive --depth 1
  git -C "$DIR" lfs install --local
  git -C "$DIR" lfs pull --include=llvm_zluda/src/device-libs/ockl.bc
fi
for patch in "$ROOT"/patches/zluda/0*.patch; do
  git -C "$DIR" apply "$patch"
  printf 'applied %s\n' "$(basename "$patch")"
done
