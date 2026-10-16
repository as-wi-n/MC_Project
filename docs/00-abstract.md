# 00 — Project Abstract

## Dual-Axis Solar Tracking System with an ESP8266-based SWD Debug Probe

**Project folder:** `mc_project`
**Target MCU:** STM32F103C8/CB "Blue Pill" (128 KB flash, Cortex-M3)
**Development/debug probe:** ESP8266 NodeMCU (serial-only, bit-banged SWD)
**Framework:** Arduino (STM32duino + ESP8266 core) via `arduino-cli`

---

## 1. Abstract (summary)

This project combines two things built together:

1. **An ESP8266-based SWD (Serial Wire Debug) probe.** A cheap WiFi module
   (ESP8266) is turned into a full **flash-and-debug probe** for an STM32F103 —
   **no ST-Link and no OpenOCD**. The ESP8266 bit-bangs SWD on its GPIOs while a
   Python client on the PC drives it over the ESP8266's USB-serial port. The host
   can erase, program, verify, read, and debug the STM32, plus read its RAM
   live.

2. **A Dual-Axis Solar Tracker** running on that STM32F103. Four LDRs
   (light-dependent resistors) sense the brightest direction; the firmware
   computes a horizontal (azimuth) and vertical (elevation) error and drives two
   hobby servos to point a panel/solar cell at the light. It filters the sensor
   readings, uses a dead-banded proportional control loop, enforces mechanical
   travel limits, and parks the panel at night. An optional SSD1306 OLED shows
   live values.

The entire tracker firmware was developed, compiled, and flashed **through that
same ESP8266 probe** — proving the probe works end-to-end.

The SWD transport is ported from the ESP32 probe in
`github.com/jithurx/mcproject` (credited here; the upstream source itself is not
included in this repository).

---

## 2. Objectives

| # | Objective | Status |
|---|-----------|--------|
| 1 | Flash/debug an STM32 without an ST-Link, using an ESP8266 | **Done & verified** |
| 2 | Read the target's RAM live over SWD | **Done & verified** |
| 3 | Sense light with 4 quadrant LDRs | **Done & verified** |
| 4 | Drive 2 servos to track the brightest direction | **Done & verified** — both servos move; direction polarity pending (see §10) |
| 5 | Show live state on an OLED (optional) | **Skipped** — auto-disabled due to slow I2C (see §9) |
| 6 | Day/night detection + night parking | **Done** |

---

## 3. System Architecture

```
   PC (Windows)                          ESP8266 Probe                 STM32F103 Target
+-----------------+   USB CDC / COM9   +---------------------+  SWD   +---------------------+
| Python host     |<------------------>| CH340 -> UART 115200|<------>| PA14 SWCLK          |
| flash_stm32.py  |   115200 baud      | GPIO12 = SWCLK      |  +GND  | PA13 SWDIO          |
| read_ldr.py     |                    | GPIO13 = SWDIO      |        | NRST (optional)     |
| selftest.py ... |                    | GPIO14 = NRST       |        |                     |
+-----------------+                    | GPIO2  = LED        |        |  <-- solar tracker  |
                                       +---------------------+        |      firmware       |
                                                                      +---------------------+
```

**Key idea:** The ESP8266 has **no USB peripheral** — its micro-USB port only
powers it and drives a **CH340** USB-to-UART bridge. So *all* host↔probe traffic
goes over that UART, and SWD is bit-banged on GPIOs. A $2 WiFi module becomes a
flash-and-debug probe.

### Data path
1. `host/flash_stm32.py` opens the ESP8266 COM port and sends a single-byte
   command (with an optional binary body).
2. `firmware/esp8266_swd_probe/ESP8266_STM32_Probe.ino` receives it and drives
   the SWD link: JTAG-to-SWD line reset, DP power-up, MEM-AP (AHB) access,
   Cortex-M debug registers, and the STM32F1 flash controller.
3. Data comes back over the same UART.

---

## 4. Hardware Components

