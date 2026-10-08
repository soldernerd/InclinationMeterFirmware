#!/usr/bin/env python3
"""
Wired-UART data-flow test for the InclinationMeter, API v3.

The third API transport: USART3 on the J4 / STDC14 debug header, 115200 8N1.
Reachable with nothing but a USB-serial cable — no USB enumeration, no BLE
central. Same protocol and same checks as hid_test.py / ble_test.py.

  python uart_test.py                 # auto-detect the port, one full round-trip
  python uart_test.py --port COM7     # or name it
  python uart_test.py --log           # subscribe to the debug-log stream, print live
  python uart_test.py --env           # poll the BME280 (temp/pressure/humidity) once/sec
  python uart_test.py --topics        # subscribe to the env + device-status topics, live
  python uart_test.py --live          # subscribe to the live tilt reading (about 4 values/s)
  python uart_test.py --charge        # force-start charging regardless of SOC (needs USB)
  python uart_test.py --port COM7 --log

Install:   pip install pyserial
"""

import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("Need pyserial:  pip install pyserial")

import apiv3 as a
import device_checks as dc

BAUD = 115200

# ST-Link on-board VCP (and common USB-serial bridges) so --port can be omitted.
_KNOWN_VID_PID = {
    (0x0483, 0x374B), (0x0483, 0x374E), (0x0483, 0x374F), (0x0483, 0x3752),  # ST-Link v2.1/v3
    (0x0403, 0x6001), (0x0403, 0x6015),                                      # FTDI
    (0x10C4, 0xEA60),                                                        # CP210x
}


def find_port():
    ports = list(list_ports.comports())
    for p in ports:
        if p.vid is not None and (p.vid, p.pid) in _KNOWN_VID_PID:
            return p.device
    if len(ports) == 1:
        return ports[0].device
    if ports:
        sys.exit("Multiple serial ports; pass --port:\n  " +
                 "\n  ".join(f"{p.device}  {p.description}" for p in ports))
    sys.exit("No serial ports found.")


class UartLink:
    def __init__(self, port):
        self.ser = serial.Serial(port, BAUD, timeout=0.1)
        self.reasm = a.Reassembler()
        self._pending = []

    def send(self, pkt: bytes):
        self.ser.write(pkt)

    def recv(self, timeout=3.0):
        deadline = time.time() + timeout
        while True:
            if self._pending:
                return self._pending.pop(0)
            if time.time() >= deadline:
                return (None, None, None)
            chunk = self.ser.read(64)
            if chunk:
                self._pending.extend(self.reasm.feed(chunk))

    def close(self):
        self.ser.close()


def main():
    args = sys.argv[1:]
    port = None
    if "--port" in args:
        port = args[args.index("--port") + 1]
    port = port or find_port()
    print(f"Opening {port} @ {BAUD} ...")
    try:
        link = UartLink(port)
    except serial.SerialException as e:
        sys.exit(f"open failed ({e}). Wrong port, or another program has it open.")
    try:
        if "--log" in args:
            dc.run_log(link)
        elif "--env" in args:
            dc.run_env(link)
        elif "--topics" in args:
            dc.run_topics(link)
        elif "--live" in args:
            dc.run_live(link)
        elif "--charge" in args:
            s, _ = dc.request(link, a.OP_COMMANDS_FORCE_CHARGE_EXECUTE)
            print(f"  FORCE CHARGE  [{dc.st(s)}]  (charges while USB present; clears on full / unplug)")
        else:
            dc.run_basic(link)
    finally:
        link.close()


if __name__ == "__main__":
    main()
