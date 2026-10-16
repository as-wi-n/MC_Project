#!/usr/bin/env python3
"""
Live monitor for the standalone LDR test sketch (firmware/LDR_Test/LDR_Test.ino),
read over SWD through the ESP8266 probe -- no USB-TTL needed.

Looks up val[4]/vmin[4]/vmax[4] from the compiled .elf (via nm), reads them from
the running STM32 and prints the four LDR values plus the direction errors.
Cover each LDR and watch only its column move.

Usage:
    python read_ldr_test.py [COM9] [iterations]
    (iterations omitted or 0 = loop until Ctrl+C)

Requires pyserial.  The ESP8266 probe must be wired to the STM32 (SWDIO/SWCLK/GND).
"""

import glob
import os
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import flash_stm32 as f

ELF = r"C:\Users\LOQ\OneDrive\Desktop\mc_project\firmware\LDR_Test\build\LDR_Test.ino.elf"

WANT = ["val", "vmin", "vmax"]


def find_nm():
    pats = [
        r"C:\Users\LOQ\AppData\Local\Arduino15\packages\STMicroelectronics\tools\xpack-arm-none-eabi-gcc\*\bin\arm-none-eabi-nm.exe",
        r"C:\Program Files*\Arm GNU Toolchain*\*\bin\arm-none-eabi-nm.exe",
    ]
    for p in pats:
        hits = glob.glob(p)
        if hits:
            return sorted(hits)[-1]
    return "arm-none-eabi-nm"


def symbol_addresses(elf):
    nm = find_nm()
    out = subprocess.check_output([nm, elf], text=True)
    want = set(WANT)
    syms = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] in want:
            syms[parts[2]] = int(parts[0], 16)
    missing = want - set(syms)
    if missing:
        raise SystemExit("Symbols not found in ELF: %s" % ", ".join(sorted(missing)))
    return syms


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM9"
    try:
        iters = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    except ValueError:
        iters = 0

    syms = symbol_addresses(ELF)
    print("reading LDR test RAM over %s (val=%s)" % (port, hex(syms["val"])))
    print("cover/shade one LDR at a time and watch its column move\n")

    p = f.Probe(port)
    p.alive()
    print("%8s  %4s %4s %4s %4s   %6s %6s   %s"
          % ("time", "tl", "tr", "bl", "br", "dAz", "dEl", "min..max"))
    print("-" * 78)
    t0 = time.time()
    last = None
    last_print = 0.0
    try:
        n = 0
        while iters == 0 or n < iters:
            val = struct.unpack("<4H", p.read_mem(syms["val"], 8))
            vmin = struct.unpack("<4H", p.read_mem(syms["vmin"], 8))
            vmax = struct.unpack("<4H", p.read_mem(syms["vmax"], 8))
            tl, tr, bl, br = val
            dAz = (tl + bl) - (tr + br)
            dEl = (tl + tr) - (bl + br)
            row = (val, vmin, vmax)
            now = time.time()
            if row != last or now - last_print > 2.0:
                print("%7.1fs  %4d %4d %4d %4d   %+6d %+6d   TL%d-%d TR%d-%d BL%d-%d BR%d-%d"
                      % (now - t0, tl, tr, bl, br, dAz, dEl,
                         vmin[0], vmax[0], vmin[1], vmax[1],
                         vmin[2], vmax[2], vmin[3], vmax[3]))
                last = row
                last_print = now
            n += 1
            time.sleep(0.4)
    except KeyboardInterrupt:
        pass
    finally:
        p.close()


if __name__ == "__main__":
    main()
