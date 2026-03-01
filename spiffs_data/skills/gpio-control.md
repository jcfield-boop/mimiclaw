# GPIO Control

Control the ESP32-C6 GPIO pins directly from a conversation.

## When to use
When the user asks to control hardware pins — relays, LEDs, sensors, switches.

## Available tools
- `gpio_mode` — configure a pin as input or output before first use
- `gpio_read` — read the current logic level of a pin
- `gpio_write` — set a pin HIGH or LOW (output pins only)

## Safe pins
Valid GPIO pins: **0–22**, excluding:
- Pin 8 — WS2812 RGB LED (board built-in, controlled by firmware)
- Pin 9 — BOOT button (strapping, input-only)
- Pins 18–21 — SPI flash interface (internal to module)

Safe for general use: 0–7, 10–17, 22

## Workflow

### Read a pin
```
1. Call gpio_read: {"pin": 4}
→ Returns: {"pin":4,"state":"HIGH","raw":1}
```

### Control an output (e.g. relay on pin 5)
```
1. Call gpio_mode: {"pin": 5, "mode": "output"}  ← configure first
2. Call gpio_write: {"pin": 5, "state": "HIGH"}   ← activate relay
3. Call gpio_write: {"pin": 5, "state": "LOW"}    ← deactivate relay
```

### Read a sensor / button
```
1. Call gpio_mode: {"pin": 4, "mode": "input_pullup"}
2. Call gpio_read: {"pin": 4}
→ LOW = button pressed (pullup logic), HIGH = not pressed
```

## Named pins (from SERVICES.md)
If a `## GPIO` section exists in /spiffs/config/SERVICES.md, refer to pins by name
instead of number. Example:
```
## GPIO
relay: 5
door_sensor: 4
status_led: 10
```

## Safety rules
- Always configure mode before reading or writing a new pin
- Never attempt to control flash (18-21), LED (8), or BOOT (9) pins
- Relay control: always confirm OFF state before ending a session
- If gpio_write returns an error, the pin may not be configured as output —
  call gpio_mode first

## Example use cases
- "Turn on the relay on pin 5" → gpio_mode + gpio_write HIGH
- "Check if the door sensor is open" → gpio_read pin 4
- "Blink the LED on pin 10" → gpio_mode output, gpio_write HIGH, wait, gpio_write LOW
- "Read all sensor pins" → gpio_read each pin individually