| Role | Component | Notes |
|------|-----------|-------|
| Debug probe | ESP8266 NodeMCU v2 / D1 mini | CH340 USB-serial, 160 MHz |
| Target MCU | STM32F103C8/CB "Blue Pill" | 128 KB flash, 20 KB RAM, 72 MHz |
| Sensors | 4 × LDR + 10 kΩ dividers | Voltage dividers into ADC pins |
| Actuators | 2 × hobby servo | Azimuth (horizontal) + Elevation (vertical) |
| Display | SSD1306 128×64 OLED | I2C, address 0x3C (optional) |
| Power (target) | STM32's own USB, or ESP 3.3 V (one source only) | Never double-power |
| Power (servos) | Separate 5 V adapter | Common ground required |

### Confirmed target identity (read over SWD)
- DPIDR `0x2BA01477`
- DBGMCU_IDCODE `0x20036410` (device `0x410` = medium-density F103)
- CPUID `0x412FC231` (Cortex-M3 r2p1)
- 128 KB flash, 1024-byte pages, single bank, MEM-AP APSEL 0

---

## 5. Pin Map (STM32F103 Blue Pill)

Board silk = port letter + number (`A0` = PA0, `C13` = PC13).

| Function | MCU pin | Board label |
|----------|---------|-------------|
| LDR top-left | PA0 | A0 |
| LDR top-right | PA1 | A1 |
| LDR bottom-left | PA4 | A4 |
| LDR bottom-right | PA5 | A5 |
| Servo **azimuth** signal | PA8 | A8 |
| Servo **elevation** signal | PA6 | A6 |
| OLED SCL | PB6 | B6 |
| OLED SDA | PB7 | B7 |
| Debug Serial TX / RX | PA9 / PA10 | A9 / A10 (USART1) |
| On-board status LED | PC13 | C13 (active low) |

- Avoid PC14/PC15 (32.768 kHz crystal pins).
- `Serial` = USART1 (PA9/PA10); `Wire` = I2C1 (SDA=PB7, SCL=PB6).
- LDRs on PA0/PA1/PA4/PA5 (not PA2/PA3, which are USART2 by default).

---

## 6. How It Works — Solar Tracker (working principle)

### 6.1 Light sensing
Each LDR forms a **voltage divider** with a 10 kΩ resistor:
```
3.3V ──[ LDR ]──┬──► ADC pin
                │
             [ 10k ]
                │
               GND
```
Brighter light → **higher** reading on that channel.

The four quadrants are read, then **filtered**:
- **Median-of-9** per channel — rejects single-sample ADC glitches.
- **EMA** (exponential moving average, shift 2) — smooths the value.

### 6.2 Error computation
```
azimuth  error (dA) = (TL + BL) - (TR + BR)   // + = light to the left
elevation error (dE) = (TL + TR) - (BL + BR)   // + = light to the top
```

### 6.3 Control law
For each axis (azimuth and elevation):
- If `|error| > DEADBAND` (60 counts), move a step **toward the brighter side**.
- Step size is **proportional** to the error: `|error| / GAIN_DIV`, at least 1°,
  capped at **4°** per update. So it moves fast when far from the light and fine
  as it approaches balance.
- Result: smooth, **jitter-free** convergence with no hunting.
- `AZ_INVERT` / `EL_INVERT` (±1) flip an axis if it turns the wrong way
  mechanically.

### 6.4 Limits & night mode
- Azimuth limited to **0–180°**; elevation to **10–90°**.
- Power-on: azimuth 90°, elevation 50° (centred).
- **Night detection with hysteresis:** if the brightest LDR drops below 120 →
  night; it wakes above 260. In night mode the panel **parks** (azimuth 90°,
  elevation 10°).

### 6.5 Main loop
Runs every **`UPDATE_MS = 60`** ms (measured loop period confirmed = exactly
60 ms). Order: sense → filter → errors → night check → target angles →
constrain → actuate (only writes servos when the angle changed) → update OLED/LED
→ periodic serial report.

### 6.6 OLED handling
The firmware **auto-detects a slow OLED** at boot: if one `display.display()`
refresh exceeds 300 ms, it disables the display so the control loop stays fast
and precise. (This is why the OLED is currently skipped — see §9.)

---

## 7. How It Works — ESP8266 SWD Probe

The probe firmware is layered:

