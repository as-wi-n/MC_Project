# 08 — Complete Progress Log

_Everything completed in this project so far, in order. Updated 2026-10-15._

---

## Phase 1 — ESP8266 serial SWD probe (2026-10-12)

Goal: flash and debug an STM32F103 with **no ST-Link and no OpenOCD**, using a
$2 ESP8266 as a bit-banged SWD probe.

- Ported the upstream ESP32 probe (`jithurx/mcproject`) to the **ESP8266**,
  stripped to **serial-only — WiFi unused**.
- Probe pin map: **SWCLK = GPIO12 (D6), SWDIO = GPIO13 (D7), NRST = GPIO14 (D5),
  LED = GPIO2 (D4)**.
- Built with `esp8266:esp8266:nodemcuv2:xtal=160` (160 MHz for faster
  bit-banging) and flashed over USB.
- **Link verified**: `A` (alive) ping OK, `Ready.` banner received, host↔probe
  protocol in sync.
- All native commands implemented: `A X M H C S U T G N F V Y P D W J K I`.
- Speed tuned: `swd_delay_cycles` **100 → 50** → ~3.2 KB/s read, ~280 B/s
  flash program. `yield()` injected into long loops so the ESP8266 watchdog/WiFi
  stack never starves.
- `tools/build.ps1` + `header.txt`/`footer.txt` assemble the probe sketch.

## Phase 2 — Host tooling (2026-10-12 → 10-13)

Python 3.14 + `pyserial`, talking to the probe on **COM9 @115200**.

| Tool | Purpose | Status |
|------|---------|--------|
| `host/flash_stm32.py` | client + flasher: `info` / `erase` / `flash` / `dump` | working |
| `host/selftest.py` | host↔probe link check | working |
| `host/ramtest.py` | non-destructive SRAM write/read test | **PASS** |
| `host/read_ldr.py` | live RAM/LDR monitor over SWD (symbols from `.elf`) | working |
| `host/read_ldr_test.py` | live monitor for the `LDR_Test` sketch | working |

- Confirmed `flash` argument order is **`flash <PORT> <IMAGE>`** (verified by
  test — the reverse makes `COM9` the image name and fails).
- Symbol addresses resolved automatically from
  `SolarTracker.ino.elf` via `arm-none-eabi-nm`.

## Phase 3 — Target identification & full flash control (2026-10-12)

- Target identity read over SWD: **DPIDR `0x2BA01477`**, **DBGMCU_IDCODE
  `0x20036410`** (device `0x410` = medium-density F103), **CPUID `0x412FC231`**
  (Cortex-M3 r2p1), **128 KB flash, 1024-byte pages, single bank, APSEL 0**.
