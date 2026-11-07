#!/usr/bin/env bash
# Builds the QNX target programs. Run inside Linux (or WSL) with QNX SDP 8.0 installed.
#   tools/build-target.sh [x86_64|aarch64le]
set -euo pipefail
PLATFORM="${1:-x86_64}"
SDP="${QNX_SDP:-$HOME/qnx800}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${FL_BUILD_DIR:-$HOME/faultline-build}"

# shellcheck disable=SC1091
source "$SDP/qnxsdp-env.sh" > /dev/null
rm -rf "$WORK"
mkdir -p "$WORK"
cp -r "$HERE/target" "$WORK/"
cd "$WORK/target"
if ! make PLATFORM="$PLATFORM" > make.log 2>&1; then
    grep -v '^qcc ' make.log | head -60
    echo "BUILD FAILED"
    exit 1
fi
grep -i -E 'warning|error' make.log | head -30 || true
ls -l "build/$PLATFORM"
echo "BUILD OK: $WORK/target/build/$PLATFORM"
