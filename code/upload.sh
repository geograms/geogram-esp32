#!/bin/bash
# Upload firmware to ESP32-S3 ePaper device
# Usage: ./upload.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# Environment
ENV="esp32s3_epaper_1in54"

echo "=== Geogram ESP32 Firmware Upload ==="

# Check if firmware exists
FIRMWARE=".pio/build/${ENV}/firmware.bin"
if [ ! -f "$FIRMWARE" ]; then
    echo "Firmware not found. Building first..."
    ~/.platformio/penv/bin/pio run -e "$ENV"
fi

echo "Uploading firmware via built-in JTAG..."

# Upload using PlatformIO with esp-builtin (JTAG) protocol
# This is more reliable than USB-CDC serial on ESP32-S3
~/.platformio/penv/bin/pio run -e "$ENV" -t upload

echo ""
echo "Upload complete!"
echo ""
echo "To monitor serial output, run:"
echo "  ~/.platformio/penv/bin/pio device monitor -b 115200"