| Layer | What it does |
|-------|--------------|
| SWD transport | Bit-banged DP read/write, ACK/parity handling, sticky-error clear, JTAG-to-SWD sequence |
| AP / MEM-AP | Auto-probes APSEL for a MEM-AP, SELECT bank handling, TAR auto-increment block read/write |
| Cortex-M core | Halt/resume/step, register read/write via DCRSR/DCRDR, SYSRESETREQ, DFSR halt reason |
| Breakpoints/watchpoints | FPB v1/v2 comparators; DWT watchpoints + software single-step fallback |
| STM32F1 flash | Density detection, 1 KB page erase, mass erase, halfword programming, read-back verify |
| Native protocol | Command set below |

### Native serial protocol
Single-byte commands; probe replies `status,detail` + payload:

```
A X M H C S U T G N F V Y P D W J K I
```

- `A` = alive ping, `X` = read memory, `M` = write memory, `H` = halt,
  `U` = resume, `T` = read DHCSR, `G` = read CPU registers, `F` = flash command,
  `Y`/`P` = dump/read, `I` = identity (dpidr, fsize, idcode), etc.

### Performance
- `swd_delay_cycles = 50` at 160 MHz (tuned from 100 for speed).
- Measured throughput: **~3.2 KB/s read**, **~280 B/s flash program**.
- `yield()` is injected into long loops so the ESP8266 WiFi/stack/watchdog
  doesn't starve.

---

## 8. Contents of the `mc_project` Folder

### Firmware (`firmware/`)

| Sketch | Purpose | Size / status |
|--------|---------|---------------|
| `esp8266_swd_probe/ESP8266_STM32_Probe.ino` | The ESP8266 SWD probe firmware (flashed to the ESP8266) | Flashed, working |
| `SolarTracker/SolarTracker.ino` | **The project firmware** — dual-axis tracker | 37,752 B flash (28%), 2,064 B RAM; flashed & verified, loop 60 ms |
| `LDR_Test/LDR_Test.ino` | Standalone LDR test (median+EMA, prints TL/TR/BL/BR) | 15,432 B; flashed & verified |
| `ServoTest/ServoTest.ino` | Servo sweep test (AZ 0–180, EL 10–90, LED blink) | 12,248 B; flashed & verified |
| `ServoMultiPin/ServoMultiPin.ino` | Sweeps one pin at a time (PA8,PA6,PA0,PA1,PB0,PB1) with LED count to find the servo pin | 15,508 B |
| `test_sketch/BluePill_Blink.ino` | Known-good blink test image | 9,332 B; flashed & confirmed |

Prebuilt images (`.bin` for flashing, `.elf` for symbols, `.hex`) ship only for
the two sketches the docs tell you to flash: `SolarTracker/build/` and
`test_sketch/build/`. Rebuild any sketch with `arduino-cli`
(`docs/03-build-and-flash.md`).

### Host tools (`host/`) — Python, run on the PC

| Script | Purpose |
|--------|---------|
| `flash_stm32.py` | Main client + flasher (`info / erase / flash / dump`) |
| `read_ldr.py` | Live LDR/RAM monitor over SWD (change-only + heartbeat) |
| `read_ldr_test.py` | Live monitor for the LDR_Test sketch (`val/vmin/vmax`) |
| `selftest.py` | Checks the host↔probe link (alive ping) |
| `ramtest.py` | Non-destructive SRAM write/read test |

### Docs (`docs/`)
`00-abstract.md` (this file), `01-overview.md`, `02-wiring.md`,
`03-build-and-flash.md`, `04-native-protocol.md`, `05-solar-tracker.md`,
`06-troubleshooting.md`, `07-project-wiring.md`, `08-progress-log.md`.

### Other
- `README.md` — project overview and quick start.
- `tools/` — probe firmware assembly (`build.ps1`, `header.txt`, `footer.txt`).

---

## 9. Build & Flash Process

**Toolchain**
- `arduino-cli` v1.5.1 at
  `C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe`
- ESP8266 FQBN: `esp8266:esp8266:nodemcuv2:xtal=160`
- STM32 FQBN: `STMicroelectronics:stm32:GenF1:pnum=BLUEPILL_F103CB`
- Host port used: **COM9** (CH340), 115200 baud.
- Python 3.14 + `pyserial`.

**Compile (STM32):**
```powershell
& "<arduino-cli>" compile `
  --fqbn STMicroelectronics:stm32:GenF1:pnum=BLUEPILL_F103CB `
  --output-dir "firmware\SolarTracker\build" `
  "firmware\SolarTracker"
