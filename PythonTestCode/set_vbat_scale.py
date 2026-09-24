#!/usr/bin/env python3
"""
Read/set the battery-divider calibration (Settings 0x0F/0x10/0x1C) over
the wired-UART API transport. SET persists to EEPROM immediately (the
firmware's dispatch_settings() saves on every SET) -- flashing new
Config/config.h DEFAULT_VBAT_SCALE_* values only seeds a *blank* EEPROM
page, it does not overwrite an already-provisioned board, hence this.

  python set_vbat_scale.py --port COM6                # just read back current values
  python set_vbat_scale.py --port COM6 --num 133 --den 100 --offset 0
  python set_vbat_scale.py --port COM6 --offset 79     # tweak just one field

See docs/bench_instruments.md / Config/config.h for the current divider.
"""

import argparse
import struct
import sys
import time

import serial
from serial.tools import list_ports

import apiv2 as a

BAUD = 115200
_KNOWN_VID_PID = {
    (0x0483, 0x374B), (0x0483, 0x374E), (0x0483, 0x374F), (0x0483, 0x3752),
    (0x0403, 0x6001), (0x0403, 0x6015), (0x10C4, 0xEA60),
}


def find_port():
    for p in list_ports.comports():
        if p.vid is not None and (p.vid, p.pid) in _KNOWN_VID_PID:
            return p.device
    ports = list(list_ports.comports())
    if len(ports) == 1:
        return ports[0].device
    sys.exit("Multiple/no serial ports; pass --port:\n  " +
              "\n  ".join(f"{p.device}  {p.description}" for p in ports))


class Link:
    def __init__(self, port):
        self.ser = serial.Serial(port, BAUD, timeout=0.2)
        self.reasm = a.Reassembler()

    def request(self, op, payload=b"", timeout=2.0):
        self.ser.reset_input_buffer()
        self.ser.write(a.build(op, payload))
        deadline = time.time() + timeout
        while time.time() < deadline:
            for pop, status, data in self.reasm.feed(self.ser.read(128)):
                if pop == op:
                    return status, data
        return None, None

    def close(self):
        self.ser.close()


def get_u16(link, res):
    st, d = link.request(a.opcode(a.GET, a.CAT_SETTINGS, res))
    return struct.unpack("<H", d[:2])[0] if (st == 0 and d and len(d) >= 2) else None


def get_i32(link, res):
    st, d = link.request(a.opcode(a.GET, a.CAT_SETTINGS, res))
    return struct.unpack("<i", d[:4])[0] if (st == 0 and d and len(d) >= 4) else None


def set_u16(link, res, val):
    st, _ = link.request(a.opcode(a.SET, a.CAT_SETTINGS, res), struct.pack("<H", val))
    return a.STATUS.get(st, st)


def set_i32(link, res, val):
    st, _ = link.request(a.opcode(a.SET, a.CAT_SETTINGS, res), struct.pack("<i", val))
    return a.STATUS.get(st, st)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port")
    ap.add_argument("--num", type=int, help="vbat_scale_num (1..10000)")
    ap.add_argument("--den", type=int, help="vbat_scale_den (1..10000)")
    ap.add_argument("--offset", type=int, help="vbat_offset_mv, signed (-500..500)")
    args = ap.parse_args()

    port = args.port or find_port()
    print(f"Opening {port} @ {BAUD} ...")
    link = Link(port)

    num = get_u16(link, a.SET_VBAT_SCALE_NUM)
    den = get_u16(link, a.SET_VBAT_SCALE_DEN)
    off = get_i32(link, a.SET_VBAT_OFFSET_MV)
    print(f"  current: num={num} den={den} offset={off}mv"
          + (f"  (ratio {num/den:.4f})" if num and den else ""))

    if args.num is not None:
        print(f"  set num={args.num}: {set_u16(link, a.SET_VBAT_SCALE_NUM, args.num)}")
    if args.den is not None:
        print(f"  set den={args.den}: {set_u16(link, a.SET_VBAT_SCALE_DEN, args.den)}")
    if args.offset is not None:
        print(f"  set offset={args.offset}: {set_i32(link, a.SET_VBAT_OFFSET_MV, args.offset)}")

    if args.num is not None or args.den is not None or args.offset is not None:
        num = get_u16(link, a.SET_VBAT_SCALE_NUM)
        den = get_u16(link, a.SET_VBAT_SCALE_DEN)
        off = get_i32(link, a.SET_VBAT_OFFSET_MV)
        print(f"  now:     num={num} den={den} offset={off}mv"
              + (f"  (ratio {num/den:.4f})" if num and den else ""))

    st, data = link.request(a.OP_SYS_DEVICE_STATE)
    if st == 0:
        print("  DEVICE_STATE:", a.decode_device_state(data))

    link.close()


if __name__ == "__main__":
    main()
