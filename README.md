# mc_project — ESP8266 SWD Probe + Dual-Axis Solar Tracker

> **Full history of everything built → `docs/08-progress-log.md`** (chronological,
> phase by phase, with every verification result).

Two things built together in this project:

1. **ESP8266 SWD debug probe** (serial-only, no WiFi). It erases / programs /
   verifies and debugs an **STM32F103 "Blue Pill"** over SWD — **no ST-Link and
   no OpenOCD required**. The host talks to it with a small Python client over
   the ESP8266's USB serial port.
2. **Dual-Axis Solar Tracker** firmware for the STM32F103 (4 LDRs + 2 servos +
   SSD1306 OLED), developed and flashed entirely through that probe.

The SWD transport is ported from the ESP32 probe in
`github.com/jithurx/mcproject` (credited here; the upstream source itself is not
included in this repository).

---

## Hardware

| Role   | Board                                            |
|--------|--------------------------------------------------|
| Probe  | ESP8266 NodeMCU v2 / D1 mini (CH340 USB)         |
| Target | STM32F103C8/CB "Blue Pill"                        |
| Sensors| 4 × LDR + 10 kΩ dividers                         |
| Actuators | 2 × hobby servo (azimuth + elevation)         |
| Display| SSD1306 128×64 OLED (I2C, addr 0x3C)             |

Confirmed target identity (read over SWD): DPIDR `0x2BA01477`,
DBGMCU_IDCODE `0x20036410` (device `0x410` = medium-density F103),
CPUID `0x412FC231` (Cortex-M3 r2p1), 128 KB flash, 1024-byte pages,
single bank, MEM-AP APSEL 0.

---

## Quick start

```powershell
# 0. one-time: python -m pip install pyserial

# 1. Flash the probe firmware to the ESP8266 (see docs/03-build-and-flash.md)
#    firmware\esp8266_swd_probe\ESP8266_STM32_Probe.ino

# 2. Wire probe <-> target  (see docs/02-wiring.md)

# 3. Check the host<->probe link
python host\selftest.py COM9

# 4. Flash the tracker image   (order: flash <PORT> <IMAGE>)
python host\flash_stm32.py flash COM9 firmware\SolarTracker\build\SolarTracker.ino.bin

# 5. Watch the four LDRs live while you shade them (Ctrl+C to stop)
python host\read_ldr.py COM9
```

---

## Layout

```
mc_project/
├── README.md                     you are here
├── docs/
│   ├── 00-abstract.md            FULL project abstract (start here)
│   ├── 01-overview.md            architecture + status
│   ├── 02-wiring.md              all wiring, LDR divider, power warnings
│   ├── 03-build-and-flash.md     toolchain paths, build + flash commands
│   ├── 04-native-protocol.md     the serial command set
│   ├── 05-solar-tracker.md       firmware behaviour, tunables, live monitor
│   ├── 06-troubleshooting.md     error codes + known issues
│   ├── 07-project-wiring.md      finished tracker hookup
│   └── 08-progress-log.md        EVERYTHING done so far (chronological)
├── firmware/
│   ├── esp8266_swd_probe/        ESP8266_STM32_Probe.ino   (the probe)
│   ├── SolarTracker/             SolarTracker.ino + build/  (PROJECT firmware)
│   ├── LDR_Test/                 LDR_Test.ino               (LDR test)
│   ├── ServoTest/                ServoTest.ino              (servo sweep test)
│   ├── ServoMultiPin/            ServoMultiPin.ino          (find the servo pin)
│   └── test_sketch/              BluePill_Blink.ino + build/ (known-good test image)
├── host/                         Python tools run on the PC
│   ├── flash_stm32.py            client + flasher  (info/erase/flash/dump)
│   ├── read_ldr.py               live LDR/RAM monitor over SWD
│   ├── read_ldr_test.py          live monitor for the LDR_Test sketch
│   ├── selftest.py               host<->probe link check
│   └── ramtest.py                non-destructive SRAM write/read test
└── tools/                        probe firmware assembly (build.ps1 + header/footer)
```

Only the two images the docs tell you to flash ship prebuilt:
`firmware/SolarTracker/build/` (the tracker) and
`firmware/test_sketch/build/` (known-good blink). The other sketches are
sources — rebuild them with `arduino-cli` (see `docs/03-build-and-flash.md`).

---

## Status

- Probe firmware built, flashed, link verified, SRAM + flash read/write verified.
- Target's stock firmware dumped and verified before flashing; Blink test image
  flashed and confirmed blinking.
- **Project firmware** `SolarTracker.ino` compiled, flashed and **running**
  (control loop measured at exactly 60 ms; latest build 37,752 B / 28 % flash,
  2,064 B RAM, `VERIFY PASS`).
- LDR sensing confirmed live over SWD; all four LDRs verified with a multimeter,
  readings now in a healthy range (~1.7k–3.8k counts under room light).
- **Both servos confirmed working** — powered from a separate 5 V adapter with a
  common ground to the STM32 (never from the Blue Pill's 5 V pin).
- **Elevation hunting bug found and fixed**: a heavy EMA (`LDR_EMA_SHIFT=2`) plus
  a 4°/tick step made the panel overshoot into a limit cycle (EL oscillating
  24°↔90°). Now `LDR_EMA_SHIFT=1` + 2°/tick → the panel settles and holds.
  Verified live: EL steady, no oscillation.
- Tracker is in **DAY tracking** mode and follows the brightest quadrant;
  azimuth/elevation sit at their limits when the light is off to one side.
- **Open — verify direction polarity** (`AZ_INVERT`/`EL_INVERT`): move a lamp
  left→right and up→down and confirm the panel turns *toward* it; flip to −1 if
  an axis turns the wrong way.
- The OLED is **auto-disabled** (slow I²C — add 4.7 kΩ pull-ups to revive it).

---

## Licence

No licence file is included: **all rights reserved**. You are free to read and
study it; contact the author for any other use.
