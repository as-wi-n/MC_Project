#!/usr/bin/env python3
"""Non-destructive write-path test: halt the target, write a 256-byte pattern
into SRAM, read it back, compare, then reset+resume. Touches only volatile RAM
(lost on reset), never flash."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import flash_stm32 as f

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM9"
ADDR = 0x20000000
LEN = 256
pattern = bytes((i * 7 + 3) & 0xFF for i in range(LEN))

with f.Probe(PORT) as p:
    p.alive()
    print("halting target...")
    p.halt()
    print("writing %d bytes to 0x%08X..." % (LEN, ADDR))
    p.write_mem(ADDR, pattern)
    rb = p.read_mem(ADDR, LEN)
    if rb == pattern:
        print("RAM write/read OK: %d bytes match" % LEN)
        ok = True
    else:
        bad = [i for i in range(LEN) if pattern[i] != rb[i]]
        print("MISMATCH at %d offset(s), first: +0x%X wrote %02X read %02X"
              % (len(bad), bad[0], pattern[bad[0]], rb[bad[0]]))
        ok = False
    print("resetting + resuming target...")
    p.reset()
    import time; time.sleep(0.3)
    p.resume()
print("PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
