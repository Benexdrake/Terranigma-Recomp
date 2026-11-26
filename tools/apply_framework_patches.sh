#!/usr/bin/env bash
# Apply the local snesrecomp patches (patches/snesrecomp-*.patch). Idempotent:
# a patch that is already applied is skipped. CMake does the same at configure
# time, so this script is only needed to patch the framework without building.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
for p in "$ROOT"/patches/snesrecomp-*.patch; do
  [ -f "$p" ] || continue
  if git -C "$ROOT/snesrecomp" apply --check --reverse "$p" 2>/dev/null; then
    echo "already applied: $(basename "$p")"
  else
    git -C "$ROOT/snesrecomp" apply --ignore-whitespace "$p"
    echo "applied: $(basename "$p")"
  fi
done
