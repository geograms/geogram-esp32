#!/bin/bash
# Monitor serial output from ESP32-S3 ePaper device
# Usage: ./monitor.sh [port]

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# Default port (auto-detect if not specified)
PORT="$1"

# Auto-detect port if not specified
if [ -z "$PORT" ]; then
    if [ -e /dev/ttyACM0 ]; then
        PORT="/dev/ttyACM0"
    elif [ -e /dev/ttyUSB0 ]; then
        PORT="/dev/ttyUSB0"
    else
        echo "Error: No serial port found. Please specify port as argument."
        echo "Usage: $0 [port]"
        exit 1
    fi
fi

echo "=== Geogram Serial Monitor ==="
echo "Port: $PORT"
echo "Baud: 115200"
echo "Press Ctrl+C to exit"
echo ""

~/.platformio/penv/bin/pio device monitor -p "$PORT" -b 115200
