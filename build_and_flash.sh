#!/bin/bash
# Builds Sentinel Glance and flashes it to an M5Stack Core2 plugged in by USB.
# First run installs PlatformIO into ./.venv (about 2 minutes, plus the ESP32 toolchain download).
#   ./build_and_flash.sh          build + flash
#   ./build_and_flash.sh build    build only
set -euo pipefail
cd "$(dirname "$0")"

if [ ! -x .venv/bin/pio ]; then
  echo "Installing PlatformIO into .venv ..."
  python3 -m venv .venv
  .venv/bin/pip install -q --upgrade pip platformio
fi
PIO=.venv/bin/pio

if [ "${1:-}" = "build" ]; then
  $PIO run -e core2
  exit 0
fi

PORT=$(ls /dev/cu.usbserial-* /dev/cu.wchusbserial* /dev/cu.SLAB_USBtoUART 2>/dev/null | head -1 || true)
if [ -z "$PORT" ]; then
  echo "No Core2 found. Plug it in with a data USB-C cable (the Core2 uses a CH9102 USB chip)."
  exit 1
fi
echo "Flashing $PORT"
$PIO run -e core2 -t upload --upload-port "$PORT"
echo
echo "Done. Watch the log with:  $PIO device monitor -p $PORT -b 115200"
