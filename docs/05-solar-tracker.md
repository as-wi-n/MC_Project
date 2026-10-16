# 05 — Dual-Axis Solar Tracker Firmware

File: `firmware/SolarTracker/SolarTracker.ino`  (project firmware)
Target: STM32F103 (Blue Pill), STM32duino core.

## Behaviour

1. **Sense** — median of 9 ADC samples per LDR, then a light EMA
   (`LDR_EMA_SHIFT=1`, i.e. 1/2) to smooth the value.
2. **Errors** — `dA = (tl+bl) − (tr+br)` (azimuth), `dE = (tl+tr) − (bl+br)`
   (elevation).
3. **Day/night** — with hysteresis: go to NIGHT when the brightest LDR drops
   below `NIGHT_ON`; wake when it rises above `NIGHT_OFF`.
4. **Decide target** — inside the deadband the axis holds; outside it, step a
   proportional amount (capped at `AZ/EL_MAX_STEP_DEG`) toward the brighter
   side. At night park at `PARK_AZ`/`PARK_EL`.
5. **Actuate** — write the servos only when an angle actually changes.
6. **Report** — update the OLED and print a serial line every `OLED_UPDATE_MS`.

The on-board LED (PC13, active low) is on while tracking, off when parked.

## Pin map

| Function | Pin |
|----------|-----|
| LDR top-left / top-right / bottom-left / bottom-right | PA0 / PA1 / PA4 / PA5 |
| Servo azimuth / elevation | PA8 / PA6 |
| OLED SDA / SCL (I2C1) | PB7 / PB6 |
| Debug serial (USART1) | PA9 / PA10 @115200 |
| Status LED | PC13 (active low) |

## Tunables (top of the `.ino`)

| Define | Default | Meaning |
|--------|---------|---------|
| `LDR_SAMPLES` | 9 | ADC reads per reading (median) |
| `LDR_EMA_SHIFT` | 1 | EMA smoothing 1/2 (heavier → error lags → hunting) |
| `DEADBAND` | 60 | ADC counts; ignore smaller errors |
| `UPDATE_MS` | 60 | control-loop period (ms) |
| `AZ_GAIN_DIV` / `EL_GAIN_DIV` | 40 | proportional step = `\|err\|/DIV` |
| `AZ_MAX_STEP_DEG` / `EL_MAX_STEP_DEG` | 2 / 2 | max degrees per update (33 °/s) |
| `OLED_UPDATE_MS` | 250 | OLED + serial report period |
| `AZ_MIN` / `AZ_MAX` | 0 / 180 | azimuth limits |
| `EL_MIN` / `EL_MAX` | 10 / 90 | elevation limits |
| `AZ_CENTER` / `EL_CENTER` | 90 / 50 | power-on angles |
| `AZ_INVERT` / `EL_INVERT` | 1 / 1 | flip to match servo mount |
| `NIGHT_ON` / `NIGHT_OFF` | 120 / 260 | night threshold + wake hysteresis |
| `PARK_AZ` / `PARK_EL` | 90 / 10 | night rest position |
| `OLED_ADDR` | 0x3C | SSD1306 I²C address |

If an axis moves the **wrong way** when you cover an LDR, flip the matching
`*_INVERT`. If it jitters, raise `DEADBAND`.

## Live monitoring over SWD (no OLED / no USB-TTL needed)

`host/read_ldr.py` looks up the tracker's RAM variables from the `.elf` (via
`arm-none-eabi-nm`) and reads them from the running target over SWD.

```powershell
python host\read_ldr.py COM9          # loop until Ctrl+C
python host\read_ldr.py COM9 20       # 20 samples
```

Columns: `tl tr bl br` (raw LDRs), `dA dE` (errors), `AZ EL` (angles),
`state` (DAY/NIGHT). It prints when a value changes plus a 2 s heartbeat.

Symbol addresses in the current build (auto-resolved at runtime):

| Symbol | Address |
|--------|---------|
| `elAngle` | 0x20000000 |
| `azAngle` | 0x20000004 |
| `lastDisplay` | 0x20000128 |
| `lastUpdate` | 0x2000012C |
| `br` | 0x20000130 |
| `bl` | 0x20000134 |
| `tr` | 0x20000138 |
| `tl` | 0x2000013C |
| `night` | 0x20000140 |
| `oledOk` | 0x20000141 |

## Confirming the sensors read correctly

Shade / expose one LDR at a time while `read_ldr.py` runs:

- shade **A0** → `tl` drops
- shade **A1** → `tr` drops
- shade **A4** → `bl` drops
- shade **A5** → `br` drops

A column that never moves means that LDR isn't being read (loose wire or a
missing 10 kΩ to GND). During testing, shading the bottom-left LDR moved `bl`
from ~384 to ~1553 within one update — proof the ADC path works end to end.

## Flashing

```powershell
python host\flash_stm32.py flash COM9 firmware\SolarTracker\build\SolarTracker.ino.bin
```

Build output (current `SolarTracker.ino`): 37,752 bytes flash (28%), 2,064
bytes RAM (10%). Argument order is `flash <PORT> <IMAGE>` — `flash <image> COM9`
would make `COM9` the image name and fail.
