# 03 — Build & Flash

Environment: Windows + PowerShell.

## Toolchain

| Tool | Path / value |
|------|--------------|
| arduino-cli | `C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe` (v1.5.1) |
| ESP8266 core | `esp8266:esp8266` 3.1.2 |
| STM32 core | `STMicroelectronics:stm32` 3.0.0 |
| Libraries | Adafruit SSD1306 2.5.17, Adafruit GFX 1.12.6, Adafruit BusIO 1.17.4 (Servo is bundled with the STM32 core) |
| arm-none-eabi-nm | `…\Arduino15\packages\STMicroelectronics\tools\xpack-arm-none-eabi-gcc\14.2.1-1.1\bin\arm-none-eabi-nm.exe` |
| Python | 3.14.3 with `pyserial` 3.5 (`python -m pip install pyserial`) |

FQBNs:

- ESP8266 probe: `esp8266:esp8266:nodemcuv2:xtal=160`  (160 MHz for SWD speed)
- STM32 target:   `STMicroelectronics:stm32:GenF1:pnum=BLUEPILL_F103CB`

---

## Build the ESP8266 probe firmware

`firmware/esp8266_swd_probe/ESP8266_STM32_Probe.ino` is generated from the
upstream reference by `tools/build.ps1`, which:

1. Takes the portable core (SWD transport → native protocol) from the upstream
   ESP32 probe at `github.com/jithurx/mcproject`,
2. Injects the required `yield()` calls (6 anchors) so the ESP8266 watchdog /
   WiFi stack don't starve during long loops,
3. Prepends `tools/header.txt` (includes, pin map, fast GPIO, timing) and
   appends `tools/footer.txt` (`setup()` / `loop()` and the serial entry point).

> The `.ino` checked into `firmware/esp8266_swd_probe/` is the **source of truth**
> (it carries the final `swd_delay_cycles = 50`). `tools/header.txt` still shows
> the original 100 — edit the `.ino` if you need to change the delay.

Compile & upload to the ESP8266:

```powershell
& "C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe" `
  compile --fqbn esp8266:esp8266:nodemcuv2:xtal=160 `
  "C:\Users\LOQ\OneDrive\Desktop\mc_project\firmware\esp8266_swd_probe"

& "C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe" `
  upload -p COM9 --fqbn esp8266:esp8266:nodemcuv2:xtal=160 `
  "C:\Users\LOQ\OneDrive\Desktop\mc_project\firmware\esp8266_swd_probe"
```

After flashing, the probe prints at 115200:

```
ESP8266 ST-Link probe (serial-only) starting...
SWCLK=GPIO12  SWDIO=GPIO13  NRST=GPIO14
Ready. Waiting for host commands...
```

---

## Build the STM32 solar tracker

```powershell
& "C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe" `
  compile --fqbn STMicroelectronics:stm32:GenF1:pnum=BLUEPILL_F103CB `
  --output-dir "C:\Users\LOQ\OneDrive\Desktop\mc_project\firmware\SolarTracker\build" `
  "C:\Users\LOQ\OneDrive\Desktop\mc_project\firmware\SolarTracker"
```

Outputs land in `firmware/SolarTracker/build/`:
`.bin` (flash image), `.elf` (symbols — used by `read_ldr.py`), `.hex`, `.map`.

Prebuilt images are already committed there:
- `SolarTracker/build/SolarTracker.ino.bin` — 37,752 bytes (~28% of flash),
  2,064 bytes RAM. **This is the project firmware.**
- `test_sketch/build/BluePill_Blink.ino.bin` — 9,332 bytes, known-good test.

The other sketches (`LDR_Test`, `ServoTest`, `ServoMultiPin`) ship as source
only — the same compile line, pointing at that folder, rebuilds them.

---

## Flash the target through the probe

The probe must be wired to the target (SWD + GND). The one-line client is
`host/flash_stm32.py`:

```powershell
# Identify the chip
python host\flash_stm32.py info COM9

# Erase + program + verify   (order: flash <PORT> <IMAGE>)
python host\flash_stm32.py flash COM9 firmware\SolarTracker\build\SolarTracker.ino.bin

# Mass erase
python host\flash_stm32.py erase COM9

# Back up / dump flash  (dump <out> [size] [port]) — writes your own file
python host\flash_stm32.py dump my_backup.bin 0x20000 COM9
```

`flash` options: `--address 0x08000000` (default), `--chunk-kb {1,2,4}`,
`--full` (skip the read-back skip-identical optimisation),
`--mass-erase-first`, `--no-verify`, `--no-reset`.

The flasher pre-reads the region, skips pages that are already correct, erases
only pages that need it, programs in 4 KB chunks and then does a full read-back
verify. On success it resets and resumes the target.

### Backing up / restoring the target's firmware

Take a dump **before** you overwrite a board you care about, then restore from
your own file if needed:

```powershell
# 1. save whatever is currently on the chip
python host\flash_stm32.py dump my_backup.bin 0x20000 COM9

# 2. ... later, put it back
python host\flash_stm32.py flash COM9 my_backup.bin
```

## Verify the link / hardware without flashing

```powershell
python host\selftest.py COM9     # host<->probe alive ping
python host\ramtest.py COM9      # non-destructive SRAM write/read (halts, then resets)
```

> Only one program may have the COM port open at a time. Close any Arduino
> Serial Monitor before running the Python tools.