- **Full 128 KB flash read** — byte-for-byte verified.
- **SRAM write/read verified** (non-destructive).
- **Mass erase + program + verify PASS.**
- Original firmware dumped and verified first (131072 B, kept locally — the
  board's stock image is not published in this repository).
- `BluePill_Blink` known-good image flashed, LED confirmed blinking — proves the
  whole PC → USB → CH340 → ESP8266 → SWD → STM32 chain end to end.

## Phase 4 — Tracker firmware (2026-10-12 → 10-15)

Support sketches written, compiled and flashed first:

| Sketch | Result |
|--------|--------|
| `LDR_Test.ino` | each LDR confirmed (shade A0→tl, A1→tr, A4→bl, A5→br) |
| `ServoTest.ino` | sweeps AZ (PA8) then EL (PA6) — both servos move |
| `ServoMultiPin.ino` | pin finder: 1-6 LED blinks = PA8/PA6/PA0/PA1/PB0/PB1 |

**Project firmware `firmware/SolarTracker/SolarTracker.ino`:**

- 4-quadrant LDR sensing: `azErr = (TL+BL)−(TR+BR)`, `elErr = (TL+TR)−(BL+BR)`.
- **Median of 9** samples per LDR (glitch rejection) + light EMA.
- **Day/night with hysteresis** (`NIGHT_ON 120` / `NIGHT_OFF 260`) → parks at
  `PARK_AZ 90°`, `PARK_EL 10°` at night.
- **Deadband (60 counts) + proportional step, capped 2°/60 ms** → jitter-free
  hold at balance, 33 °/s max slew.
- Angle limits AZ 0-180°, EL 10-90°; `AZ_INVERT`/`EL_INVERT` for mounting.
- OLED auto-detect: first refresh timed at boot; if I²C is slow the display is
  **disabled automatically** so the loop never stalls (`oledOk = 0` now).
- Built: **37,752 B flash (28 %) / 2,064 B RAM (10 %)**.
- Flashed through the probe: **VERIFY PASS (38,080 B)**, target reset + resumed.
- **Control loop measured at exactly 60 ms** (sampled `lastUpdate`, counted
  changes → `UPDATE_MS` honoured).
- **Servo PWM confirmed**: TIM2 @ 55.7 Hz (17.9 ms frame), PA8 ≈ 1.5 ms,
  PA6 ≈ 0.9 ms; pins in push-pull output mode (attach OK).

## Phase 5 — Hardware bring-up (2026-10-14 → 10-15)

**Power.** Established the rules the project follows now:

- **Never double-power** (ESP8266 3.3 V + STM32 USB at the same time).
- **Servos always from a separate 5 V adapter (4.8–6 V, ≥1 A)** with a
  **jumper from adapter − to STM32 GND** (common ground mandatory), plus a
  470–1000 µF bulk cap. Never from the Blue Pill's 5 V/3.3 V pin.
- **Over-current incident**: powering both servos from the STM32's 5 V pin
  tripped the laptop USB port → the Blue Pill's **5 V pin now reads 0 V**.
  Recovery: full power-cycle / different port. Harmless here — the MCU still
  runs (proven over SWD) and the tracker + servos don't use that pin.
  One single-servo bench test from the 5 V pin **did work first**, proving
  firmware, signal and 3.3 V logic level were always fine.

**Servos — RESOLVED (2026-10-14).** Separate adapter + common ground →
**both servos run**.

**LDRs — RESOLVED (2026-10-15).** Progression of live readings:
1. unconnected/floating → `207 / 532 / 345 / 976`
2. connected but divider wrong → `~0-5` → `maxLdr < NIGHT_ON` → **NIGHT mode**,
   servos parked (this looked like "servos don't track")
3. divider corrected (`3.3V — LDR — Ax — 10k — GND`) → **healthy `~1700-3800`**
   under room light → `night = 0`, **DAY tracking**.

## Phase 6 — Control-loop bug found & fixed (2026-10-15)

**Symptom:** elevation swept **24° ↔ 90° every ~4.4 s** (hunting) while the
light was stationary.

**Diagnosis:** reconstructed the raw error from the EMA deltas and showed the
light field was static (error slope ≈ 6.5 counts/°, balance ≈ EL 57°) — so it
was purely a control defect. `LDR_EMA_SHIFT=2` delayed the filtered error by
~0.7 s while `*_MAX_STEP_DEG=4` moved the panel at 66 °/s: the panel sailed past
the balance point, hit a stop, waited for the filter to catch up, then reversed
— a classic **filtered-feedback limit cycle**.

**Fix (two lines):**
```c
LDR_EMA_SHIFT       2 → 1    // error lag 0.7 s → ~0.35 s
AZ/EL_MAX_STEP_DEG  4 → 2    // 66 °/s → 33 °/s, overshoot 47° → ~9° (inside DEADBAND)
```

**Verified:** rebuilt (37,752 B), flashed **VERIFY PASS (38,080 B, 160.5 s)**,
monitored live over SWD for 20 s — **EL held rock-steady, zero oscillation**,
`night = 0`, both axes pointing at the bright quadrant.

## Phase 7 — Live diagnostics over SWD (2026-10-13 → 10-15)

Read the running firmware's own variables — no OLED, no serial capture needed:

- Watched `tl/tr/bl/br`, `azAngle`, `elAngle`, `night`, `oledOk`, `lastUpdate`
  in real time while shading LDRs and moving the lamp.
- Confirmed the tracker **responds to the light's direction, not its total
  intensity** (all-four-brighter = no movement; one side brighter = moves).
- Diagnosed the NIGHT state, the oscillation, and the LDR wiring fault purely
  from these samples.

## Phase 8 — Documentation (2026-10-12 → 10-15)

| File | Contents |
|------|----------|
| `README.md` | project pitch, hardware, quick start, layout, status |
| `docs/00-abstract.md` | full 14-section project abstract |
| `docs/01-overview.md` | architecture + status |
| `docs/02-wiring.md` | wiring, LDR divider, power warnings |
| `docs/03-build-and-flash.md` | toolchain, build + flash commands |
| `docs/04-native-protocol.md` | the serial command set |
| `docs/05-solar-tracker.md` | firmware behaviour, tunables, live monitor |
| `docs/06-troubleshooting.md` | error codes + resolved/open issues |
| `docs/07-project-wiring.md` | finished tracker hookup |
| `docs/08-progress-log.md` | this file |

Last full pass (2026-10-15): fixed the **wrong `flash <bin> COM9` argument
order** everywhere (correct = `flash <PORT> <IMAGE>`), refreshed all status
sections to reflect working servos + the oscillation fix, corrected stale
tunable values, and harmonised the capacitor guidance.

---

## Phase 9 — Publication cleanup (2026-10-15)

Repository prepared for official release:

- **Removed** (not part of the published deliverable): `code.c` (stray duplicate
  of `SolarTracker.ino`), `SESSION.md` (internal handoff notes),
  `backup/backup_0x08000000.bin` (the board's stock firmware dump),
  `reference/esp32_main_reference.cpp` (upstream third-party source — credited in
  the README instead), `firmware/solar_tracker/` (superseded earlier tracker),
  `host/__pycache__/`, all `*.map` linker maps, and the build folders of the
  throwaway test sketches.
- **Kept** prebuilt images only where the docs flash them:
  `firmware/SolarTracker/build/` and `firmware/test_sketch/build/`.
- Updated every doc that pointed at the removed items (`README`, `00-abstract`,
  `01-overview`, `03-build-and-flash`, `06-troubleshooting`).
- Repository shrunk from **6.7 MB → 0.44 MB**, 33 files, single clean commit.
- No license file was added, by decision — the repository is published under
  **all rights reserved**.

---

## Still open

1. **Direction polarity** — move a lamp left→right and up→down; the panel must
   turn *toward* it (AZ 0→180, EL rising). Flip `AZ_INVERT`/`EL_INVERT` to `-1`
   if an axis turns away.
2. **Mount the mechanics** and balance the LDR cross-divider (TR reads lower
   than its neighbours → a static `dA` bias of a few hundred counts).
3. **Optional OLED** — 4.7 kΩ pull-ups on SDA/SCL→3.3V, shorter wires; the
   firmware re-enables it automatically when I²C is healthy.
4. **Field tune** `DEADBAND`, gain and night thresholds under real sunlight.
