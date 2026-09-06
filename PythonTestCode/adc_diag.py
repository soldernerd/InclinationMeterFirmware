#!/usr/bin/env python3
"""
ADS131M04 diagnostics over the wired-UART API.

Runs one throw-away bulk capture (to refresh the capture stats), then GETs
the raw-data diagnostic resource and prints the ADS register read-back
plus the effective sample rate.

  python adc_diag.py                 # auto-detect port
  python adc_diag.py --port COM6
  python adc_diag.py --no-capture    # just GET, don't run a capture first

Install:  pip install pyserial
"""

import argparse
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


def run_capture(ser, timeout=25.0):
    r = a.Reassembler()
    op = a.OP_BULK_RAW_ADC_START
    ser.reset_input_buffer()
    ser.write(a.build(op))
    got = 0
    acked = False
    deadline = time.time() + timeout
    while got < a.ADC_BULK_SAMPLE_COUNT and time.time() < deadline:
        d = ser.read(4096)
        if not d:
            continue
        for pop, st, dd in r.feed(d):
            if pop != op:
                continue
            if not acked:
                acked = True
                continue
            if st == 0x00:
                _, rows = a.decode_bulk_adc_chunk(dd)
                got += len(rows)
    return got


def get_diag(ser, timeout=3.0):
    r = a.Reassembler()
    ser.reset_input_buffer()
    ser.write(a.build(a.OP_RAW_ADC_DIAG))
    deadline = time.time() + timeout
    while time.time() < deadline:
        d = ser.read(512)
        if not d:
            continue
        for pop, st, dd in r.feed(d):
            if pop == a.OP_RAW_ADC_DIAG:
                if st != 0x00:
                    raise RuntimeError(f"diag GET -> {a.STATUS.get(st, st)}")
                return a.decode_adc_diag(dd)
    raise TimeoutError("no diag response")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--no-capture", action="store_true")
    args = ap.parse_args()

    port = args.port or find_port()
    print(f"Opening {port} @ {BAUD} ...")
    with serial.Serial(port, BAUD, timeout=0.2) as ser:
        if not args.no_capture:
            n = run_capture(ser)
            print(f"  ran a {n}-sample capture to refresh stats")
            time.sleep(0.2)
        d = get_diag(ser)

    print()
    print(f"  regs_read_ok : {d['regs_read_ok']}    ads_ok : {d['ads_ok']}")
    print(f"  ID     = 0x{d['ID']:04X}")
    print(f"  STATUS = 0x{d['STATUS']:04X}")
    print(f"  MODE   = 0x{d['MODE']:04X}")
    print(f"  CLOCK  = 0x{d['CLOCK']:04X}   (driver wrote 0x{d['CLOCK_expected']:04X})"
          f"   {'MATCH' if d['CLOCK'] == d['CLOCK_expected'] else '*** MISMATCH ***'}")
    print(f"  GAIN1  = 0x{d['GAIN1']:04X}")
    print(f"  CFG    = 0x{d['CFG']:04X}")
    print(f"  OSR field = {d['OSR_field']}  -> OSR {d['OSR']}  "
          f"-> fDATA nominal {d['fDATA_nominal_Hz']:.0f} Hz")
    lc = d["last_capture"]
    print(f"  last capture: {lc['samples']} samples, {lc['drops']} drops, "
          f"{lc['elapsed_ms']} ms  -> effective {lc['effective_Hz']:.0f} Hz")


if __name__ == "__main__":
    main()
