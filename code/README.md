# Geogram ESP32 Firmware

ESP32 firmware for Geogram weather stations with e-paper display support.

## Features

- **E-Paper Display** - 1.54" black/white e-paper with LVGL UI
- **Environmental Sensors** - Temperature and humidity (SHTC3)
- **Real-Time Clock** - PCF85063 RTC with battery backup
- **WiFi Connectivity** - Station mode with AP fallback for configuration
- **NOSTR Integration** - Secp256k1 key generation with bech32 npub/nsec encoding
- **Station API** - HTTP REST API for weather data and device control
- **Remote Access** - SSH, Telnet, and Serial console for device management
- **SD Card Support** - Optional SD card for data logging

## Supported Boards

| Board | Model | Display | Status |
|-------|-------|---------|--------|
| ESP32-S3 ePaper 1.54" | `esp32s3_epaper_1in54` | Waveshare 200x200 | Full |
| ESP32 Generic | `esp32_generic` | None | Skeleton |

## Building

### Prerequisites

- [PlatformIO](https://platformio.org/) (VS Code extension or CLI)
- ESP-IDF 5.2.1 (automatically installed by PlatformIO)

### Build Commands

```bash
# Build default environment (esp32s3_epaper_1in54)
pio run

# Build specific environment
pio run -e esp32s3_epaper_1in54
pio run -e esp32_generic

# Upload firmware
pio run -t upload

# Monitor serial output
pio device monitor
```

### Output

Firmware binaries are copied to `firmware/` directory:
- `geogram-ESP32S3-ePaper1in54.bin`
- `geogram-ESP32S3-ePaper1in54.elf`

## Remote Access

The device provides multiple ways to access the command-line interface:

### Serial Console

Connect via USB serial at 115200 baud. The console starts automatically on boot.

```bash
pio device monitor
# or
screen /dev/ttyUSB0 115200
```

### SSH (Port 22)

Secure shell access when connected to WiFi.

```bash
ssh root@<device-ip>
```

**Features:**
- Passwordless login by default
- RSA 2048-bit host key (generated on first boot, stored in NVS)
- Password can be set via CLI command

### Telnet (Port 23)

Unencrypted remote access (for legacy clients or debugging).

```bash
telnet <device-ip>
```

## CLI Commands

### System Commands

| Command | Description |
|---------|-------------|
| `help` | Show available commands |
| `version` | Show firmware version |
| `info` | Show system information |
| `restart` | Reboot the device |
| `free` | Show memory usage |
| `tasks` | List FreeRTOS tasks |

### WiFi Commands

| Command | Description |
|---------|-------------|
| `wifi status` | Show WiFi connection status |
| `wifi scan` | Scan for available networks |
| `wifi connect <ssid> [password]` | Connect to a network |
| `wifi disconnect` | Disconnect from current network |
| `wifi forget` | Clear saved credentials |

### Display Commands

| Command | Description |
|---------|-------------|
| `display refresh` | Force display refresh |
| `display clear` | Clear the display |
| `display rotate <0\|90\|180\|270>` | Set screen rotation |

### Configuration Commands

| Command | Description |
|---------|-------------|
| `config show` | Show current configuration |
| `config callsign [value]` | Get/set station callsign |
| `config output <text\|json>` | Set output format |

### SSH Commands

| Command | Description |
|---------|-------------|
| `ssh status` | Show SSH server status and fingerprint |
| `ssh password <password>` | Set SSH password |
| `ssh password clear` | Remove password (enable passwordless login) |

### SD Card Commands

| Command | Description |
|---------|-------------|
| `sd status` | Show SD card status |
| `sd mount` | Mount SD card |
| `sd unmount` | Unmount SD card |
| `sd ls [path]` | List directory contents |

## HTTP API

When connected to WiFi, the device runs an HTTP server with the following endpoints:

### Status Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/status` | GET | Basic device status |
| `/api/status` | GET | Full station status (JSON) |
| `/api/sensors` | GET | Current sensor readings |

### Configuration Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/config` | GET | Get device configuration |
| `/api/config` | POST | Update configuration |

### Example Response

```json
{
  "callsign": "X3ABCD",
  "npub": "npub1abc...",
  "firmware": "1.0.0",
  "uptime": 3600,
  "wifi": {
    "ssid": "MyNetwork",
    "rssi": -45,
    "ip": "192.168.1.100"
  },
  "sensors": {
    "temperature": 22.5,
    "humidity": 45.2
  }
}
```

## Station Callsign

Each device generates a unique NOSTR keypair on first boot:
- **Private key (nsec)** - Stored securely in NVS
- **Public key (npub)** - Used to derive the station callsign

The callsign format is `X3` + first 4 characters of npub (after "npub1"), uppercased.
Example: `npub1abc...` becomes callsign `X3ABC`.

## Project Structure

```
code/
├── src/
│   └── main.cpp              # Application entry point
├── include/
│   └── app_config.h          # Board model definitions
├── components/
│   ├── geogram_console/      # Serial/Telnet/SSH CLI
│   ├── geogram_ssh/          # SSH server wrapper
│   ├── geogram_telnet/       # Telnet server
│   ├── geogram_wifi/         # WiFi management
│   ├── geogram_http/         # HTTP server
│   ├── geogram_station/      # Station API
│   ├── geogram_nostr/        # NOSTR key generation
│   ├── geogram_epaper_1in54/ # E-paper driver
│   ├── geogram_lvgl/         # LVGL port
│   ├── geogram_ui/           # UI screens
│   ├── geogram_shtc3/        # Temperature/humidity sensor
│   ├── geogram_pcf85063/     # RTC driver
│   ├── geogram_sdcard/       # SD card support
│   └── ...
├── managed_components/       # ESP Component Registry deps
├── firmware/                 # Build output
├── platformio.ini            # PlatformIO configuration
└── sdkconfig.defaults        # ESP-IDF defaults
```

## Memory Usage

Typical memory usage for ESP32-S3 with full features:

| Resource | Used | Total | Percentage |
|----------|------|-------|------------|
| RAM | ~86 KB | 320 KB | 27% |
| Flash | ~1.4 MB | 4 MB | 35% |

## Configuration

### sdkconfig.defaults

Key configuration options:

```ini
# PSRAM (required for LVGL buffer)
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_QUAD=y

# WiFi
CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=10

# SSH Authentication
CONFIG_EXAMPLE_ALLOW_PASSWORD_AUTH=y
CONFIG_EXAMPLE_ALLOW_PUBLICKEY_AUTH=n
```

### Build Flags

Board-specific features are enabled via build flags in `platformio.ini`:

```ini
-DBOARD_MODEL=MODEL_ESP32S3_EPAPER_1IN54
-DHAS_EPAPER_DISPLAY=1
-DHAS_RTC=1
-DHAS_HUMIDITY_SENSOR=1
-DHAS_PSRAM=1
```

## WiFi Setup

On first boot (or when no credentials are saved):

1. Device starts in AP mode: **SSID: "Geogram-Setup"** (open network)
2. Connect to the AP with a phone/laptop
3. Open `http://192.168.4.1` in a browser
4. Enter your WiFi credentials
5. Device reboots and connects to your network

## License

MIT License - See LICENSE file for details.

## Related Projects

- [Geogram Desktop](../geogram-desktop/) - Desktop application for station management
