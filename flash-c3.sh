#!/bin/bash
# Flash firmware to ESP32-C3 (PlatformIO)
# Usage: ./flash-c3.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR/code"

ENV="esp32c3_mini"

echo "=== Geogram ESP32-C3 Flash ==="
~/.platformio/penv/bin/pio run -e "$ENV" -t upload
