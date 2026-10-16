# 04 — Native Serial Protocol

The host sends **one command byte** (optionally followed by a body). The probe
replies with a 2-byte header, then an optional payload:

```
reply header = <status:u8> <detail:u8>
  status 0  => OK     (detail is 0)
  status 1  => error  (detail = signed error code, as u8 two's complement)
```

All multi-byte integers are **little-endian**. Baud 115200, 8N1.

## Commands

| Cmd | Name | Body | Reply payload |
|-----|------|------|---------------|
| `A` | alive ping | — | — (header only) |
| `X` | block read | `addr:u32, nwords:u16` | `nwords × u32` (max 1024 words) |
| `M` | memory write | `addr:u32, len:u32, data[len]` | — (len ≤ 4096) |
| `H` | halt core | — | — |
| `C` | resume core | — | — (honours software watchpoints) |
| `S` | single step | — | — |
| `U` | system reset | — | — (SYSRESETREQ) |
| `T` | status | — | `DHCSR:u32` (`bit17 S_HALT` = core halted) |
| `G` | read core regs | — | `r0..r15:u32, xpsr:u32` (17 words, core must be halted) |
| `N` | write core reg | `sel:u8, val:u32` | — |
| `F` | FPB breakpoint | `op:u8, addr:u32` (op 1=set,0=clear,else clear-all) | `slot:u8` |
| `V` | DWT watchpoint | `op:u8, addr:u32, kind:u8` (kind 4=any,6=read,7=write) | `slot:u8` |
| `Y` | flash erase | `addr:u32, len:u32` (0,0 = mass erase) | — |
| `P` | flash program+verify | `addr:u32, len:u32, data[len]` | — (even addr/len, len ≤ 4096) |
| `D` | AP/DP reg read | `is_ap:u8, bank:u8, reg:u8` | `val:u32` |
| `W` | AP/DP reg write | `is_ap:u8, bank:u8, reg:u8, val:u32` | — |
| `J` | halt reason | — | `DFSR:u32` |
| `K` | part info | — | `cpuid:u32, page_size:u32, dual_bank:u8, apsel:u8` |
| `I` | chip info | — | `dpidr:u32, flash_size:u32, idcode:u32` |

`flash_size` is the raw `FLASH_SIZE` register value (`0x1FFFF7E0`); the low
16 bits are the size in KB. `idcode` is `DBGMCU_IDCODE` (`0xE0042000`).

## Error codes (in `detail`)

| Code | Meaning |
|------|---------|
| -2 | SWD parity error |
| -3 | SWD FAULT ack |
| -4 | WAIT retries exhausted (target stall) |
| -5 | DPIDR mismatch / target not responding |
| -6 | debug power-up timeout |
| -7 | core did not halt/step in time |
| -8 | no free FPB comparator |
| -9 | no free DWT/software watchpoint slot |
| -10 | flash PGERR / WRPRTERR |
| -11 | flash busy-wait timeout |
| -12 | probe out of memory |
| -13 | verify mismatch |
| -14 | bad command / bad parameters |
| -15 | FPB v1: address outside code region |
| -16 | no MEM-AP found |
| -99 | *(host-side only)* no reply from probe |

## Notes

- Most operations **re-run `swd_init()`** before executing, with a single retry,
  so the link self-heals after transient faults.
- `C` (resume) runs `cortex_resume_checked()`: if any software watchpoints are
  armed it single-steps and halts as soon as watched memory changes.
- Single-step sets `C_MASKINTS` in one write and `C_STEP` in a second write (as
  ARMv7-M requires) so pending interrupts can't derail the step.
- The `X` command caps at 1024 words (4 KB) per call; the host client loops for
  larger reads.

The reference implementation of every handler is `handle_native_cmd()` in
`firmware/esp8266_swd_probe/ESP8266_STM32_Probe.ino`.
