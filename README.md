# VDA IR Control Firmware

ESP32 firmware for IR transmission and reception, designed for home automation control of TVs, AV receivers, and other IR-controllable devices.

## Supported Boards

| Board | Connection | Features |
|-------|------------|----------|
| **Olimex ESP32-POE-ISO** | Ethernet + PoE | Isolated PoE, stable connection |
| **ESP32 DevKit** | WiFi | Easy setup, captive portal |

## Quick Start

### Download Pre-built Firmware

Download the latest release from the [Releases](https://github.com/vda-solutions/vda-ir-firmware/releases) page:

- `firmware-esp32-poe-iso-vX.X.X.bin` - For Olimex ESP32-POE-ISO
- `firmware-esp32-devkit-wifi-vX.X.X.bin` - For ESP32 DevKit

### Flash Using esptool

```bash
# Install esptool
pip install esptool

# Flash to ESP32 (replace COM port and firmware file)
esptool.py --chip esp32 --port /dev/ttyUSB0 write_flash 0x0 firmware-esp32-poe-iso-v1.2.5.bin
```

### Flash Using Web Flasher

Visit [ESP Web Tools](https://esphome.github.io/esp-web-tools/) and select your firmware file.

## Building from Source

### Prerequisites

- [PlatformIO](https://platformio.org/) (CLI or IDE)
- USB cable for your ESP32 board

### Build Commands

```bash
# Build for both boards
cd firmware && pio run

# Build for specific board
pio run -e esp32-poe-iso
pio run -e esp32-devkit

# Upload to connected board
pio run -e esp32-devkit -t upload

# Monitor serial output
pio run -t monitor
```

### Create Merged Binary (for distribution)

```bash
VERSION="1.2.5"

# POE-ISO
pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32 merge_bin \
  -o "../releases/firmware-esp32-poe-iso-v${VERSION}.bin" \
  --flash_mode dio --flash_size 4MB \
  0x1000 .pio/build/esp32-poe-iso/bootloader.bin \
  0x8000 .pio/build/esp32-poe-iso/partitions.bin \
  0x10000 .pio/build/esp32-poe-iso/firmware.bin

# DevKit WiFi
pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32 merge_bin \
  -o "../releases/firmware-esp32-devkit-wifi-v${VERSION}.bin" \
  --flash_mode dio --flash_size 4MB \
  0x1000 .pio/build/esp32-devkit/bootloader.bin \
  0x8000 .pio/build/esp32-devkit/partitions.bin \
  0x10000 .pio/build/esp32-devkit/firmware.bin
```

## GPIO Pin Mapping

### Olimex ESP32-POE-ISO

| Function | Recommended GPIO | Notes |
|----------|-----------------|-------|
| IR Output 1 | GPIO 4 | Main IR LED |
| IR Output 2 | GPIO 5 | Secondary IR LED |
| IR Receiver | GPIO 36 | Input-only pin |

**Reserved for Ethernet**: 17, 18, 19, 21, 22, 23, 25, 26, 27

### ESP32 DevKit (WiFi)

| Function | Recommended GPIO | Notes |
|----------|-----------------|-------|
| IR Output 1 | GPIO 4 | Main IR LED |
| IR Output 2 | GPIO 5 | Secondary IR LED |
| IR Receiver | GPIO 15 | Any input pin |
| Status LED | GPIO 2 | Built-in LED |

## REST API

See [docs/API_REFERENCE.md](docs/API_REFERENCE.md) for full API documentation.

### Quick Reference

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/info` | GET | Board info and firmware version |
| `/status` | GET | Port status |
| `/send_ir` | POST | Send IR code |
| `/learning/start` | POST | Start IR learning |
| `/learning/status` | GET | Get learned code |

## Changelog

### v1.2.5
- Improved WiFi auto-reconnect stability
- OTA update improvements
- Bug fixes for IR timing

### v1.2.4
- Enhanced captive portal for WiFi setup
- Memory optimization

### v1.2.3
- Fixed IR output timing issues
- Improved board discovery reliability

### v1.2.2
- Added support for more GPIO pins
- Stability improvements

### v1.2.1
- Initial stable release
- Support for ESP32 DevKit and Olimex POE-ISO
- OTA firmware updates
- REST API for IR control

## Related Repositories

- [vda-ir-control](https://github.com/vda-solutions/vda-ir-control) - Home Assistant Integration
- [vda-ir-control-admin-card](https://github.com/vda-solutions/vda-ir-control-admin-card) - Admin Lovelace Card
- [vda-ir-remote-card](https://github.com/vda-solutions/vda-ir-remote-card) - Remote Control Card
- [vda-ir-profiles](https://github.com/vda-solutions/vda-ir-profiles) - Community IR Profiles

## License

MIT License - See [LICENSE](LICENSE) for details.
