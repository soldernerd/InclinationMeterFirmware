#!/usr/bin/env python3
"""
Wired-UART data-flow test for the InclinationMeter, API v2.

The third API transport: USART3 on the J4 / STDC14 debug header, 115200 8N1.
Reachable with nothing but a USB-serial cable — no USB enumeration, no BLE
central. Same protocol and same checks as hid_test.py / ble_test.py.

  python uart_test.py                 # auto-detect the port, one full round-trip
  python uart_test.py --port COM7     # or name it
  python uart_test.py --log           # subscribe to the debug-log stream, print live
  python uart_test.py --env           # poll the BME280 (temp/pressure/humidity) once/sec
  python uart_test.py --port COM7 --log

Install:   pip install pyserial
"""

import datetime
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


def request(link, op, payload=b""):
    link.send(a.build(op, payload))
    _op, status, data = link.recv()
    return status, data


def run_basic(link):
    st, data = request(link, a.OP_SYS_IDENTITY)
    print(f"  IDENTITY      [{a.STATUS.get(st, st)}] "
          f"{a.decode_identity(data) if st == 0 else (data or b'').hex()}")

    st, data = request(link, a.OP_SYS_DEVICE_STATE)
    print(f"  DEVICE_STATE  [{a.STATUS.get(st, st)}] "
          f"{a.decode_device_state(data) if st == 0 else (data or b'').hex()}")

    st, data = request(link, a.opcode(a.GET, a.CAT_MEAS, a.MEAS_ONBOARD_TEMP))
    if st == 0 and len(data) >= 2:
        print(f"  TEMP          [OK] {struct.unpack('<h', data[:2])[0] / 100:+.2f} C")
    else:
        print(f"  TEMP          [{a.STATUS.get(st, st)}] {(data or b'').hex()}")

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

    # RTC: read, set to this host's wall clock, read back.
    st, data = request(link, a.opcode(a.GET, a.CAT_SYSTEM, a.SYS_RTC))
    before = a.decode_rtc(data) if st == 0 else (a.STATUS.get(st, st))
    n = datetime.datetime.now()
    st2, _ = request(link, a.opcode(a.SET, a.CAT_SYSTEM, a.SYS_RTC),
                     a.build_rtc_set(n.year, n.month, n.day, n.hour, n.minute, n.second))
    st3, data3 = request(link, a.opcode(a.GET, a.CAT_SYSTEM, a.SYS_RTC))
    after = a.decode_rtc(data3) if st3 == 0 else (a.STATUS.get(st3, st3))
    print(f"  RTC           [{before}] -> set [{a.STATUS.get(st2, st2)}] -> [{after}]")

    # Signal analysis (ADS131M04 stream): off at boot; start then stop.
    st_on, _  = request(link, a.OP_CMD_SIGNAL_ANALYSIS, b"\x01")
    st_off, _ = request(link, a.OP_CMD_SIGNAL_ANALYSIS, b"\x00")
    print(f"  SIGNAL_ANALYS start [{a.STATUS.get(st_on, st_on)}] "
          f"-> stop [{a.STATUS.get(st_off, st_off)}]")

    # BME280 (WP9) — GET temp / pressure / humidity / ok
    env = read_bme280(link)
    if env is None:
        print("  BME280        GET failed")
    else:
        t, p, h, ok = env
        fresh = "fresh" if ok else "STALE (sensor not connected)"
        print(f"  BME280        {t:+.2f} C  {p:.1f} hPa  {h:.1f} %RH   [{fresh}]")


def read_bme280(link):
    """GET the four BME280 Measurements resources. Returns
    (temp_C, pressure_hPa, humidity_pct, ok_bool) or None on any failure."""
    def meas(res):
        st, d = request(link, a.opcode(a.GET, a.CAT_MEAS, res))
        return d if st == 0 else None
    dt, dp, dh, dok = (meas(a.MEAS_BME280_TEMP), meas(a.MEAS_BME280_PRESS),
                       meas(a.MEAS_BME280_HUMID), meas(a.MEAS_BME280_OK))
    if None in (dt, dp, dh, dok):
        return None
    return (struct.unpack("<h", dt[:2])[0] / 100,
            struct.unpack("<I", dp[:4])[0] / 100,
            struct.unpack("<H", dh[:2])[0] / 100,
            bool(dok[0]))


def run_env(link, period=1.0):
    print("Polling BME280 once/sec. Ctrl+C to stop.\n")
    try:
        while True:
            env = read_bme280(link)
            ts = datetime.datetime.now().strftime("%H:%M:%S")
            if env is None:
                print(f"  {ts}  GET failed")
            else:
                t, p, h, ok = env
                print(f"  {ts}  {t:+6.2f} C   {p:8.2f} hPa   {h:5.1f} %RH"
                      f"   {'' if ok else '[STALE]'}")
            time.sleep(period)
    except KeyboardInterrupt:
        print("\nstopped.")


def run_log(link, min_sev=0):
    print(f"Subscribing to debug log (min severity {a.SEVERITY[min_sev]}). Ctrl+C to stop.\n")
    link.send(a.build(a.opcode(a.SUBSCRIBE, a.CAT_DEBUG, a.DBG_LOG_STREAM), bytes([min_sev])))
    sub_op = a.opcode(a.SUBSCRIBE, a.CAT_DEBUG, a.DBG_LOG_STREAM)
    try:
        while True:
            op, st, data = link.recv(timeout=5.0)
            if op != sub_op or st != 0 or data is None or len(data) < 3:
                continue
            issue, _page, sev = data[0], data[1], data[2]
            print(f"  [{a.SEVERITY.get(sev, sev):5}] #{issue:<3} {data[3:].decode('ascii', 'replace')}")
    except KeyboardInterrupt:
        link.send(a.build(a.opcode(a.UNSUBSCRIBE, a.CAT_DEBUG, a.DBG_LOG_STREAM)))
        time.sleep(0.1)
        print("\nunsubscribed.")


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
            run_log(link)
        elif "--env" in args:
            run_env(link)
        else:
            run_basic(link)
    finally:
        link.close()


if __name__ == "__main__":
    main()
