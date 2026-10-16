#!/usr/bin/env python3
"""Quick link self-test: open the probe's serial port, wait for its 'Ready.'
banner, send the 'A' alive ping, and confirm the two-byte 00 00 OK reply.
Works with or without an STM32 target attached (it only proves host<->ESP8266
comms). Requires pyserial."""
import sys, time
import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM9"

s = serial.Serial(PORT, 115200, timeout=0.5)
try:
    s.setDTR(False); s.setRTS(False)
except Exception:
    pass

# Wait for the firmware banner. The ESP8266 ROM prints garbage at 74880 baud
# first, then our sketch prints "Ready. Waiting for host commands..." at
# 115200 once setup() (incl. cortex_cleanup) completes.
print("waiting for probe banner...")
buf = b""
deadline = time.time() + 15
while time.time() < deadline and b"Ready." not in buf:
    c = s.read(256)
    if c:
        buf += c
banner_ok = b"Ready." in buf
print("banner seen:", banner_ok)
if banner_ok:
    tail = buf[buf.find(b"ESP8266"):] if b"ESP8266" in buf else buf[-80:]
    sys.stdout.write("  " + tail.decode("ascii", "replace").replace("\r", "").replace("\n", "\n  ").strip() + "\n")

time.sleep(0.3)
s.reset_input_buffer()

for attempt in range(1, 6):
    s.write(b"A")
    resp = s.read(2)
    if len(resp) == 2 and resp[0] == 0:
        print("alive OK: reply =", resp.hex(), "(status=%d detail=%d)" % (resp[0], resp[1]))
        sys.exit(0)
    print("attempt %d: bad reply %r, retrying..." % (attempt, resp))
    time.sleep(0.5)

print("FAILED: probe did not answer 'A' alive ping on", PORT)
sys.exit(1)
