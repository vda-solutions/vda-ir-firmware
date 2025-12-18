# VDA IR Firmware REST API Reference

The firmware exposes a REST API on port 80 for all operations.

## Discovery

Boards advertise via mDNS as `vda-ir-XXXXXX.local` where XXXXXX is the last 6 characters of the MAC address.

## Endpoints

### GET /info

Returns board information and firmware version.

**Response:**
```json
{
  "board_id": "vda-ir-abc123",
  "firmware_version": "1.2.1",
  "board_type": "ESP32-POE-ISO",
  "mac_address": "AA:BB:CC:DD:EE:FF",
  "ip_address": "192.168.1.100",
  "uptime": 3600
}
```

### GET /status

Returns current port configuration and status.

**Response:**
```json
{
  "ports": [
    {
      "port": 1,
      "gpio": 4,
      "mode": "output",
      "name": "IR Output 1"
    },
    {
      "port": 2,
      "gpio": 36,
      "mode": "input",
      "name": "IR Receiver"
    }
  ]
}
```

### POST /ports/configure

Configure a GPIO port.

**Request:**
```json
{
  "port": 1,
  "gpio": 4,
  "mode": "output",
  "name": "Living Room IR"
}
```

**Response:**
```json
{
  "success": true,
  "message": "Port configured"
}
```

### POST /send_ir

Send an IR code.

**Request:**
```json
{
  "port": 1,
  "protocol": "NEC",
  "code": "20DF10EF",
  "bits": 32
}
```

**Protocols Supported:**
- `NEC` - Most common (LG, Vizio, TCL, etc.)
- `SAMSUNG` - Samsung TVs
- `SONY` - Sony devices (12, 15, or 20 bit)
- `PIONEER` - Pioneer AV receivers
- `RC5`, `RC6` - Philips protocol
- `RAW` - Raw timing data

**Response:**
```json
{
  "success": true,
  "message": "IR code sent"
}
```

### POST /test_output

Test an IR output port by sending a test signal.

**Request:**
```json
{
  "port": 1
}
```

### POST /learning/start

Start IR learning mode on a receiver port.

**Request:**
```json
{
  "port": 2
}
```

**Response:**
```json
{
  "success": true,
  "message": "Learning started"
}
```

### GET /learning/status

Get the status of IR learning.

**Response (learning in progress):**
```json
{
  "status": "learning",
  "message": "Waiting for IR signal..."
}
```

**Response (code captured):**
```json
{
  "status": "captured",
  "protocol": "NEC",
  "code": "20DF10EF",
  "bits": 32,
  "raw": [9000, 4500, 560, 560, ...]
}
```

### POST /learning/stop

Stop IR learning mode.

**Response:**
```json
{
  "success": true,
  "message": "Learning stopped"
}
```

### POST /adopt

Adopt the board with a custom name.

**Request:**
```json
{
  "board_name": "Living Room Controller"
}
```

## WiFi-Only Endpoints

These endpoints are only available on ESP32 DevKit (WiFi) boards.

### GET /wifi/scan

Scan for available WiFi networks.

**Response:**
```json
{
  "networks": [
    {
      "ssid": "MyNetwork",
      "rssi": -45,
      "encryption": "WPA2"
    }
  ]
}
```

### POST /wifi/config

Configure WiFi credentials.

**Request:**
```json
{
  "ssid": "MyNetwork",
  "password": "mypassword"
}
```

## Error Responses

All endpoints return errors in this format:

```json
{
  "success": false,
  "error": "Error description"
}
```

**HTTP Status Codes:**
- `200` - Success
- `400` - Bad request (invalid parameters)
- `404` - Endpoint not found
- `500` - Internal server error
