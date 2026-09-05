#!/usr/bin/env python3
"""
USB HID data-flow test for the InclinationMeter, API v2.

  python hid_test.py           # identity / device-state / temp / settings round-trip
  python hid_test.py --log     # subscribe to the device debug-log stream and print it live

Install:   pip install hidapi        ("import hid")
"""

import struct
import sys
import time

try:
    import hid
except ImportError:
    sys.exit("Need hidapi:  pip install hidapi")

import apiv2 as a

VID, PID = 0x04D8, 0xF08F
REPORT_LEN = 64


class UsbLink:
    def __init__(self):
        self.dev = hid.device()
        self.dev.open(VID, PID)
        self.dev.set_nonblocking(False)

    def send(self, pkt: bytes):
        self.dev.write(b"\x00" + pkt)          # leading report-id byte (device uses none)

    def recv(self, timeout_ms=1500):
        data = self.dev.read(REPORT_LEN, timeout_ms=timeout_ms)
        return a.parse(bytes(data)) if data else (None, None, None)

    def close(self):
        self.dev.close()


def request(link, op, payload=b"", timeout_ms=1500):
    link.send(a.build(op, payload))
    r_op, status, data = link.recv(timeout_ms)
    return status, data


def run_basic(link):
    print(f"  manufacturer: {link.dev.get_manufacturer_string()!r}   "
          f"product: {link.dev.get_product_string()!r}")

    st, data = request(link, a.OP_SYS_IDENTITY)
    print(f"  IDENTITY      [{a.STATUS.get(st, st)}] {a.decode_identity(data) if st == 0 else data.hex()}")

    st, data = request(link, a.OP_SYS_DEVICE_STATE)
    print(f"  DEVICE_STATE  [{a.STATUS.get(st, st)}] {a.decode_device_state(data) if st == 0 else data.hex()}")

    st, data = request(link, a.opcode(a.GET, a.CAT_MEAS, a.MEAS_ONBOARD_TEMP))
    if st == 0 and len(data) >= 2:
        print(f"  TEMP          [OK] {struct.unpack('<h', data[:2])[0] / 100:+.2f} C")
    else:
        print(f"  TEMP          [{a.STATUS.get(st, st)}] {data.hex()}")

    # Settings round-trip: read stream_interval_ms, bump it, read back.
    getop = a.opcode(a.GET, a.CAT_SETTINGS, a.SET_STREAM_INTERVAL_MS)
    setop = a.opcode(a.SET, a.CAT_SETTINGS, a.SET_STREAM_INTERVAL_MS)
    st, data = request(link, getop)
    if st == 0 and len(data) >= 2:
        cur = struct.unpack("<H", data[:2])[0]
        new = 250 if cur != 250 else 200
        st2, _ = request(link, setop, struct.pack("<H", new))
        st3, data3 = request(link, getop)
        back = struct.unpack("<H", data3[:2])[0] if (st3 == 0 and len(data3) >= 2) else None
        print(f"  SETTINGS      stream_interval_ms {cur} -> set {new} "
              f"[{a.STATUS.get(st2, st2)}] -> read back {back}")
    else:
        print(f"  SETTINGS      GET stream_interval_ms [{a.STATUS.get(st, st)}]")


def run_log(link, min_sev=0):
    print(f"Subscribing to debug log (min severity {a.SEVERITY[min_sev]}). Ctrl+C to stop.\n")
    link.send(a.build(a.opcode(a.SUBSCRIBE, a.CAT_DEBUG, a.DBG_LOG_STREAM), bytes([min_sev])))
    sub_op = a.opcode(a.SUBSCRIBE, a.CAT_DEBUG, a.DBG_LOG_STREAM)
    try:
        while True:
            op, st, data = link.recv(timeout_ms=2000)
            if op is None:
                continue
            if op != sub_op or st != 0 or len(data) < 3:
                continue
            issue, page, sev = data[0], data[1], data[2]
            msg = data[3:].decode("ascii", "replace")
            print(f"  [{a.SEVERITY.get(sev, sev):5}] #{issue:<3} {msg}")
    except KeyboardInterrupt:
        link.send(a.build(a.opcode(a.UNSUBSCRIBE, a.CAT_DEBUG, a.DBG_LOG_STREAM)))
        time.sleep(0.1)
        print("\nunsubscribed.")


def main():
    print(f"Opening {VID:#06x}:{PID:#06x} ...")
    try:
        link = UsbLink()
    except OSError as e:
        sys.exit(f"open failed ({e}). Unplugged, or another program has it open.")
    try:
        if "--log" in sys.argv:
            run_log(link)
        else:
            run_basic(link)
    finally:
        link.close()


if __name__ == "__main__":
    main()
