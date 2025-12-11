# Geogram HTTP API

The Geogram firmware provides an HTTP API for device status, configuration, and control. The API is available when the device is connected to WiFi.

## Base URL

When connected to WiFi as a station:
```
http://<device-ip>/
```

When in AP mode (setup):
```
http://192.168.4.1/
```

## Endpoints

### WiFi Configuration

#### `GET /`

Returns the WiFi configuration page (HTML form).

**Response:** HTML page with WiFi setup form.

Used during initial device setup when the device is in AP mode.

---

#### `POST /connect`

Submit WiFi credentials to connect to a network.

**Content-Type:** `application/x-www-form-urlencoded`

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `ssid` | string | Yes | WiFi network name (max 32 chars) |
| `password` | string | No | WiFi password (max 64 chars) |

**Example:**
```bash
curl -X POST http://192.168.4.1/connect \
  -d "ssid=MyNetwork&password=secret123"
```

**Response:** HTML success page. Device will attempt to connect to the specified network.

**Side Effects:**
- Credentials are saved to NVS for auto-reconnection on reboot
- Device will disable AP mode and connect as a station

---

### Status Endpoints

#### `GET /status`

Basic status check endpoint.

**Response:**
```json
{
  "status": "ok",
  "device": "geogram"
}
```

---

#### `GET /api/status`

Full device status with station information. Only available when Station API is enabled (after WiFi connection).

**Response:**
```json
{
  "station": {
    "callsign": "ESPAB12",
    "name": "Geogram Station",
    "version": "1.0.0",
    "uptime": 3600,
    "clients": 0
  },
  "wifi": {
    "status": "connected",
    "ip": "192.168.1.50"
  },
  "sensors": {
    "temperature": 23.5,
    "humidity": 45.2
  },
  "sdcard": {
    "mounted": true,
    "capacity_gb": 7.45
  },
  "heap": {
    "free": 245632
  }
}
```

**Headers:**
- `Access-Control-Allow-Origin: *` (CORS enabled)

**Example:**
```bash
curl http://192.168.1.50/api/status
```

---

### WebSocket (Planned)

#### `WS /ws`

WebSocket endpoint for real-time communication with connected clients.

**Status:** Not currently enabled. Requires `CONFIG_HTTPD_WS_SUPPORT=y` in sdkconfig.

**Planned Message Types:**

**Client -> Server:**
```json
{"type": "HELLO", "callsign": "CLIENT1", "nickname": "User", "platform": "Android"}
```

```json
{"type": "PING"}
```

**Server -> Client:**
```json
{"type": "HELLO_ACK", "success": true, "message": "Welcome"}
```

```json
{"type": "PONG", "timestamp": 1234567890}
```

---

## Station API

The Station API provides information about the Geogram device acting as a local "station" that clients can connect to.

### Callsign

Each device has a unique callsign generated from its MAC address (e.g., `ESPAB12`). This identifies the station on the network.

### Client Management

The station can track up to 8 connected clients (via WebSocket when enabled). Each client has:
- `callsign` - Unique client identifier
- `nickname` - Display name
- `platform` - Client platform (Android, iOS, Linux, etc.)
- `connected_at` - Connection timestamp
- `last_activity` - Last message timestamp

---

## Error Responses

### HTTP 400 Bad Request

Missing or invalid parameters.

```json
{"error": "Missing SSID"}
```

### HTTP 500 Internal Server Error

Server-side error.

```json
{"error": "Failed to process request"}
```

---

## Usage Examples

### Python

```python
import requests

# Get device status
response = requests.get('http://192.168.1.50/api/status')
status = response.json()
print(f"Callsign: {status['station']['callsign']}")
print(f"Uptime: {status['station']['uptime']}s")
print(f"Temperature: {status['sensors']['temperature']}C")

# Configure WiFi (when in AP mode)
requests.post('http://192.168.4.1/connect', data={
    'ssid': 'MyNetwork',
    'password': 'secret123'
})
```

### JavaScript

```javascript
// Fetch device status
fetch('http://192.168.1.50/api/status')
  .then(response => response.json())
  .then(status => {
    console.log(`Callsign: ${status.station.callsign}`);
    console.log(`Temperature: ${status.sensors.temperature}C`);
  });
```

### curl

```bash
# Get status
curl http://192.168.1.50/api/status | jq

# Configure WiFi
curl -X POST http://192.168.4.1/connect \
  -d "ssid=MyNetwork&password=secret123"
```

---

## Device Modes

### AP Mode (Setup)

When no WiFi credentials are saved or connection fails, the device starts in Access Point mode:
- SSID: `Geogram-Setup`
- Password: (none - open network)
- IP: `192.168.4.1`

Connect to this network and navigate to `http://192.168.4.1/` to configure WiFi.

### Station Mode (Connected)

After successful WiFi connection, the device:
- Obtains an IP address via DHCP
- Starts the Station API server on port 80
- Exposes `/api/status` and `/ws` endpoints

---

## Rate Limiting

There is no rate limiting implemented. For polling `/api/status`, a reasonable interval is 1-5 seconds.

---

## Security Considerations

- The HTTP API has no authentication
- WiFi credentials are stored in NVS (non-volatile storage)
- CORS is enabled (`Access-Control-Allow-Origin: *`)
- Intended for local network use only

For production deployments, consider:
- Adding API authentication
- Using HTTPS (requires certificate configuration)
- Restricting CORS origins
