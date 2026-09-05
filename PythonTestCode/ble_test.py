#!/usr/bin/env python3
"""
BLE data-flow test for the InclinationMeter (RN4871 Transparent UART), API v2.

  python ble_test.py           # identity / device-state / temp / settings round-trip
  python ble_test.py --log     # subscribe to the device debug-log stream, print it live

Install:   pip install bleak
If connect fails: remove the device from Windows Settings > Bluetooth first
(GATT access to this unencrypted service needs no OS bond).
"""

import asyncio
import struct
import sys

try:
    from bleak import BleakScanner, BleakClient
except ImportError:
    sys.exit("Need bleak:  pip install bleak")

import apiv2 as a

NAME_PREFIX = "Leveltronic"
SVC_UUID = "49535343-fe7d-4ae5-8fa9-9fafd205e455"
TX_UUID  = "49535343-1e4d-4bd9-ba61-23c647249616"   # module -> host, Notify
RX_UUID  = "49535343-8841-43f4-a8d4-ecbe34729bb3"   # host -> module, Write


class BleLink:
    def __init__(self, client):
        self.client = client
        self.reasm = a.Reassembler()
        self.q = asyncio.Queue()

    def _on_notify(self, _sender, data: bytearray):
        for pkt in self.reasm.feed(bytes(data)):
            self.q.put_nowait(pkt)

    async def start(self):
        await self.client.start_notify(TX_UUID, self._on_notify)

    async def send(self, pkt: bytes):
        await self.client.write_gatt_char(RX_UUID, pkt, response=False)

    async def recv(self, timeout=3.0):
        try:
            return await asyncio.wait_for(self.q.get(), timeout)
        except asyncio.TimeoutError:
            return (None, None, None)


async def request(link, op, payload=b""):
    await link.send(a.build(op, payload))
    _op, status, data = await link.recv()
    return status, data


async def run_basic(link):
    st, data = await request(link, a.OP_SYS_IDENTITY)
    print(f"  IDENTITY      [{a.STATUS.get(st, st)}] {a.decode_identity(data) if st == 0 else (data or b'').hex()}")

    st, data = await request(link, a.OP_SYS_DEVICE_STATE)
    print(f"  DEVICE_STATE  [{a.STATUS.get(st, st)}] {a.decode_device_state(data) if st == 0 else (data or b'').hex()}")

    st, data = await request(link, a.opcode(a.GET, a.CAT_MEAS, a.MEAS_ONBOARD_TEMP))
    if st == 0 and len(data) >= 2:
        print(f"  TEMP          [OK] {struct.unpack('<h', data[:2])[0] / 100:+.2f} C")
    else:
        print(f"  TEMP          [{a.STATUS.get(st, st)}] {(data or b'').hex()}")

    getop = a.opcode(a.GET, a.CAT_SETTINGS, a.SET_STREAM_INTERVAL_MS)
    setop = a.opcode(a.SET, a.CAT_SETTINGS, a.SET_STREAM_INTERVAL_MS)
    st, data = await request(link, getop)
    if st == 0 and len(data) >= 2:
        cur = struct.unpack("<H", data[:2])[0]
        new = 250 if cur != 250 else 200
        st2, _ = await request(link, setop, struct.pack("<H", new))
        st3, data3 = await request(link, getop)
        back = struct.unpack("<H", data3[:2])[0] if (st3 == 0 and len(data3) >= 2) else None
        print(f"  SETTINGS      stream_interval_ms {cur} -> set {new} "
              f"[{a.STATUS.get(st2, st2)}] -> read back {back}")
    else:
        print(f"  SETTINGS      GET stream_interval_ms [{a.STATUS.get(st, st)}]")


async def run_log(link, min_sev=0):
    print(f"Subscribing to debug log (min severity {a.SEVERITY[min_sev]}). Ctrl+C to stop.\n")
    await link.send(a.build(a.opcode(a.SUBSCRIBE, a.CAT_DEBUG, a.DBG_LOG_STREAM), bytes([min_sev])))
    sub_op = a.opcode(a.SUBSCRIBE, a.CAT_DEBUG, a.DBG_LOG_STREAM)
    try:
        while True:
            op, st, data = await link.recv(timeout=5.0)
            if op != sub_op or st != 0 or data is None or len(data) < 3:
                continue
            issue, page, sev = data[0], data[1], data[2]
            print(f"  [{a.SEVERITY.get(sev, sev):5}] #{issue:<3} {data[3:].decode('ascii', 'replace')}")
    except KeyboardInterrupt:
        await link.send(a.build(a.opcode(a.UNSUBSCRIBE, a.CAT_DEBUG, a.DBG_LOG_STREAM)))
        await asyncio.sleep(0.1)
        print("\nunsubscribed.")


async def main():
    print(f"Scanning for {NAME_PREFIX}* ...")
    dev = None
    for d in await BleakScanner.discover(timeout=6.0):
        if d.name and d.name.startswith(NAME_PREFIX):
            dev = d
            break
    if not dev:
        sys.exit(f"No {NAME_PREFIX}* device found.")
    print(f"  found {dev.name}  [{dev.address}]")

    async with BleakClient(dev.address) as client:
        print(f"  connected: {client.is_connected}")
        link = BleLink(client)
        await link.start()
        if "--log" in sys.argv:
            await run_log(link)
        else:
            await run_basic(link)
        await client.stop_notify(TX_UUID)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
