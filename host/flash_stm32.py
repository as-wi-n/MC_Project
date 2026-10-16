#!/usr/bin/env python3
"""
Standalone host client + flasher for the ESP8266 STM32 SWD probe.

The ESP8266 runs the serial-only SWD probe firmware (ESP8266_STM32_Probe.ino)
and is connected to this PC over USB. This script talks the probe's native
binary protocol over that serial port and can erase/program/verify an
STM32F1 (Blue Pill / F103) target over SWD -- no ST-Link required.

This file is self-contained (no dependency on the mcproject repo layout).

Usage:
    python flash_stm32.py info                 [PORT]
    python flash_stm32.py flash firmware.bin   [PORT]
    python flash_stm32.py erase                [PORT]
    python flash_stm32.py dump  out.bin [SIZE] [PORT]

PORT defaults to COM9 (Windows) -- pass e.g. /dev/ttyUSB0 on Linux.

Requires: pyserial  (pip install pyserial)
"""

import argparse
import struct
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("ERROR: pyserial is required.  Install with:  pip install pyserial")

DEFAULT_PORT = "COM9"
DEFAULT_ADDRESS = 0x08000000
MAX_P_LEN = 4096
READ_LEN = 4096

ERROR_NAMES = {
    -2: "parity error", -3: "SWD FAULT ack", -4: "WAIT retries exhausted",
    -5: "DPIDR mismatch (target not responding)", -6: "debug power-up timeout",
    -7: "core did not halt/step in time", -8: "no free FPB comparator",
    -9: "no free DWT/software watchpoint slot", -10: "flash PGERR/WRPRTERR",
    -11: "flash busy-wait timeout", -12: "probe out of memory",
    -13: "verify mismatch", -14: "bad command / bad parameters",
    -15: "FPB V1: address outside code region", -16: "no MEM-AP found",
    -99: "no reply from probe (not connected / still booting?)",
}


class ProbeError(RuntimeError):
    def __init__(self, code):
        self.code = code
        super().__init__("probe error %d (%s)" % (code, ERROR_NAMES.get(code, "unknown")))


class Probe:
    def __init__(self, port, baud=115200, boot_timeout=10.0):
        self.ser = serial.Serial(port, baud, timeout=5)
        # Do not hold the ESP8266 in reset / bootloader: release DTR+RTS so
        # the CH340/CP2102 auto-reset circuit lets the sketch run normally.
        try:
            self.ser.setDTR(False)
            self.ser.setRTS(False)
        except Exception:
            pass
        self._wait_boot(boot_timeout)

    def close(self):
        self.ser.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def _wait_boot(self, timeout):
        """Wait for the probe's 'Ready.' banner so the first reply header
        isn't desynced against boot/banner bytes. The ROM prints garbage at
        74880 baud first, then the sketch banner at 115200 once setup()-
        including the best-effort cortex_cleanup() - has finished."""
        s = self.ser
        s.timeout = 0.5
        buf = b""
        deadline = time.time() + timeout
        while time.time() < deadline:
            chunk = s.read(256)
            if chunk:
                buf += chunk
                if b"Ready." in buf or b"Waiting for host" in buf:
                    break
        # Drain anything still queued after the banner, then start clean.
        time.sleep(0.2)
        if s.in_waiting:
            s.read(s.in_waiting)
        s.reset_input_buffer()

    def _recvall(self, n, timeout=5):
        self.ser.timeout = timeout
        data = b""
        while len(data) < n:
            chunk = self.ser.read(n - len(data))
            if not chunk:
                break
            data += chunk
        return data

    def _cmd(self, op, body=b"", reply_len=0, timeout=5):
        self.ser.write(bytes([op]) + body)
        hdr = self._recvall(2, timeout=timeout)
        if len(hdr) != 2:
            raise ProbeError(-99)
        status, detail = hdr[0], hdr[1]
        if status != 0:
            code = detail - 256 if detail >= 128 else detail
            raise ProbeError(code)
        return self._recvall(reply_len, timeout=timeout) if reply_len else b""

    # ---- protocol (mirrors handle_native_cmd in the firmware) ----
    def alive(self):
        self._cmd(ord("A"))

    def halt(self):
        self._cmd(ord("H"))

    def resume(self):
        self._cmd(ord("C"))

    def reset(self):
        self._cmd(ord("U"))

    def read_block(self, addr, nwords):
        if nwords > 1024:
            raise ValueError("nwords exceeds probe's 1024-word cap")
        return self._cmd(ord("X"), struct.pack("<IH", addr, nwords),
                         reply_len=nwords * 4, timeout=10)

    def read_mem(self, addr, length):
        out = bytearray()
        a, remaining = addr, length
        while remaining:
            n = min(remaining, READ_LEN)
            nwords = (n + 3) // 4
            out += self.read_block(a, nwords)[:n]
            a += n
            remaining -= n
        return bytes(out)

    def write_mem(self, addr, data, timeout=30):
        off = 0
        while off < len(data):
            chunk = data[off:off + MAX_P_LEN]
            self._cmd(ord("M"), struct.pack("<II", addr + off, len(chunk)) + chunk,
                      timeout=timeout)
            off += len(chunk)

    def read_word(self, addr):
        return struct.unpack("<I", self.read_block(addr, 1))[0]

    def write_word(self, addr, val):
        self.write_mem(addr, struct.pack("<I", val))

    def core_regs(self):
        """Return r0..r15 + xpsr (17 words). Target must be halted."""
        data = self._cmd(ord("G"), reply_len=17 * 4, timeout=10)
        return struct.unpack("<17I", data)

    def flash_erase(self, addr, length, timeout=60):
        self._cmd(ord("Y"), struct.pack("<II", addr, length), timeout=timeout)

    def flash_program(self, addr, data, timeout=240):
        if len(data) > MAX_P_LEN or len(data) % 2 or addr % 2:
            raise ValueError("flash_program: addr/len must be even, len <= 4096")
        self._cmd(ord("P"), struct.pack("<II", addr, len(data)) + data, timeout=timeout)

    def chip_info(self):
        return struct.unpack("<III", self._cmd(ord("I"), reply_len=12))

    def part_info(self):
        pl = self._cmd(ord("K"), reply_len=10)
        cpuid, page = struct.unpack("<II", pl[:8])
        return cpuid, page, bool(pl[8]), pl[9]


