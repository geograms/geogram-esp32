# Geogram ESP32 Station Firmware

Custom firmware for various ESP32 boards, enabling them to function as **network stations** for the **Geogram mesh network**. ESP32 stations serve as bridge nodes that connect Geogram devices (smartphones, T-Dongles, radios) to WiFi networks, acting as relay points for offline-first communication.

---

## Overview

In the Geogram ecosystem, ESP32 stations serve as **infrastructure nodes** that:

- **Bridge networks**: Connect BLE mesh networks to WiFi/Internet when available
- **Relay messages**: Store-and-forward message delivery for offline scenarios
- **Provide local services**: HTTP configuration portal, status display, sensor data
- **Act as beacons**: Broadcast presence to nearby Geogram devices

Unlike the [geogram-tdongle](../geogram-tdongle/) which is a portable BLE-only device, ESP32 stations are designed for **semi-permanent installation** as network access points.

---

## Supported Boards

| Board | Status | Features |
|-------|--------|----------|
| **ESP32-S3 ePaper 1.54"** | Active | E-paper display, WiFi AP, RTC, Temp/Humidity sensor |
| **ESP32 Generic** | Skeleton | Basic WiFi, no display |

### ESP32-S3 ePaper 1.54" (Waveshare)

The primary supported board with full feature implementation:

- **Display**: 1.54" e-paper (200x200 pixels) with LVGL graphics
- **Sensors**: SHTC3 temperature/humidity sensor
- **RTC**: PCF85063 real-time clock for accurate timekeeping
- **WiFi**: Station mode + Access Point mode for configuration
- **Power**: Battery monitoring, low-power capable

---

## Features

- **WiFi Configuration Portal**
  - Starts in AP mode ("Geogram-Setup") when no credentials saved
  - Web-based configuration page for entering WiFi credentials
  - Credentials stored in NVS (non-volatile storage)

- **E-Paper Display UI**
  - Temperature and humidity readings
  - Time and date display (from RTC)
  - WiFi connection status and IP address
  - Status messages

- **Sensor Monitoring**
  - Temperature and humidity updates every 30 seconds
  - Display refresh every 60 seconds to prevent ghosting

- **Modular Architecture**
  - Component-based design for easy board support
  - Board-specific model initialization
  - Shared components across different boards

---

## Quick Start

### Prerequisites

