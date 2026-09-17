#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIRMWARE_DIR="$SCRIPT_DIR/firmware"
SUPPORT_DIR="$HOME/.fullwash-installer"
VENV_DIR="$SUPPORT_DIR/venv"
LOG_FILE="$HOME/Library/Logs/FullWashInstaller.log"
OLD_VENV_DIR="$HOME/Library/Application Support/FullWashInstaller/venv"

pause_before_exit() {
  echo ""
  read -n 1 -s -r -p "Press any key to close this window..."
  echo ""
}

log() {
  echo "$1" | tee -a "$LOG_FILE"
}

mkdir -p "$SUPPORT_DIR" "$(dirname "$LOG_FILE")"
: > "$LOG_FILE"

cd "$SCRIPT_DIR"

echo "========================================"
echo "  FullWash Firmware Installer"
echo "========================================"
echo ""

if ! command -v python3 >/dev/null 2>&1; then
  log "Python 3 is not available on this Mac."
  echo ""
  echo "Please install Xcode Command Line Tools:"
  echo "  1. Open Terminal (Applications > Utilities > Terminal)"
  echo "  2. Run: xcode-select --install"
  echo "  3. Click Install in the dialog"
  echo "  4. When finished, double-click install.command again"
  echo ""
  pause_before_exit
  exit 1
fi

for required in bootloader.bin partitions.bin boot_app0.bin firmware.bin; do
  if [[ ! -f "$FIRMWARE_DIR/$required" ]]; then
    log "Missing firmware file: $FIRMWARE_DIR/$required"
    echo "ERROR: A firmware file is missing from this package."
    echo "Please download the installer again and unzip it fully."
    pause_before_exit
    exit 1
  fi
done

echo "Setting up flashing tools (first run may take a minute)..."
if [[ -d "$OLD_VENV_DIR" ]]; then
  rm -rf "$OLD_VENV_DIR"
fi
if [[ ! -d "$VENV_DIR" ]]; then
  python3 -m venv "$VENV_DIR"
fi

# shellcheck disable=SC1091
source "$VENV_DIR/bin/activate"
python3 -m pip install --upgrade pip >/dev/null
python3 -m pip install --upgrade esptool >/dev/null

PYTHON="$VENV_DIR/bin/python3"
if [[ ! -x "$PYTHON" ]] || ! "$PYTHON" -m esptool version >/dev/null 2>&1; then
  log "esptool was not installed correctly."
  echo "ERROR: Could not prepare the flashing tool."
  pause_before_exit
  exit 1
fi

echo ""
echo "Looking for the board on USB..."

find_ports() {
  local ports=()
  local pattern port
  for pattern in /dev/cu.usbserial-* /dev/cu.wchusbserial* /dev/cu.SLAB_USBtoUART; do
    for port in $pattern; do
      if [[ -e "$port" ]]; then
        ports+=("$port")
      fi
    done
  done
  if [[ ${#ports[@]} -gt 0 ]]; then
    printf '%s\n' "${ports[@]}" | sort -u
  fi
}

PORTS=()
while IFS= read -r port; do
  PORTS+=("$port")
done < <(find_ports)

if [[ ${#PORTS[@]} -eq 0 ]]; then
  log "No USB serial port found."
  echo ""
  echo "Please connect the board to your Mac with a USB-C cable,"
  echo "then double-click install.command again."
  echo ""
  echo "Tip: Some USB-C cables are charge-only. Try a different cable if needed."
  pause_before_exit
  exit 1
fi

PORT=""
if [[ ${#PORTS[@]} -eq 1 ]]; then
  PORT="${PORTS[0]}"
  echo "Found board at: $PORT"
else
  echo "Multiple USB devices were found:"
  for i in "${!PORTS[@]}"; do
    echo "  $((i + 1))) ${PORTS[$i]}"
  done
  echo ""
  read -r -p "Enter the number of the FullWash board: " choice
  if [[ ! "$choice" =~ ^[0-9]+$ ]] || (( choice < 1 || choice > ${#PORTS[@]} )); then
    log "Invalid port selection: $choice"
    echo "ERROR: Invalid selection."
    pause_before_exit
    exit 1
  fi
  PORT="${PORTS[$((choice - 1))]}"
  echo "Using: $PORT"
fi

echo ""
echo "Uploading firmware. Do not unplug the board..."
echo ""

set +e
"$PYTHON" -m esptool --chip esp32 --port "$PORT" --baud 921600 \
  --before default_reset --after hard_reset write_flash -z \
  --flash_mode dio --flash_freq 40m --flash_size detect \
  0x1000 "$FIRMWARE_DIR/bootloader.bin" \
  0x8000 "$FIRMWARE_DIR/partitions.bin" \
  0xe000 "$FIRMWARE_DIR/boot_app0.bin" \
  0x10000 "$FIRMWARE_DIR/firmware.bin" 2>&1 | tee -a "$LOG_FILE"
flash_status=${PIPESTATUS[0]}
set -e

echo ""
if [[ "$flash_status" -eq 0 ]]; then
  log "Firmware upload completed successfully."
  echo "========================================"
  echo "  Done - you can unplug the board."
  echo "========================================"
else
  log "Firmware upload failed with exit code $flash_status."
  echo "========================================"
  echo "  Upload failed."
  echo "========================================"
  echo ""
  echo "Please try again. If it keeps failing, send this log file:"
  echo "  $LOG_FILE"
fi

pause_before_exit
exit "$flash_status"