def load_image(path, address):
    if path.lower().endswith(".hex"):
        try:
            import intelhex
        except ImportError:
            sys.exit("ERROR: .hex needs 'pip install intelhex', or pass a .bin")
        ih = intelhex.IntelHex()
        ih.loadhex(path)
        base = ih.minaddr()
        return base, bytes(ih.tobinarray(start=base, end=ih.maxaddr()))
    with open(path, "rb") as f:
        return address, f.read()


def with_retry(desc, retries, fn):
    last = None
    for attempt in range(1, retries + 1):
        try:
            return fn()
        except ProbeError as e:
            last = e
            print("  %s: attempt %d/%d failed: %s" % (desc, attempt, retries, e))
            if attempt < retries:
                time.sleep(1.0)
    raise last


def cmd_info(args):
    with Probe(args.port, baud=args.baud) as p:
        p.alive()
        dpidr, fsize_reg, idcode = p.chip_info()
        cpuid, page, dual, ap = p.part_info()
        print("Probe alive on %s @%d" % (args.port, args.baud))
        print("DPIDR          : 0x%08X" % dpidr)
        print("DBGMCU_IDCODE  : 0x%08X (device 0x%03X)" % (idcode, idcode & 0xFFF))
        print("CPUID          : 0x%08X" % cpuid)
        print("Flash size     : %d KB" % (fsize_reg & 0xFFFF))
        print("Page size      : %d bytes" % page)
        print("Dual bank      : %s" % dual)
        print("MEM-AP (APSEL) : %d" % ap)
    return 0


def cmd_erase(args):
    with Probe(args.port, baud=args.baud) as p:
        p.alive()
        print("Mass-erasing...")
        t0 = time.time()
        with_retry("mass erase", args.retries, lambda: p.flash_erase(0, 0, timeout=120))
        print("Erased in %.1fs" % (time.time() - t0))
    return 0


