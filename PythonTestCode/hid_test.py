#!/usr/bin/env python3
"""
USB HID data-flow test for the InclinationMeter, API v3.

  python hid_test.py           # identity / state / health / temperature / settings / clock / environment round trip
  python hid_test.py --log     # subscribe to the device debug-log stream and print it live
  python hid_test.py --live    # subscribe to the live tilt reading

Packets longer than one 64-byte HID report are split into consecutive reports (docs/api-v3-spec.md section 2.2);
HidReassembler puts them back together.

Install:   pip install hidapi        ("import hid")
"""

import sys
import time

try:
    import hid
except ImportError:
    sys.exit("Need hidapi:  pip install hidapi")

import apiv3 as a
import device_checks as dc

VID, PID = 0x04D8, 0xF08F
REPORT_LEN = a.HID_REPORT


class UsbLink:
    def __init__(self):
        self.dev = hid.device()
        self.dev.open(VID, PID)
        self.dev.set_nonblocking(False)
        self.reasm = a.HidReassembler()
        self._pending = []

    def send(self, pkt: bytes):
        for report in a.hid_reports(pkt):
            self.dev.write(b"\x00" + report)          # leading report-id byte (the device uses none)

    def recv(self, timeout=1.5):
        deadline = time.time() + timeout
        while True:
            if self._pending:
                return self._pending.pop(0)
            left = deadline - time.time()
            if left <= 0:
                return (None, None, None)
            data = self.dev.read(REPORT_LEN, timeout_ms=int(left * 1000) + 1)
            if data:
                self._pending.extend(self.reasm.feed(bytes(data)))

    def close(self):
        self.dev.close()


def main():
    print(f"Opening {VID:#06x}:{PID:#06x} ...")
    try:
        link = UsbLink()
    except OSError as e:
        sys.exit(f"open failed ({e}). Unplugged, or another program has it open.")
    try:
        print(f"  manufacturer: {link.dev.get_manufacturer_string()!r}   product: {link.dev.get_product_string()!r}")
        if "--log" in sys.argv:
            dc.run_log(link)
        elif "--live" in sys.argv:
            dc.run_live(link)
        else:
            dc.run_basic(link)
    finally:
        link.close()


if __name__ == "__main__":
    main()
