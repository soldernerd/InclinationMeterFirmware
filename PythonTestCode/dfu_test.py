#!/usr/bin/env python3
"""
Test the "reboot to DFU" path (fw 0.9.8+, EXECUTE Commands/0x05).

Sends the command over the wired UART, then measures how long the
application stays down and reads the debug-log backlog once it (maybe)
comes back. Three outcomes:

  * app self-recovers after ~1-2 s AND the log shows
    "dfu: jump reached the ROM bootloader, which handed control back"
        -> the ROM bootloader bounced control back to the app.
  * app stays down until an external reset (never self-recovers here)
        -> the device is sitting in the ROM bootloader. If the native
           USB (PA11/PA12) is cabled to this PC, a USB DFU device
           enumerates and this script reports it.
  * app self-recovers with no bounce log
        -> reset happened but the jump never transferred (investigate).

  python dfu_test.py                 # trigger + observe
  python dfu_test.py --port COM6
  python dfu_test.py --watch 30
  python dfu_test.py --no-trigger    # just observe (e.g. after the menu action)

Restore the app with:  .\flash.ps1     (ST-Link / SWD)
Install:  pip install pyserial
"""

import argparse
import subprocess
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("Need pyserial:  pip install pyserial")

import apiv2 as a

BAUD = 115200
PROG = r"C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe"
LOG_SUB   = a.opcode(a.SUBSCRIBE,   a.CAT_DEBUG, a.DBG_LOG_STREAM)
LOG_UNSUB = a.opcode(a.UNSUBSCRIBE, a.CAT_DEBUG, a.DBG_LOG_STREAM)


def find_port():
    for p in list_ports.comports():
        if p.vid == 0x0483:
            return p.device
    ports = list(list_ports.comports())
    if len(ports) == 1:
        return ports[0].device
    sys.exit("pass --port")


def identity(ser):
    ser.reset_input_buffer()
    ser.write(a.build(a.OP_SYS_IDENTITY))
    re = a.Reassembler()
    end = time.time() + 0.15
    while time.time() < end:
        for op, st, d in re.feed(ser.read(64)):
            if op == a.OP_SYS_IDENTITY and st == 0:
                return a.decode_identity(d)
    return None


def read_log_backlog(ser, secs=2.0):
    ser.reset_input_buffer()
    ser.write(a.build(LOG_SUB, bytes([0])))          # min severity INFO
    lines, re = [], a.Reassembler()
    end = time.time() + secs
    while time.time() < end:
        for op, st, d in re.feed(ser.read(128)):
            if op == LOG_SUB and st == 0 and d and len(d) >= 3:
                lines.append(f"[{a.SEVERITY.get(d[2], d[2]):5}] {d[3:].decode('ascii', 'replace')}")
    ser.write(a.build(LOG_UNSUB))
    return lines


def dfu_usb_present():
    try:
        out = subprocess.run([PROG, "-l", "usb"], capture_output=True, text=True,
                             timeout=15).stdout
    except Exception:
        return None
    if "No STM32 device in DFU mode" in out:
        return False
    return ("Device Index" in out) or ("0xDF11" in out.upper())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--watch", type=float, default=25.0)
    ap.add_argument("--no-trigger", action="store_true")
    args = ap.parse_args()

    port = args.port or find_port()
    ser = serial.Serial(port, BAUD, timeout=0.05)
    print(f"port {port}")
    print(f"pre-trigger IDENTITY: {identity(ser)}")

    if not args.no_trigger:
        ser.reset_input_buffer()
        ser.write(a.build(a.OP_CMD_REBOOT_DFU))
        t0 = time.time()
        print("REBOOT_DFU sent")
    else:
        t0 = time.time()

    went_down = None
    came_back = None
    usb_seen = None
    while time.time() - t0 < args.watch:
        alive = identity(ser) is not None
        el = time.time() - t0
        if not alive and went_down is None and el > 0.3:
            went_down = el
            print(f"  t={el:5.2f}s  app went silent")
        if alive and went_down is not None and came_back is None:
            came_back = el
            print(f"  t={el:5.2f}s  app answering again")
            break
        if usb_seen is None and went_down is not None and dfu_usb_present():
            usb_seen = el
            print(f"  t={el:5.2f}s  *** USB DFU device present ***")
        time.sleep(0.1)

    print("\n---- verdict ----")
    if went_down is None:
        print("app never went silent — reset/jump did not take. Check the command path.")
        ser.close()
        return

    if came_back is not None:
        print(f"app was down {came_back - went_down:.2f} s, then self-recovered.")
        print("debug-log backlog after recovery:")
        for ln in read_log_backlog(ser) or ["  (none)"]:
            print(f"    {ln}")
        print("\n-> a bounce log line above == the ROM bootloader handed control "
              "back to the app (software DFU entry not viable on this path).")
    else:
        held = time.time() - t0 - went_down
        print(f"app still silent {held:.1f} s after going down — no self-recovery.")
        d = usb_seen is not None or dfu_usb_present()
        if d is True or usb_seen is not None:
            print("USB DFU device IS enumerating -> device is in the ROM bootloader. "
                  "Software DFU entry WORKS. Reflash with:")
            print(f'   & "{PROG}" -c port=USB1 -w build\\Debug\\InclinationMeterFirmware.elf -v -rst')
        else:
            print("No USB DFU device seen. Either the device is wedged, or it is in "
                  "the ROM bootloader but the native USB (PA11/PA12) is not cabled to "
                  "this PC. Plug it in and re-run, or recover with .\\flash.ps1.")
    ser.close()


if __name__ == "__main__":
    main()