- [PlatformIO](https://platformio.org/) CLI or IDE extension
- USB-C cable
- ESP32-S3 ePaper board (or supported variant)

### Build and Flash

```bash
# Clone the repository
git clone https://github.com/geograms/central.git
cd central/geogram-esp32/code

# Build firmware
~/.platformio/penv/bin/pio run -e esp32s3_epaper_1in54

# Upload firmware
./upload.sh

# Monitor serial output
./monitor.sh
```

### Manual Commands

```bash
# Build specific environment
pio run -e esp32s3_epaper_1in54

# Upload with built-in JTAG (ESP32-S3)
pio run -e esp32s3_epaper_1in54 -t upload

# Serial monitor
pio device monitor -b 115200
```

---

## First Boot

1. **Power on** the device - it will start in AP mode
2. **Connect** to WiFi network "Geogram-Setup" (open network)
3. **Navigate** to `http://192.168.4.1` in your browser
4. **Enter** your WiFi network credentials
5. **Wait** for the device to connect and display the new IP address

After successful configuration, the device will:
- Connect to your WiFi network automatically on future boots
- Display temperature, humidity, time, and connection status
- Be ready to receive Geogram network connections

---

## Project Structure

```
geogram-esp32/
├── code/
│   ├── src/
│   │   └── main.cpp              # Main application entry point
│   ├── include/
│   │   └── app_config.h          # Application configuration
│   ├── components/
│   │   ├── geogram_common/       # Shared types and utilities
│   │   ├── geogram_epaper_1in54/ # E-paper display driver
│   │   ├── geogram_http/         # HTTP configuration server
│   │   ├── geogram_i2c/          # I2C bus abstraction
│   │   ├── geogram_lvgl/         # LVGL graphics port
│   │   ├── geogram_model_*/      # Board-specific initialization
│   │   ├── geogram_pcf85063/     # RTC driver
│   │   ├── geogram_shtc3/        # Temperature/humidity sensor
│   │   ├── geogram_ui/           # UI components
│   │   ├── geogram_wifi/         # WiFi management
│   │   └── geogram_button/       # Button input handling
│   ├── boards/                   # Board-specific sdkconfig
│   ├── firmware/                 # Pre-built firmware binaries
│   ├── scripts/                  # Build scripts
│   ├── platformio.ini            # PlatformIO configuration
│   ├── upload.sh                 # Upload helper script
│   └── monitor.sh                # Serial monitor helper
├── board-originals/              # Original board libraries/examples
├── docs/                         # Documentation
└── README.md                     # This file
```

---

## Configuration

### Build Flags

Key build flags in `platformio.ini`:

| Flag | Description |
|------|-------------|
| `BOARD_MODEL` | Board type identifier |
| `BOARD_NAME` | Human-readable board name |
| `HAS_EPAPER_DISPLAY` | Enable e-paper display support |
| `HAS_RTC` | Enable RTC support |
| `HAS_HUMIDITY_SENSOR` | Enable temperature/humidity sensor |
| `HAS_PSRAM` | Enable PSRAM support |

### WiFi Settings

Default AP mode configuration (in `main.cpp`):

```c
#define WIFI_AP_SSID        "Geogram-Setup"
#define WIFI_AP_PASSWORD    ""  // Open network
#define WIFI_AP_CHANNEL     1
#define WIFI_AP_MAX_CONN    4
```

---

## Adding New Board Support

1. **Create model component**: `components/geogram_model_<board>/`
   - `model_config.h` - Board-specific defines
   - `model_init.h` - Initialization function declarations
   - `model_init.c` - Hardware initialization implementation

2. **Add PlatformIO environment** in `platformio.ini`:
   ```ini
   [env:new_board]
   platform = espressif32@6.4.0
   board = your_board
   build_flags =
       -DBOARD_MODEL=MODEL_NEW_BOARD
       -DBOARD_NAME=\"New-Board\"
   ```

3. **Update `app_config.h`** with new model constant

4. **Add conditional compilation** in `main.cpp` for board-specific features

---

## Components

### Core Components

| Component | Description |
|-----------|-------------|
| `geogram_wifi` | WiFi station and AP mode management |
| `geogram_http` | HTTP server for web configuration |
| `geogram_common` | Shared types and utilities |

### Board-Specific Components

| Component | Description |
|-----------|-------------|
| `geogram_epaper_1in54` | Waveshare 1.54" e-paper driver |
| `geogram_shtc3` | Sensirion SHTC3 temp/humidity sensor |
| `geogram_pcf85063` | NXP PCF85063 RTC driver |
| `geogram_lvgl` | LVGL graphics library port |
| `geogram_ui` | Geogram UI widgets and screens |

---

## Development

### Serial Output

The firmware outputs debug information at 115200 baud:

```
=====================================
  Geogram Firmware v1.0.0
  Board: ESP32S3-ePaper-1.54
  Model: ESP32-S3 ePaper 1.54"
=====================================
Board initialized successfully
E-paper display: 200x200
Starting AP mode for WiFi configuration
```

### Troubleshooting

**Upload fails**
- Use the built-in JTAG protocol (default for ESP32-S3)
- Try reducing upload speed: `upload_speed = 115200`
- Hold BOOT button while connecting

**Display not updating**
- Check I2C connections
- Verify component initialization in serial log
- E-paper partial refresh may have ghosting - wait for full refresh

**WiFi not connecting**
- Delete saved credentials: clear NVS partition
- Check serial log for connection errors
- Ensure correct SSID and password

---

## Integration with Geogram Network

ESP32 stations integrate with the broader Geogram ecosystem:

```
┌──────────────┐         ┌──────────────┐         ┌──────────────┐
│   Geogram    │   BLE   │    ESP32     │  WiFi   │   Internet   │
│   Devices    │◄───────►│   Station    │◄───────►│   / Relay    │
│ (Phones/K5)  │         │              │         │   Server     │
└──────────────┘         └──────────────┘         └──────────────┘
                               │
                               ▼
                        ┌──────────────┐
                        │   E-Paper    │
                        │   Display    │
                        └──────────────┘
```

**Current capabilities**:
- WiFi network bridging
- Status display
- Environmental monitoring

**Planned features**:
- BLE beacon for device discovery
- Message relay and forwarding
- NOSTR relay connection
- Power management and deep sleep

---

## Related Projects

| Project | Description |
|---------|-------------|
| [geogram-tdongle](../geogram-tdongle/) | Portable ESP32-S3 BLE beacon |
| [geogram-android](../geogram-android/) | Android mobile app |
| [geogram-relay](../geogram-relay/) | WebSocket relay server |
| [central](../) | Main Geogram documentation |

---

## License

Licensed under the [Apache License 2.0](https://www.apache.org/licenses/LICENSE-2.0)

---

## Contributors

**Primary Contributor**
Max Brito (Portugal/Germany) - 2025-present
- ESP32 architecture, WiFi management, e-paper integration

See full list: [`CONTRIBUTORS.md`](https://github.com/geograms/central/blob/main/CONTRIBUTORS.md)