```

**Flash (via probe):**
```powershell
python host\flash_stm32.py flash firmware\SolarTracker\build\SolarTracker.ino.bin
```
(Flash verifies by reading back every page — "VERIFY PASS". Note: the port
defaults to COM9; if passing the port explicitly, the argument order is
`flash <port> <image>`.)

**Check the link / monitor:**
```powershell
python host\selftest.py COM9
python host\read_ldr.py COM9
```

---

## 10. Current Status & Verification

### Verified working
- ✅ ESP8266 probe firmware flashed; alive-ping + banner OK.
- ✅ Full 128 KB flash read verified byte-for-byte against the Blink image.
- ✅ SRAM write/read verified (non-destructive).
- ✅ Flash mass-erase + program + **verify PASS**.
- ✅ Original target firmware read out and verified before flashing (a full
  128 KB dump was taken during bring-up; the board's original image is not
  published in this repository).
- ✅ Blink test image flashed, LED confirmed blinking.
- ✅ **SolarTracker firmware built, flashed, running** — control loop measured at
  **exactly 60 ms** (fast, no stall).
- ✅ **LDR sensing confirmed live** over SWD — values read correctly and change
  when each LDR is shaded; all four LDRs independently verified with a multimeter.
- ✅ **Servo output signals confirmed:** `TIM2` runs at 55.7 Hz (17.9 ms frame);
  PA8 emits a ~1.5 ms pulse, PA6 a ~0.9 ms pulse — valid servo PWM.
- ✅ Servo pins configured as push-pull outputs (Servo library attach OK).
- ✅ **Both servos confirmed rotating** (resolved — see "Servo hardware issue
  (resolved)" under open issues below).
- ✅ **LDR divider wiring corrected** — readings now ~1.7k–3.8k counts under room
  light and respond to a lamp; firmware leaves NIGHT mode (`night=0`, DAY).
- ✅ **Control-loop oscillation found and fixed** — a heavy EMA plus a 4°/tick
  step caused a limit cycle (EL sweeping 24°↔90° every ~4.4 s). Root cause:
  the filtered error lagged the true position error by ~0.7 s, so the panel
  always overshot the balance point. Fix: `LDR_EMA_SHIFT` 2→1 and
  `AZ/EL_MAX_STEP_DEG` 4→2 (overshoot 47° → 9°, inside the deadband).
  Re-flashed (`VERIFY PASS`) and confirmed steady — no more hunting.
- ✅ **Day tracking confirmed live over SWD** — readings, errors, angles and
  `night` state all sampled while tracking; servos respond to the light's
  *direction* (not its total intensity).

### Known open issues
1. **OLED skipped (auto-disabled).** The SSD1306 I2C transfer stalls the loop
   (~3.7 s/pass) due to missing pull-ups / long wires / possibly an SH1106
   module. The firmware auto-detects this and disables the display so tracking
   stays fast — it still works, just without the screen. **Fix:** add 4.7 kΩ
   pull-ups on SDA/SCL→3.3V and shorten wires. Details in `06-troubleshooting.md`.
2. **Direction polarity not yet confirmed (`AZ_INVERT` / `EL_INVERT`).** Move a
   lamp left→right and up→down: azimuth must sweep 0°→180° and elevation must
   rise toward the light, with the panel **facing** the lamp at the end. If an
   axis turns away from the light, set that `*_INVERT` to `-1` and re-flash.

   **Servo hardware issue (RESOLVED).** The servos initially appeared dead even
   though the PWM was correct. Two things fixed it: power the servos from a
   **separate 5 V (4.8–6 V, ≥1 A) adapter**, and **jumper the adapter's − to the
   STM32 GND** (common ground — without it the pulses float). An earlier attempt
   to power both servos from the Blue Pill's 5 V pin tripped the PC USB
   over-current protection: the STM32's **5 V pin now reads 0 V** even unloaded
   (the MCU itself still runs fine — irrelevant now, since the servos use the
   external adapter and the Blue Pill runs from its own USB). If a single supply
   must feed servos, check `docs/06-troubleshooting.md` for the fuse/over-current
   recovery notes.

---

## 11. Servo Power Wiring (summary)

Servo wire colors (standard 3-wire hobby servo):

| Wire | Meaning |
|------|---------|
| Red | +5 V (VCC) |
| Brown or Black | GND |
| Orange (or yellow/white) | Signal (PWM) |

```
   Separate servo adapter +5V
        +5V ──┬──► AZ servo RED
              ├──► EL servo RED
              └──► cap (+)      (470–1000 µF)
        GND ──┬──► AZ servo BROWN
              ├──► EL servo BROWN
              ├──► STM32 GND      ← COMMON GROUND (required)
              └──► cap (–)
   AZ servo ORANGE ──► STM32 A8  (PA8)
   EL servo ORANGE ──► STM32 A6  (PA6)
