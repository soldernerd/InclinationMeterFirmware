#!/usr/bin/env python3
"""
BLE data-flow test for the InclinationMeter (RN4871 Transparent UART), API v3.

  python ble_test.py           # identity / state / health / temperature / settings / clock / environment round trip
  python ble_test.py --log     # subscribe to the device debug-log stream, print it live
  python ble_test.py --live    # subscribe to the live tilt reading

Install:   pip install bleak
If connect fails: remove the device from Windows Settings > Bluetooth first
(GATT access to this unencrypted service needs no OS bond).
"""

import asyncio
import queue
import sys
import threading

try:
    from bleak import BleakScanner, BleakClient
except ImportError:
    sys.exit("Need bleak:  pip install bleak")

import apiv3 as a
import device_checks as dc

NAME_PREFIX = "Leveltronic"
SVC_UUID = "49535343-fe7d-4ae5-8fa9-9fafd205e455"
TX_UUID  = "49535343-1e4d-4bd9-ba61-23c647249616"   # module -> host, Notify
RX_UUID  = "49535343-8841-43f4-a8d4-ecbe34729bb3"   # host -> module, Write


class BleLink:
    """Blocking send()/recv() on top of bleak: the asyncio client lives in a background thread."""
    def __init__(self):
        self.loop = asyncio.new_event_loop()
        self.thread = threading.Thread(target=self.loop.run_forever, daemon=True)
        self.thread.start()
        self.reasm = a.Reassembler()
        self.q = queue.Queue()
        self.client = None

    def _run(self, coro, timeout=30.0):
        return asyncio.run_coroutine_threadsafe(coro, self.loop).result(timeout)

    def _on_notify(self, _sender, data: bytearray):
        for pkt in self.reasm.feed(bytes(data)):
            self.q.put(pkt)

    def connect(self):
        async def scan_and_connect():
            dev = None
            for d in await BleakScanner.discover(timeout=6.0):
                if d.name and d.name.startswith(NAME_PREFIX):
                    dev = d
                    break
            if not dev:
                raise RuntimeError(f"No {NAME_PREFIX}* device found.")
            print(f"  found {dev.name}  [{dev.address}]")
            self.client = BleakClient(dev.address)
            await self.client.connect()
            await self.client.start_notify(TX_UUID, self._on_notify)
        self._run(scan_and_connect())
        print(f"  connected: {self.client.is_connected}")

    def send(self, pkt: bytes):
        self._run(self.client.write_gatt_char(RX_UUID, pkt, response=False))

    def recv(self, timeout=3.0):
        try:
            return self.q.get(timeout=timeout)
        except queue.Empty:
            return (None, None, None)

    def close(self):
        if self.client is not None:
            try:
                self._run(self.client.disconnect())
            except Exception:
                pass
        self.loop.call_soon_threadsafe(self.loop.stop)


def main():
    print(f"Scanning for {NAME_PREFIX}* ...")
    link = BleLink()
    try:
        link.connect()
    except RuntimeError as e:
        link.close()
        sys.exit(str(e))
    try:
        if "--log" in sys.argv:
            dc.run_log(link)
        elif "--live" in sys.argv:
            dc.run_live(link)
        else:
            dc.run_basic(link)
    finally:
        link.close()


if __name__ == "__main__":
    main()
