#!/usr/bin/env bash
# Build the firmware and stage it for OTA distribution via the frontend.
#
# Usage: scripts/release.sh
#   1. Bump FIRMWARE_VERSION in include/version.h first.
#   2. Run this script; it builds firmware.bin and copies it (plus
#      version.json) into ../eink-frontend/public/firmware/.
#   3. Commit and push the frontend repo so Vercel serves the new build.
set -euo pipefail

FIRMWARE_DIR="$(cd "$(dirname "$0")/.." && pwd)"
FRONTEND_DIR="$FIRMWARE_DIR/../eink-frontend"
OTA_DIR="$FRONTEND_DIR/public/firmware"

VERSION=$(sed -n 's/^#define FIRMWARE_VERSION "\(.*\)"/\1/p' "$FIRMWARE_DIR/include/version.h")
if [[ -z "$VERSION" ]]; then
  echo "error: FIRMWARE_VERSION not found in include/version.h" >&2
  exit 1
fi

echo "Building firmware v$VERSION..."
cd "$FIRMWARE_DIR"
python3 -m platformio run

BIN_PATH=$(ls "$FIRMWARE_DIR"/.pio/build/*/firmware.bin | head -1)
if [[ ! -f "$BIN_PATH" ]]; then
  echo "error: firmware.bin not found under .pio/build" >&2
  exit 1
fi

mkdir -p "$OTA_DIR"
cp "$BIN_PATH" "$OTA_DIR/firmware.bin"
cat > "$OTA_DIR/version.json" <<EOF
{
  "version": "$VERSION",
  "url": "https://eink-display-frontend.vercel.app/firmware/firmware.bin",
  "releasedAt": "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
}
EOF

echo "Staged OTA release v$VERSION:"
ls -lh "$OTA_DIR"
echo "Next: commit & push eink-frontend so Vercel serves the new build."