def cmd_flash(args):
    addr, data = load_image(args.image, args.address)
    orig = len(data)
    if len(data) % 2:
        data += b"\xff"
    print("Image: %s  %d bytes (%d padded) at 0x%08X" % (args.image, orig, len(data), addr))

    t_all = time.time()
    with Probe(args.port, baud=args.baud) as p:
        p.alive()
        cpuid, page, dual, ap = p.part_info()
        print("Target: CPUID=0x%08X page=%d dual=%s AP=%d" % (cpuid, page, dual, ap))
        if addr % page:
            sys.exit("ERROR: address 0x%08X not page-aligned (page %d)" % (addr, page))

        chunk_len = args.chunk_kb * 1024
        n_chunks = (len(data) + chunk_len - 1) // chunk_len
        print("Plan: %d x %d KB chunks" % (n_chunks, args.chunk_kb))

        if args.mass_erase_first:
            print("Mass-erasing...")
            with_retry("mass erase", args.retries, lambda: p.flash_erase(0, 0, timeout=120))

        if not args.full and not args.mass_erase_first:
            print("Pre-reading %d bytes (skip-identical planning)..." % len(data))
            t0 = time.time()
            existing = with_retry("pre-read", args.retries,
                                  lambda: p.read_mem(addr, len(data)))
            print("Pre-read in %.1fs" % (time.time() - t0))
        else:
            existing = None

        # page plan
        pages = []
        for off in range(0, len(data), page):
            want = data[off:off + page]
            have = existing[off:off + page] if existing is not None else None
            if have is not None and have == want:
                pages.append(("skip", off))
            elif want.strip(b"\xff") == b"":
                pages.append(("erase", off))
            else:
                pages.append(("program", off))
        n_skip = sum(1 for k, _ in pages if k == "skip")
        n_erase = sum(1 for k, _ in pages if k == "erase")
        n_prog = sum(1 for k, _ in pages if k == "program")
        print("Pages: %d skip, %d erase-only, %d program" % (n_skip, n_erase, n_prog))

        for kind, off in pages:
            if kind != "erase":
                continue
            a = addr + off
            with_retry("erase 0x%08X" % a, args.retries,
                       lambda a=a: p.flash_erase(a, page))

        runs, cur = [], []
        for kind, off in pages:
            if kind == "program":
                cur.append(off)
                if len(cur) * page >= chunk_len:
                    runs.append(cur); cur = []
            elif cur:
                runs.append(cur); cur = []
        if cur:
            runs.append(cur)

        total = sum(len(r) for r in runs) * page
        done = 0
        for run in runs:
            for i in range(0, len(run), max(1, chunk_len // page)):
                grp = run[i:i + max(1, chunk_len // page)]
                off0, off1 = grp[0], grp[-1] + page
                a, blob = addr + off0, data[off0:off1]
                t0 = time.time()
                with_retry("program 0x%08X+%d" % (a, len(blob)), args.retries,
                           lambda a=a, blob=blob: p.flash_program(a, blob, args.timeout))
                done += len(blob)
                print("  wrote 0x%08X +%d (%d/%d B, %.1f KB/s, %.1fs)"
                      % (a, len(blob), done, total, done / 1024.0 / max(time.time() - t0, 1e-3),
                         time.time() - t0))

        if not args.no_verify:
            print("Full read-back verify...")
            rb = with_retry("verify", args.retries, lambda: p.read_mem(addr, len(data)))
            if rb != data:
                i = next(j for j in range(len(data)) if data[j] != rb[j])
                print("VERIFY FAILED at 0x%08X: wrote 0x%02X read 0x%02X"
                      % (addr + i, data[i], rb[i]), file=sys.stderr)
                return 1
            print("VERIFY PASS (%d bytes)" % len(data))

        if not args.no_reset:
            p.reset(); time.sleep(0.3); p.resume()
            print("Target reset + resumed.")
    print("Done in %.1fs." % (time.time() - t_all))
    return 0


def cmd_dump(args):
    size = args.size if args.size else 0x10000
    with Probe(args.port, baud=args.baud) as p:
        p.alive()
        print("Dumping %d bytes from 0x%08X..." % (size, args.address))
        t0 = time.time()
        data = p.read_mem(args.address, size)
        with open(args.out, "wb") as f:
            f.write(data)
        print("Wrote %d bytes to %s in %.1fs" % (len(data), args.out, time.time() - t0))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    def common(sp):
        sp.add_argument("port", nargs="?", default=DEFAULT_PORT)
        sp.add_argument("--baud", type=int, default=115200)
        sp.add_argument("--retries", type=int, default=4)

    p_info = sub.add_parser("info"); common(p_info); p_info.set_defaults(func=cmd_info)
    p_erase = sub.add_parser("erase"); common(p_erase); p_erase.set_defaults(func=cmd_erase)

    p_flash = sub.add_parser("flash"); common(p_flash)
    p_flash.add_argument("image")
    p_flash.add_argument("--address", type=lambda x: int(x, 0), default=DEFAULT_ADDRESS)
    p_flash.add_argument("--chunk-kb", type=int, default=4, choices=(1, 2, 4))
    p_flash.add_argument("--timeout", type=int, default=240)
    p_flash.add_argument("--full", action="store_true")
    p_flash.add_argument("--mass-erase-first", action="store_true")
    p_flash.add_argument("--no-verify", action="store_true")
    p_flash.add_argument("--no-reset", action="store_true")
    p_flash.set_defaults(func=cmd_flash)

    p_dump = sub.add_parser("dump"); common(p_dump)
    p_dump.add_argument("out")
    p_dump.add_argument("size", nargs="?", type=lambda x: int(x, 0), default=0x10000)
    p_dump.add_argument("--address", type=lambda x: int(x, 0), default=DEFAULT_ADDRESS)
    p_dump.set_defaults(func=cmd_dump)

    args = ap.parse_args()
    try:
        sys.exit(args.func(args))
    except ProbeError as e:
        print("Probe error: %s" % e, file=sys.stderr)
        print("Hints: -4 WAIT = target stall (press STM32 RESET, retry); "
              "-99 = probe desync (close any serial monitor, reopen); "
              "-5 DPIDR = check SWD wiring/GND.", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
