# 02 — Wiring

> Read the **Power** section at the bottom before connecting anything.

## 1. Probe ↔ target (SWD)

| ESP8266 probe | Direction | STM32 Blue Pill |
|---------------|-----------|-----------------|
| GPIO12 (D6) SWCLK | → | PA14 (SWCLK / JTCK) |
| GPIO13 (D7) SWDIO | ↔ | PA13 (SWDIO / JTMS) |
| GPIO14 (D5) NRST  | → | NRST (optional, active-low) |
| GND               | — | GND (**mandatory**, common ground) |
| 3.3V (see Power)  | → | 3.3V |

- **GND is mandatory.** Without a common ground the SWD link will not work.
- **NRST is optional.** The probe can reset the target via SWD (SYSRESETREQ);
  a physical NRST wire is only needed for hard-reset recovery.
- Keep the SWD jumpers short. If the link is unstable, this is the first thing
  to fix; you can also raise `swd_delay_cycles` in the probe firmware (slower
  but more tolerant).

## 2. Sensors, servos, OLED on the target

Board silk = port letter + pin number (`A0` = PA0, `C13` = PC13, …).

| Function | MCU pin | Board label |
|----------|---------|-------------|
| LDR top-left | PA0 | A0 |
| LDR top-right | PA1 | A1 |
| LDR bottom-left | PA4 | A4 |
| LDR bottom-right | PA5 | A5 |
| Servo elevation | PA6 | A6 |
| Servo azimuth | PA8 | A8 |
| OLED SCL | PB6 | B6 |
| OLED SDA | PB7 | B7 |
| Debug serial TX / RX | PA9 / PA10 | A9 / A10 (USART1 @115200) |
| On-board LED | PC13 | C13 (active low) |

Notes:

- Avoid **PC14 / PC15** — they are the 32.768 kHz crystal pins.
- LDRs use PA0/PA1/PA4/PA5 (not PA2/PA3, which are USART2 by default) so the
  ADC pins don't collide with a serial port.
- `Serial` on this board = **USART1** (PA9/PA10). `Wire` = **I2C1**
  (SDA = PB7, SCL = PB6).
- The STM32duino `Servo` library is software-timed (one timer ISR toggling any
  GPIO), so servo pins are free — here azimuth = PA8, elevation = PA6.

## 3. LDR divider circuit

Each LDR forms a voltage divider so brighter light = **higher** ADC reading:

```
   3.3V ──[ LDR ]──┬─── ADC pin (A0 / A1 / A4 / A5)
                   │
                [ 10k ]
                   │
                  GND
```

Mount the four LDRs in quadrants behind a small cross-shaped divider so each
one sees a distinct wedge of the sky. The firmware's error terms are:

- Azimuth error  `dA = (tl + bl) − (tr + br)`  (positive = brighter on the left)
- Elevation error `dE = (tl + tr) − (bl + br)` (positive = brighter on top)

## 4. OLED

- SSD1306 128×64, I2C address **0x3C**, SDA = PB7, SCL = PB6, VCC = 3.3V.
- Needs pull-ups on SDA/SCL (most modules have 10 kΩ on-board). Weak/missing
  pull-ups cause the slow/stalled display transfer noted in troubleshooting.

## 5. Power

- Servos draw significant current and usually want **5V** (only available while
   the STM32's own micro-USB is plugged in). Put a **470–1000 µF** capacitor across
  the servo 5V/GND.
- During development the target was powered from the **ESP8266's 3.3V** pin so
  only one USB cable was needed for flashing.
- **NEVER power the STM32 from both the ESP8266 3.3V and its own USB at the same
  time.**
- **Do not drive the servos while flashing.** Flash with the servo supply off.
