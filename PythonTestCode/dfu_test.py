#!/usr/bin/env python3
"""
Exercise "Reboot to DFU" (fw 0.9.12+, EXECUTE Commands/0x05).

The command sets the nBOOT0 option byte to 0 and launches an option-byte
reload, so the device boots into the STM32 ROM bootloader (USB DFU,
VID 0x0483 / PID 0xDF11) and STAYS there until reflashed with nBOOT0=1.

  python dfu_test.py            # trigger, then watch for the DFU device
  python dfu_test.py --port COM6
  python dfu_test.py --no-trigger   # just watch

If the DFU device does not appear within ~10 s, tap NRST or unplug/replug
the USB cable once — some hosts don't re-enumerate a bus-powered device
across the fast option-byte-reload reset. Recover with:  .\dfu_flash.ps1
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
    end = time.time() + 0.2
    while time.time() < end:
        for op, st, d in re.feed(ser.read(64)):
            if op == a.OP_SYS_IDENTITY and st == 0:
                return a.decode_identity(d)
    return None


def dfu_present():
    try:
        out = subprocess.run([PROG, "-l", "usb"], capture_output=True, text=True,
                             timeout=15).stdout
    except Exception:
        return None
    return "Device Index" in out and "No STM32 device in DFU mode" not in out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--watch", type=float, default=15.0)
    ap.add_argument("--no-trigger", action="store_true")
    args = ap.parse_args()

    port = args.port or find_port()
    ser = serial.Serial(port, BAUD, timeout=0.05)
    print(f"port {port}")
    print(f"pre-trigger IDENTITY: {identity(ser)}")

    if not args.no_trigger:
        ser.reset_input_buffer()
        ser.write(a.build(a.OP_CMD_REBOOT_DFU))
        time.sleep(0.25)
        ack = [a.STATUS.get(st, st) for op, st, d in
               a.Reassembler().feed(ser.read(64)) if op == a.OP_CMD_REBOOT_DFU]
        print(f"REBOOT_DFU ack: {ack or '(reset beat the response out)'}")

    t0 = time.time()
    silent_at = dfu_at = None
    while time.time() - t0 < args.watch:
        el = time.time() - t0
        if silent_at is None and identity(ser) is None and el > 0.3:
            silent_at = el
            print(f"  t={el:4.1f}s  app offline")
        if dfu_at is None and dfu_present():
            dfu_at = el
            print(f"  t={el:4.1f}s  *** STM32 Bootloader (USB DFU) enumerated ***")
            break
        time.sleep(0.5)
    ser.close()

    print("\n---- verdict ----")
    if dfu_at is not None:
        print("PASS: device is in the ROM bootloader and USB DFU is live.")
        print("  reflash + restore normal boot with:  .\\dfu_flash.ps1")
    elif silent_at is not None:
        print("app went offline but no USB DFU device appeared.")
        print("  -> tap NRST or unplug/replug USB once, then re-check with")
        print("     STM32_Programmer_CLI -l usb   (or recover: .\\flash.ps1)")
    else:
        print("app never went offline — the command did not take. Check the path.")


if __name__ == "__main__":
    main()
