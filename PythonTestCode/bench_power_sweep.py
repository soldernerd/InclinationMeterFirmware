#!/usr/bin/env python3
"""
Autonomous current-consumption bisection: Keysight E36104A (supply current
readback, read-only) paired with the firmware power-test mask over UART.

  python bench_power_sweep.py                 # full sweep, restores 0xFF at the end
  python bench_power_sweep.py --settle 1.5    # longer settle per step
  python bench_power_sweep.py --avg 20        # more current samples per step

The supply is only ever queried (MEAS:CURR?, MEAS:VOLT?) — voltage / output
/ protection are never changed.
"""

import argparse
import struct
import sys
import time

import pyvisa
import serial
from serial.tools import list_ports

import apiv2 as a

PSU_RES = "USB0::0x2A8D::0x0802::MY55506105::0::INSTR"
BAUD = 115200
_KNOWN = {(0x0483, 0x374B), (0x0483, 0x374E), (0x0483, 0x374F), (0x0483, 0x3752)}


def find_port():
    for p in list_ports.comports():
        if p.vid is not None and (p.vid, p.pid) in _KNOWN:
            return p.device
    ports = list(list_ports.comports())
    if len(ports) == 1:
        return ports[0].device
    sys.exit("pass --port")


class Dev:
    def __init__(self, port):
        self.ser = serial.Serial(port, BAUD, timeout=0.3)
        self.re = a.Reassembler()

    def _req(self, op, payload=b""):
        self.ser.reset_input_buffer()
        self.ser.write(a.build(op, payload))
        t = time.time()
        while time.time() - t < 2.0:
            for pop, st, d in self.re.feed(self.ser.read(128)):
                if pop == op:
                    return st, d
        return None, None

    def set_mask(self, mask):
        st, d = self._req(a.OP_CMD_POWER_TEST, struct.pack("<I", mask & 0xFFFFFFFF))
        return struct.unpack("<I", d[:4])[0] if (st == 0 and d and len(d) >= 4) else None

    def identity(self):
        st, d = self._req(a.OP_SYS_IDENTITY)
        return a.decode_identity(d) if st == 0 else "?"


class Psu:
    def __init__(self, res):
        self.rm = pyvisa.ResourceManager()
        self.h = self.rm.open_resource(res)
        self.h.timeout = 3000

    def idn(self):
        return self.h.query("*IDN?").strip()

    def read(self, n, gap=0.05):
        i = []
        for _ in range(n):
            i.append(float(self.h.query("MEAS:CURR?")))
            time.sleep(gap)
        v = float(self.h.query("MEAS:VOLT?"))
        i.sort()
        return sum(i) / len(i), v, i[0], i[-1]


# build-up order: rails first, then the loads that depend on them
BUILDUP = [
    ("3V3 rail",          a.PWR_3V3_RAIL),
    ("5V rail",           a.PWR_5V_RAIL),
    ("LEDs",              a.PWR_LEDS),
    ("CPU spin (no WFI)", a.PWR_CPU_SPIN),
    ("AD9833",            a.PWR_AD9833),
    ("ADS131M04",         a.PWR_ADS131M04),
    ("BLE (RN4871)",      a.PWR_BLE),
    ("display",           a.PWR_DISPLAY),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--settle", type=float, default=1.0)
    ap.add_argument("--avg", type=int, default=12)
    args = ap.parse_args()

    dev = Dev(args.port or find_port())
    psu = Psu(PSU_RES)
    print(f"PSU : {psu.idn()}")
    print(f"DUT : {dev.identity()}\n")

    def step(label, mask):
        applied = dev.set_mask(mask)
        time.sleep(args.settle)
        ma, v, lo, hi = psu.read(args.avg)
        ma *= 1000; lo *= 1000; hi *= 1000
        print(f"  {label:22} mask 0x{applied:02X}  {ma:7.2f} mA  "
              f"(spread {hi - lo:4.2f})  Vout {v:5.3f}")
        if v < 3.5:
            print("  !! Vout sagged — supply likely in CC / brownout; results suspect")
        return ma

    print("baseline")
    base_all = step("all on (0xFF)", a.PWR_ALL)

    print("\ncumulative build-up (from everything off)")
    prev = step("all off (0x00)", 0x00)
    running = 0
    rows = []
    for name, bit in BUILDUP:
        running |= bit
        cur = step(f"+ {name}", running)
        rows.append((name, cur - prev, cur))
        prev = cur

    print("\nisolated cut (from all on, one bit removed)")
    iso = []
    for name, bit in BUILDUP:
        cur = step(f"- {name}", a.PWR_ALL & ~bit)
        iso.append((name, base_all - cur))
        step("  (restore)", a.PWR_ALL)

    print("\n--- summary (mA, per-subsystem contribution) ---")
    print(f"{'subsystem':22} {'add-delta':>10} {'cut-delta':>10}")
    for (n, add_d, _), (_, cut_d) in zip(rows, iso):
        print(f"{n:22} {add_d:10.2f} {cut_d:10.2f}")
    print(f"\nall-on {base_all:.1f} mA")

    print("\nrestoring 0xFF")
    step("all on (0xFF)", a.PWR_ALL)


if __name__ == "__main__":
    main()
