#!/usr/bin/env python3
"""
Live LDR monitor for the Dual-Axis Solar Tracker, read over SWD through the
ESP8266 probe -- no OLED or USB-TTL needed.

It looks up the tracker's RAM variables from the compiled .elf (via nm), then
reads them from the running STM32 and prints the four LDR values plus the
computed errors and servo angles. Cover/uncover each LDR and watch the
matching value change.

Usage:
    python read_ldr.py [COM9] [iterations]
    (iterations omitted or 0 = loop until Ctrl+C)

Requires pyserial.  The ESP8266 probe must be wired to the STM32 (SWDIO/SWCLK/
GND) and a target GND shared.
"""

import glob
import os
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import flash_stm32 as f

ELF = r"C:\Users\LOQ\OneDrive\Desktop\mc_project\firmware\SolarTracker\build\SolarTracker.ino.elf"

WANT = ["tl", "tr", "bl", "br", "night", "azAngle", "elAngle", "oledOk"]


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
    base = min(syms.values())
    end = max(syms.values()) + 4
    length = ((end - base) + 3) & ~3
    print("reading %d bytes @ 0x%08X over %s" % (length, base, port))
    print("cover/uncover each LDR and watch its value move\n")

    p = f.Probe(port)
    p.alive()
    print("%8s  %4s %4s %4s %4s   %6s %6s   %3s %3s   %s"
          % ("time", "tl", "tr", "bl", "br", "dA", "dE", "AZ", "EL", "state"))
    print("-" * 80)
    t0 = time.time()
    last = None
    last_print = 0.0
    try:
        n = 0
        while iters == 0 or n < iters:
            blk = p.read_mem(base, length)
            def val(name):
                off = syms[name] - base
                return struct.unpack_from("<i", blk, off)[0]
            tl, tr, bl, br = val("tl"), val("tr"), val("bl"), val("br")
            az, el = val("azAngle"), val("elAngle")
            night = blk[syms["night"] - base] & 1
            dA = (tl + bl) - (tr + br)
            dE = (tl + tr) - (bl + br)
            row = (tl, tr, bl, br, dA, dE, az, el, night)
            now = time.time()
            if row != last or now - last_print > 2.0:
                print("%7.1fs  %4d %4d %4d %4d   %+6d %+6d   %3d %3d   %s"
                      % (now - t0, tl, tr, bl, br, dA, dE, az, el,
                         "NIGHT" if night else "DAY"))
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
