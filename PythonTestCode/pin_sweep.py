#!/usr/bin/env python3
"""
Static drive of the 6 MCU->level-converter signals (display + buzzer),
current measured per pattern on the Keysight E36104A. Hunts a short in
the 5V level converter (`EXECUTE Commands/0x04`, HAL_App/hal_pintest.h).

  python pin_sweep.py                 # 32-pattern sweep, DISP_ON pinned low
  python pin_sweep.py --disp-on       # 64 patterns incl. DISP_ON — PANEL MUST BE UNPLUGGED
  python pin_sweep.py --pat 0x14 --hold 10   # hold one pattern, log I/V fast
  python pin_sweep.py --ilim 0.3     # raise the supply current limit for this run (restored after)

Pattern bit map: 0 SCK  1 MOSI  2 CS  3 DISP_ON  4 VCOM  5 BUZZER.
A pattern that hangs the MCU is recovered with an ST-Link NRST pulse
between steps. Supply voltage is left at whatever it was; the current
limit is restored on exit.
"""

import argparse
import statistics
import subprocess
import sys
import time

import pyvisa
import serial
from serial.tools import list_ports

import apiv2 as a

PSU_RES = "USB0::0x2A8D::0x0802::MY55506105::0::INSTR"
PROG = r"C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe"
BITS = [("SCK", 0x01), ("MOSI", 0x02), ("CS", 0x04),
        ("DISP_ON", 0x08), ("VCOM", 0x10), ("BUZ", 0x20)]


def find_port():
    return next(p.device for p in list_ports.comports() if p.vid == 0x0483)


def nrst():
    subprocess.run([PROG, "-c", "port=SWD", "mode=UR", "-rst"],
                   capture_output=True, timeout=20)
    time.sleep(2.0)


class Rig:
    def __init__(self):
        self.psu = pyvisa.ResourceManager().open_resource(PSU_RES)
        self.psu.timeout = 3000
        self.port = find_port()
        self._open()

    def _open(self):
        self.s = serial.Serial(self.port, 115200, timeout=0.15)
        self.re = a.Reassembler()

    def reopen(self):
        try:
            self.s.close()
        except Exception:
            pass
        self._open()

    def send(self, op, pl=b""):
        self.s.reset_input_buffer()
        self.s.write(a.build(op, pl))
        time.sleep(0.18)
        for o, st, d in self.re.feed(self.s.read(96)):
            if o == op:
                return st
        return None

    def alive(self):
        self.s.reset_input_buffer()
        self.s.write(a.build(a.OP_SYS_IDENTITY))
        time.sleep(0.15)
        return any(o == a.OP_SYS_IDENTITY and st == 0
                   for o, st, d in self.re.feed(self.s.read(200)))

    def iv(self):
        return (float(self.psu.query("MEAS:CURR?")) * 1000,
                float(self.psu.query("MEAS:VOLT?")))


def lbl(p):
    return " ".join(n for n, b in BITS if p & b) or "(all low)"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--disp-on", action="store_true", help="include DISP_ON (panel MUST be unplugged)")
    ap.add_argument("--pat", help="hold a single pattern (hex/dec) instead of sweeping")
    ap.add_argument("--hold", type=float, default=6.0, help="seconds to hold each pattern")
    ap.add_argument("--ilim", type=float, help="supply current limit A for this run")
    args = ap.parse_args()

    rig = Rig()
    ilim0 = rig.psu.query("SOUR:CURR?").strip()
    if args.ilim:
        rig.psu.write(f"SOUR:CURR {args.ilim:.3f}")
        time.sleep(0.2)
    allow = 0x40 if args.disp_on else 0x00

    try:
        if args.pat is not None:
            pats = [int(args.pat, 0) & 0x3F]
        elif args.disp_on:
            pats = list(range(0x40))
        else:
            pats = [p for p in range(0x40) if not (p & 0x08)]

        if not rig.alive():
            nrst(); rig.reopen()
        i0, v0 = rig.iv()
        print(f"baseline {i0:.1f} mA  {v0:.3f} V   Iset {rig.psu.query('SOUR:CURR?').strip()}\n")

        for p in pats:
            if not rig.alive():
                nrst(); rig.reopen()
            rig.send(a.OP_CMD_PIN_TEST, bytes([(p & 0x3F) | allow]))
            t0 = time.time(); I = []; V = []
            while time.time() - t0 < args.hold:
                i, v = rig.iv(); I.append(i); V.append(v)
            al = rig.alive()
            fault = (min(V) < 3.5) or (max(I) > i0 + 40) or not al
            print(f"  0x{p:02X} {lbl(p):26} I[{min(I):5.0f}/{max(I):5.0f}/{statistics.mean(I):5.0f}] "
                  f"Vmin {min(V):.3f}  MCU {'OK' if al else 'DEAD'}  {'<== FAULT' if fault else ''}")
            if not al:
                nrst(); rig.reopen()
    finally:
        rig.send(a.OP_CMD_PIN_TEST, bytes([0x80]))
        time.sleep(0.5)
        rig.psu.write(f"SOUR:CURR {ilim0}")
        time.sleep(0.2)
        nrst(); rig.reopen()
        i, v = rig.iv()
        print(f"\nrestored  Iset {rig.psu.query('SOUR:CURR?').strip()}   {i:.1f} mA  {v:.3f} V  "
              f"MCU {'OK' if rig.alive() else 'DEAD'}")


if __name__ == "__main__":
    main()
