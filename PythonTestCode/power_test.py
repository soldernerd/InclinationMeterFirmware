#!/usr/bin/env python3
"""
Current-consumption bisection helper over the wired-UART API.

Sets the power-test bitmask (Commands / 0x03, svc_powertest.h): each bit
keeps one subsystem/rail/clock on, cleared bits cut it immediately. UART
and USB run off the always-on standby rail, so every combination stays
reachable.

  python power_test.py                 # interactive: guided bisection
  python power_test.py --mask 0x00     # everything off
  python power_test.py --mask 0xFF     # everything on (boot default)
  python power_test.py --mask 0x03     # only the two rails on
  python power_test.py --get           # just read the current mask

Bit map (set = ON):
  0 5V rail   1 3V3 rail   2 AD9833   3 ADS131M04
  4 BLE       5 display    6 LEDs     7 CPU-spin (clear = WFI idle)

Measure the supply current with a bench meter at each step.
Install:  pip install pyserial
"""

import argparse
import struct
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("Need pyserial:  pip install pyserial")

import apiv2 as a

BAUD = 115200
_KNOWN = {(0x0483, 0x374B), (0x0483, 0x374E), (0x0483, 0x374F), (0x0483, 0x3752),
          (0x0403, 0x6001), (0x0403, 0x6015), (0x10C4, 0xEA60)}


def find_port():
    ports = list(list_ports.comports())
    for p in ports:
        if p.vid is not None and (p.vid, p.pid) in _KNOWN:
            return p.device
    if len(ports) == 1:
        return ports[0].device
    sys.exit("Pass --port; candidates:\n  " +
             "\n  ".join(f"{p.device}  {p.description}" for p in ports))


class Link:
    def __init__(self, port):
        self.ser = serial.Serial(port, BAUD, timeout=0.3)
        self.re = a.Reassembler()

    def req(self, op, payload=b""):
        self.ser.reset_input_buffer()
        self.ser.write(a.build(op, payload))
        t = time.time()
        while time.time() - t < 2.0:
            for pop, st, d in self.re.feed(self.ser.read(128)):
                if pop == op:
                    return st, d
        return None, None

    def set_mask(self, mask):
        st, d = self.req(a.OP_CMD_POWER_TEST, struct.pack("<I", mask & 0xFFFFFFFF))
        applied = struct.unpack("<I", d[:4])[0] if (st == 0 and d and len(d) >= 4) else None
        return st, applied

    def get_mask(self):
        st, d = self.req(a.OP_RAW_PWRTEST)
        if st != 0 or not d or len(d) < 5:
            return None, None
        return struct.unpack("<I", d[:4])[0], d[4]


def fmt_mask(mask):
    on = [name for name, bit in a.PWR_BITS if mask & bit]
    off = [name for name, bit in a.PWR_BITS if not (mask & bit)]
    return f"0x{mask:02X}  on: {', '.join(on) or '(none)'}   off: {', '.join(off) or '(none)'}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--mask", help="hex or decimal mask to set, then exit")
    ap.add_argument("--get", action="store_true")
    args = ap.parse_args()

    port = args.port or find_port()
    print(f"Opening {port} @ {BAUD} ...")
    link = Link(port)

    if args.get:
        m, flags = link.get_mask()
        print(f"  current mask {fmt_mask(m)}" if m is not None else "  GET failed")
        print(f"  rails: 3V3={'on' if flags & 1 else 'off'}  5V={'on' if flags & 2 else 'off'}")
        return

    if args.mask is not None:
        m = int(args.mask, 0)
        st, applied = link.set_mask(m)
        print(f"  set -> [{a.STATUS.get(st, st)}]  {fmt_mask(applied) if applied is not None else ''}")
        return

    # Interactive guided bisection: everything off, then add one bit at a time.
    print("\nGuided bisection. Measure supply current at each prompt.\n")
    st, applied = link.set_mask(0)
    input(f"  ALL OFF ({fmt_mask(applied)})\n  measure, press Enter... ")
    running = 0
    for name, bit in a.PWR_BITS:
        running |= bit
        st, applied = link.set_mask(running)
        input(f"  + {name:18} -> {fmt_mask(applied)}\n  measure, press Enter... ")
    print("\n  restoring all-on.")
    link.set_mask(a.PWR_ALL)


if __name__ == "__main__":
    main()
