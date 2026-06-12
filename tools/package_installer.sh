#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
ENV_NAME="T-SIM7600X"
BUILD_DIR="$PROJECT_ROOT/.pio/build/$ENV_NAME"
INSTALLER_DIR="$PROJECT_ROOT/installer"
FIRMWARE_DIR="$INSTALLER_DIR/firmware"
DIST_DIR="$PROJECT_ROOT/dist"
ZIP_NAME="FullWashInstaller.zip"

cd "$PROJECT_ROOT"

needs_build=false
if [[ ! -f "$BUILD_DIR/firmware.bin" ]]; then
  needs_build=true
else
  newest_src="$(find "$PROJECT_ROOT/src" -type f -name '*.cpp' -o -name '*.h' -print0 \
    | xargs -0 stat -f '%m' 2>/dev/null | sort -nr | head -1 || echo 0)"
  firmware_mtime="$(stat -f '%m' "$BUILD_DIR/firmware.bin")"
  if [[ "$newest_src" -gt "$firmware_mtime" ]]; then
    needs_build=true
  fi
fi

if [[ "$needs_build" == true ]]; then
  echo "Building firmware ($ENV_NAME)..."
  pio run -e "$ENV_NAME"
else
  echo "Using existing firmware build at $BUILD_DIR"
fi

find_boot_app0() {
  local candidate
  for candidate in \
    "$HOME/.platformio/packages/framework-arduinoespressif32@3.20011.230801/tools/partitions/boot_app0.bin" \
    "$HOME/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin" \
    "$HOME/.platformio/packages/framework-arduinoespressif32"*/tools/partitions/boot_app0.bin; do
    if [[ -f "$candidate" ]]; then
      echo "$candidate"
      return 0
    fi
  done
  return 1
}

BOOT_APP0="$(find_boot_app0 || true)"
if [[ -z "$BOOT_APP0" ]]; then
  echo "ERROR: Could not find boot_app0.bin in PlatformIO packages." >&2
  echo "Run 'pio run -e $ENV_NAME' once so the Arduino framework is installed." >&2
  exit 1
fi

for required in bootloader.bin partitions.bin firmware.bin; do
  if [[ ! -f "$BUILD_DIR/$required" ]]; then
    echo "ERROR: Missing $BUILD_DIR/$required" >&2
    exit 1
  fi
done

mkdir -p "$FIRMWARE_DIR" "$DIST_DIR"
cp "$BUILD_DIR/bootloader.bin" "$FIRMWARE_DIR/"
cp "$BUILD_DIR/partitions.bin" "$FIRMWARE_DIR/"
cp "$BUILD_DIR/firmware.bin" "$FIRMWARE_DIR/"
cp "$BOOT_APP0" "$FIRMWARE_DIR/boot_app0.bin"

chmod +x "$INSTALLER_DIR/install.command"

rm -f "$DIST_DIR/$ZIP_NAME"
(
  cd "$INSTALLER_DIR"
  zip -r "$DIST_DIR/$ZIP_NAME" README.txt install.command firmware/*.bin
)

echo ""
echo "Package created: $DIST_DIR/$ZIP_NAME"
echo "Contents:"
unzip -l "$DIST_DIR/$ZIP_NAME"
