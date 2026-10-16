# 07 — Project Wiring (connect it up)

This is the full hookup for the finished tracker running
`firmware/SolarTracker/SolarTracker.ino`.

Board silk = port letter + pin number: `A0` = PA0, `A8` = PA8, `B6` = PB6,
`C13` = PC13.

## Quick pin table

| Function | STM32 pin | Board label | Connects to |
|----------|-----------|-------------|-------------|
| LDR top-left | PA0 | A0 | divider node 1 |
| LDR top-right | PA1 | A1 | divider node 2 |
| LDR bottom-left | PA4 | A4 | divider node 3 |
| LDR bottom-right | PA5 | A5 | divider node 4 |
| Servo **azimuth** signal | PA8 | A8 | servo 1 signal (orange) |
| Servo **elevation** signal | PA6 | A6 | servo 2 signal (orange) |
| OLED SCL | PB6 | B6 | OLED SCL |
| OLED SDA | PB7 | B7 | OLED SDA |
| Debug serial TX / RX | PA9 / PA10 | A9 / A10 | optional USB-TTL RX / TX |
| Status LED | PC13 | C13 | on-board (nothing to wire) |
| 3.3V | 3V3 | 3V3 | LDR tops, OLED VCC |
| GND | G | GND | LDR bottoms, OLED GND, servo GND, common |

Avoid PC14 / PC15 (32.768 kHz crystal pins).

## 1. Probe → target (only needed to FLASH)

Wire the ESP8266 probe to the STM32 to program it, then you can disconnect:

| ESP8266 probe | STM32 |
|---------------|-------|
| GPIO12 (D6) | PA14 (SWCLK) |
| GPIO13 (D7) | PA13 (SWDIO) |
| GND | GND |
| 3.3V | 3V3 *(only if the STM32's own USB is unplugged)* |

**Never power the STM32 from both the ESP 3.3V and its own USB at once.**

## 2. LDR dividers (one per channel)

```
   3.3V ──[ LDR ]──┬─── A0 / A1 / A4 / A5
                   │
                [ 10k ]
                   │
                  GND
```

Brighter light → **higher** reading on that channel. Mount the four LDRs in
quadrants behind a small **cross-shaped divider** so each sees a distinct
wedge of the sky. Confirm each with `host/read_ldr_test.py` before final
assembly (cover one → only its column drops).

## 3. Servos

| Servo | Signal → | Power |
|-------|----------|-------|
| Azimuth (horizontal) | PA8 (A8) | 5V + GND |
| Elevation (vertical) | PA6 (A6) | 5V + GND |

- Orange wire = signal, red = +5V, brown/black = GND.
- **Power the servos from a 5V supply** (not the STM32 3.3V pin). A common
  ground between the supply, the servos and the STM32 is **required**.
- Put an electrolytic capacitor across the servo 5V/GND to absorb the current
  inrush — **470–1000 µF** is ideal, ~100 µF the minimum.
- If an axis turns the *wrong way* when you shade an LDR, flip its `*_INVERT`
  in the firmware (or swap the servo mechanically).

## 4. OLED (optional)

| OLED | STM32 |
|------|-------|
| SCL | PB6 (B6) |
| SDA | PB7 (B7) |
| VCC | 3.3V |
| GND | GND |

- Most SSD1306 modules have on-board pull-ups. If the screen is blank or the
  firmware prints `OLED skipped`, add **4.7 kΩ** resistors SDA→3.3V and
  SCL→3.3V and keep the wires short.
- The firmware **auto-detects** a slow OLED at boot and disables it so tracking
  stays fast — you don't have to change code, but the screen won't update until
  the I2C is healthy.

## 5. Power summary

- **STM32:** 5V pin from USB, or 3.3V from the probe while flashing (pick one).
- **Servos:** separate 5V supply, common ground, 470–1000 µF bulk cap.
- **LDRs + OLED:** 3.3V from the STM32 board.

## Order of operations

1. Flash with the probe (servos off), see `docs/03-build-and-flash.md`.
2. Power the servos and OLED.
3. Watch it track. If an axis runs away: check `*_INVERT`, the LDR quadrature
   orientation, and the servo signal pins.

## Live check over SWD (no serial needed)

```powershell
python host\read_ldr_test.py COM9   # the standalone LDR test sketch
```
For the project firmware, `host/read_ldr.py` (points at
`firmware/SolarTracker/build/SolarTracker.ino.elf`) reads `tl/tr/bl/br` and the
`azAngle/elAngle` live.