```

- Servo **red → adapter +5 V only** (never the STM32 3.3 V or board 5 V pin).
- Servo **brown → adapter –**, and that **– must also tie to STM32 GND**.
- Adapter must be **4.8–6 V**; add the capacitor across +5 V/GND.
- For a quick no-load bench test, **one** small servo can be powered from the
  STM32's own 5 V pin (but not two, and no load — it may brown out).

---

## 12. LDR Divider & Tracker Wiring (summary)

```
3.3V ──[ LDR ]──┬──► A0 / A1 / A4 / A5
                │
             [ 10k ]
                │
               GND
```
Mount the four LDRs in quadrants behind a **cross-shaped divider** so each sees
a distinct wedge of the sky — otherwise ambient imbalance pins the tracker at its
travel limits (see §10).

Full hookup: `docs/07-project-wiring.md`.

---

## 13. Tunables (top of `SolarTracker.ino`)

| Constant | Value | Meaning |
|----------|-------|---------|
| `UPDATE_MS` | 60 | Control loop period (ms) |
| `LDR_SAMPLES` | 9 | Median of 9 ADC reads (glitch rejection) |
| `LDR_EMA_SHIFT` | 1 | EMA smoothing 1/2 — keep light, or the error lags and the panel hunts |
| `DEADBAND` | 60 | Ignore smaller errors (prevents hunting) |
| `AZ_GAIN_DIV` / `EL_GAIN_DIV` | 40 | Step = `|err|/DIV` (proportional) |
| `AZ_MAX_STEP_DEG` / `EL_MAX_STEP_DEG` | 2 | Max degrees per update (= 33 °/s) |
| `AZ_MIN/AZ_MAX` | 0/180 | Azimuth limits |
| `EL_MIN/EL_MAX` | 10/90 | Elevation limits |
| `AZ_INVERT` / `EL_INVERT` | 1 | Flip to −1 if an axis turns backwards |
| `NIGHT_ON` / `NIGHT_OFF` | 120/260 | Night thresholds (hysteresis) |
| `PARK_AZ` / `PARK_EL` | 90/10 | Night parking position |
| `OLED_UPDATE_MS` / `OLED_SLOW_MS` | 250/300 | Display refresh / slow-OLED disable |
| `ENABLE_OLED` | 1 | 0 compiles the display out entirely |

---

## 14. How to Resume / Next Steps

**Resume the work:**
1. Reconnect the probe (SWCLK→PA14, SWDIO→PA13, GND→GND; leave probe 3.3 V off
   if the STM32 has its own USB). `python host\selftest.py COM9`.
2. Build the tracker: compile `firmware\SolarTracker`.
3. Flash: `python host\flash_stm32.py flash COM9 firmware\SolarTracker\build\SolarTracker.ino.bin`
   (argument order is `<PORT>` then `<IMAGE>`).
4. Monitor: `python host\read_ldr.py COM9`.

**Open next steps:**
- **Confirm direction polarity** (`AZ_INVERT` / `EL_INVERT`, §10): move a lamp
  left→right and up→down and make sure the panel turns *toward* it.
- Mount the LDR cross-divider so the four channels balance under even light
  (TR currently reads lower than its neighbours — a static `dA` bias of a few
  hundred counts).
- Optionally revive the OLED (4.7 kΩ pull-ups) — it auto-enables when the I2C is
  healthy.
- Tune `DEADBAND`, gain, and night thresholds once the mechanics are mounted.

**Full protocol reference:** `docs/04-native-protocol.md`. **Everything completed
so far:** `docs/08-progress-log.md`.
