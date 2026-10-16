# 01 — Overview & Architecture

## The idea

An ESP8266 can bit-bang SWD on its GPIOs. The ESP8266 has **no USB peripheral**:
its micro-USB port only powers it and drives a CH340 USB-to-UART bridge. So all
host↔probe traffic goes over that UART, and SWD is bit-banged on GPIOs. Result:
a $2 WiFi module becomes a flash-and-debug probe for an STM32.

```
   PC                         ESP8266 probe                  STM32F103 target
+--------+   USB CDC   +--------------------------+  SWD   +----------------+
| python |<----------->| CH340 -> UART @115200    |<------>| PA13 SWDIO     |
| host   |   (COMx)    | GPIO12=SWCLK GPIO13=SWDIO|  +GND  | PA14 SWCLK     |
+--------+             | GPIO14=NRST (optional)   |        | NRST (optional)|
                       +--------------------------+        +----------------+
```

The host sends single-byte commands; the probe replies `status,detail` + payload.
The protocol is identical to the upstream ESP32 probe.

## Data path

1. `host/flash_stm32.py` opens the ESP8266 COM port and sends a command byte
   (optionally with a binary body).
2. The ESP8266 (`firmware/esp8266_swd_probe/ESP8266_STM32_Probe.ino`) receives
   it and drives the SWD link: JTAG-to-SWD line reset, DP power-up, MEM-AP
   (AHB) access, Cortex-M debug registers, STM32F1 flash controller.
3. Data comes back the same way.

## Layers in the probe firmware

| Layer | What it does |
|-------|--------------|
| SWD transport | bit-banged DP reads/writes, ACK/parity handling, sticky-error clear, JTAG-to-SWD sequence |
| AP / MEM-AP | auto-probes APSEL for a MEM-AP, SELECT bank handling, TAR auto-increment block read/write (re-issues TAR at every 1 KB boundary) |
| Cortex-M core | halt/resume/step (step masks interrupts per ARMv7-M rules), register read/write via DCRSR/DCRDR, SYSRESETREQ, DFSR halt reason |
| Breakpoints/watchpoints | FPB v1/v2 comparators; DWT watchpoints **plus** a software single-step fallback for early Cortex-M3 r1p0 silicon whose DWT never fires |
| STM32F1 flash | device-density detection, 1 KB page erase, mass erase, halfword programming, read-back verify |
| Native protocol | `A X M H C S U T G N F V Y P D W J K I` (see `04-native-protocol.md`) |

## Notable implementation details

- **GPIO speed**: fast direct register access on the ESP8266
  (`GPIO_OUT_W1TS/W1TC`, `GPIO_IN`) instead of `digitalWrite`.
- **Clock timing**: `swd_delay_cycles = 50` (was 100) with the ESP8266 clocked
  at **160 MHz** — re-verified for reliability after the speed-up.
- **`yield()`** is injected into the long loops so the ESP8266's WiFi/network
  stack and watchdog don't starve.
- **AP auto-probe**: rather than assuming APSEL 0, the probe scans for the first
  MEM-AP and caches it; AP reads are posted and use the DP RDBUFF follow-up.
- **SW watchpoints**: on early F103 silicon the DWT comparators don't halt the
  core, so the probe records watchpoints and single-steps the CPU comparing
  memory (transparent on cores with a working DWT).
- **CKS32 / clones**: any DPIDR is accepted; `'I'` reports whatever it saw.

## Build/frequency facts

- ESP8266 FQBN: `esp8266:esp8266:nodemcuv2:xtal=160`
- STM32 FQBN: `STMicroelectronics:stm32:GenF1:pnum=BLUEPILL_F103CB`
- Host port used during development: **COM9**, 115200 baud.
- Measured throughput: ~3.2 KB/s read, ~280 B/s flash program.

## Project status

Done and verified:

- Probe firmware flashed; alive ping + banner OK.
- Full flash read (128 KB dump) byte-for-byte verified against the Blink image.
- SRAM write/read verified (non-destructive).
- Flash mass-erase + program + verify PASS.
- Original target firmware dumped and verified before flashing (the dump is
  kept locally, not published).
- Blink test image built, flashed, LED confirmed blinking.
- Solar tracker firmware built, flashed, running (60 ms loop, latest build
  37,752 B / 28 % flash).
- LDR sensing confirmed live over SWD; all four dividers read correctly.
- **Both servos confirmed moving** (external 5 V adapter + common ground).
- **Day tracking confirmed**: the panel follows the brightest quadrant and the
  elevation axis settles without oscillation after the control-loop fix
  (`LDR_EMA_SHIFT` 2→1, step 4°→2°).

Open:

- Confirm **direction polarity** (`AZ_INVERT` / `EL_INVERT`) with a lamp moved
  left→right and up→down.
- OLED I2C stalls the control loop (~3.7 s/pass) — auto-disabled by the firmware;
  revive it with 4.7 kΩ pull-ups. See `06-troubleshooting.md`.
