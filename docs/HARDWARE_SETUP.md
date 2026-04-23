# Hardware Setup Guide

## Components Needed

### For IR Transmission
- IR LED (940nm recommended)
- 100Ω resistor
- NPN transistor (2N2222 or similar) for better range

### For IR Reception
- IR receiver module (TSOP38238 or VS1838B)
- 100nF capacitor (optional, for noise filtering)

## Wiring Diagrams

### Simple IR LED (Direct GPIO)

```
ESP32 GPIO ──┬── 100Ω ───── IR LED (+) ──── GND
             │
            └── (cathode/short leg)
```

**Range:** ~3-5 meters

### Transistor-Boosted IR LED (Recommended)

```
                        ┌─── IR LED (+)
                        │
ESP32 GPIO ── 1kΩ ──┬── NPN Collector
                    │
                   Base
                    │
                  Emitter ──── GND
                        │
                       └── IR LED (-)
```

**Range:** ~8-12 meters

### IR Receiver Module

```
TSOP38238 / VS1838B

    ┌───────┐
    │  OUT  ├──── ESP32 GPIO (input)
    │  GND  ├──── GND
    │  VCC  ├──── 3.3V
    └───────┘
```

## GPIO Recommendations

### Olimex ESP32-POE-ISO

**Available for IR:**
- Output: GPIO 4, 5, 13, 14, 15, 16, 32, 33
- Input (receiver): GPIO 34, 35, 36, 39 (input-only)

**Avoid (Ethernet PHY):**
- GPIO 17, 18, 19, 21, 22, 23, 25, 26, 27

**Default Configuration:**
| Port | GPIO | Function |
|------|------|----------|
| 1 | 4 | IR Output |
| 2 | 5 | IR Output |
| 3 | 36 | IR Receiver |

### ESP32 DevKit (WiFi)

**Available for IR:**
- Output: Most GPIOs except boot-strapping pins
- Input: Any GPIO

**Avoid:**
- GPIO 0 (boot mode)
- GPIO 2 (built-in LED, can be used but blinks on boot)
- GPIO 12 (affects flash voltage on some boards)

**Default Configuration:**
| Port | GPIO | Function |
|------|------|----------|
| 1 | 4 | IR Output |
| 2 | 5 | IR Output |
| 3 | 15 | IR Receiver |

## Multiple IR LEDs

For controlling devices in different directions, you can use multiple IR outputs:

```
ESP32 GPIO4 ─── 100Ω ─── IR LED 1 (front)
ESP32 GPIO5 ─── 100Ω ─── IR LED 2 (left)
ESP32 GPIO13 ── 100Ω ─── IR LED 3 (right)
```

Each can be configured as a separate port and assigned to different devices.

## Power Supply

### Olimex ESP32-POE-ISO
- PoE (802.3af) via Ethernet port
- External 5V via barrel jack
- USB (for programming only)

### ESP32 DevKit
- USB 5V
- External 5V via VIN pin

## Enclosure Tips

- Use IR-transparent plastic for the LED window
- Position receiver away from LEDs to avoid interference
- Ensure adequate ventilation for the ESP32
