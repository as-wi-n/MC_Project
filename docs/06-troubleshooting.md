# 06 — Troubleshooting & Known Issues

## Resolved: servos appeared dead / elevation hunted

Both are fixed — kept here so they aren't rediscovered.

**Servos didn't move.** PWM on PA8/PA6 was proven correct, so the fault was
powering. The servos now run from a **separate 5 V adapter (4.8–6 V, ≥1 A)** with
a **jumper from the adapter − to the STM32 GND**. Without that common ground the
servo receives valid pulses but has no reference, so it sits still.

**PC USB over-current (the Blue Pill's 5 V pin now reads 0 V).** Powering *both*
servos from the Blue Pill's 5 V pin drew too much from the laptop port and
tripped its over-current protection. Recovery, in order:

1. Unplug **everything** from the PC USB port (STM32 + ESP8266) for ~30 s.
2. Plug the Blue Pill into a **different** USB port (or a powered hub/wall
   charger) — many ports need a full power-cycle to close the latch again.
3. Verify with a multimeter: **5 V pin ≈ 5 V** with nothing connected to it.

If the 5 V pin still reads 0 V, the onboard polyfuse (`F1`) or diode (`D1`) may
have opened. That is **harmless for this project**: the tracker runs from the
Blue Pill's 3.3 V rail and the servos use their own adapter — just never power
loads from the 5 V pin again.

**Panel oscillated instead of tracking.** The elevation servo swept 24°↔90°
every ~4.4 s. Cause: `LDR_EMA_SHIFT=2` delayed the error ~0.7 s while the step
rate was 4°/60 ms, so the panel always sailed past the balance point and only
reversed after hitting a stop. Fixed by `LDR_EMA_SHIFT` 2→1 and
`AZ/EL_MAX_STEP_DEG` 4→2 (overshoot ~47° → ~9°, absorbed by `DEADBAND`).
Verified: angles hold steady.

**All four LDRs read ~0 (NIGHT mode, servos parked).** The divider wasn't
reaching the ADC — check that each LDR's top leg is on **3.3 V** and the 10 kΩ
goes to **GND**, i.e. `3.3V — LDR — Ax — 10k — GND`. Healthy readings are in the
hundreds to thousands under normal light.

---

## Open issue: OLED stalls the main loop

**Symptom:** the tracker updates only every ~3.7 s (`lastUpdate` in
`read_ldr.py` jumps by ~3745 ms). The ADC re-samples and the servos step at
that slow rate instead of every `UPDATE_MS`.

**Diagnosis so far:** the STEMMA/SSD1306 `display.display()` I²C transfer is
blocking far too long. With `oledOk = 1` the panel *is* detected at 0x3C, so
the bus is being driven but each transaction appears to run to an STM32 I²C
timeout (≈33 chunks × ~113 ms ≈ 3.7 s for the 1 KB framebuffer). The core is
*not* halted (`DHCSR bit17 = 0`) — it's just spending nearly all its time in
the display transfer.

**Likely cause:** weak / missing pull-ups on SDA/SCL, or long/loose I²C wires,
or a partly-responding module.

**Try, in order:**

1. Add **4.7 kΩ pull-ups** from SDA→3.3V and SCL→3.3V (if the module doesn't
   already have them). Shorten the I²C wires.
2. Confirm the module is a real SSD1306 (not an SH1106, which some 1.3" 128×64
   boards use and which won't render correctly with the SSD1306 driver).
3. If the OLED isn't essential yet, comment out the `updateDisplay()` call (or
   the whole OLED block) to get a fast control loop; the tracker runs fine
   without a display.
4. Afterwards, consider updating the display less often and/or moving it off
   the critical path once the bus is healthy.

The loop should complete in well under `UPDATE_MS` (60 ms) once I²C is fixed.
(The firmware already times the first refresh at boot and **auto-disables** the
display if it's slow — `oledOk = 0` — so this is cosmetic, not a blocker.)

---

## Error codes from `flash_stm32.py`

The client prints a friendly hint for the common ones:

| Code | Meaning | What to do |
|------|---------|------------|
| -4 | WAIT retries exhausted | target stalled — press the STM32 RESET, retry |
| -99 | no reply from probe | probe desynced — close any Serial Monitor, reopen; make sure nothing else holds COM9 |
| -5 | DPIDR mismatch | check SWD wiring (SWDIO/SWCLK) and GND |
| -3 | SWD FAULT ack | transient — the client retries; check wiring if persistent |
| -6 | debug power-up timeout | target not powered / not seated |
| -13 | verify mismatch | re-flash; if it persists, lower `swd_delay_cycles` speed (raise the value) |
| -16 | no MEM-AP found | target not responding to AP access |

## Common gotchas

- **Only one process may open COM9 at a time.** Close the Arduino Serial
  Monitor before running any Python tool.
- **Argument order.** `flash` takes **`<PORT>` then `<IMAGE>`**:
  `flash COM9 firmware\...\x.bin`. The reverse (`flash x.bin COM9`) makes `COM9`
  the image name and fails. `dump` is `<out> [size] [port]`; `info`/`erase` are
  just `<port>`.
- **COM port number changes.** `COM9` is the CH340 (ESP8266). If it differs,
  pass your port explicitly. Avoid the Bluetooth COM ports.
- **Probe desync right after boot.** The ESP8266 ROM prints garbage at 74880
  baud, then the sketch banner at 115200. The host client waits for the
  `Ready.` marker, so let it.
- **Powering the target:** never feed the STM32 from both the ESP8266 3.3V and
  its own USB at once. Don't drive servos while flashing.
- **Link flakiness over long jumpers:** raise `swd_delay_cycles` in
  `ESP8266_STM32_Probe.ino` (e.g. back to 100). Slower, but the target is
  clocked by the probe so a slower clock is always safe.

## Recovering a "bricked" target

If the tracker firmware misbehaves, you can always:

1. `python host\selftest.py COM9` — confirm the probe replies.
2. `python host\flash_stm32.py erase COM9` — mass erase.
3. `python host\flash_stm32.py flash COM9 firmware\test_sketch\build\BluePill_Blink.ino.bin`
   — flash the known-good blink image to confirm the target is alive.
4. `python host\flash_stm32.py flash COM9 my_backup.bin`
   — restore a dump you took earlier (see `03-build-and-flash.md`; the board's
   original image is not shipped with this repository).
