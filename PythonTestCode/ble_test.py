#!/usr/bin/env python3
"""
Quick BLE data-flow test for the InclinationMeter (RN4871 Transparent UART).

The BLE sibling of hid_test.py: same svc_api protocol, same commands, over
the RN4871's Transparent UART service instead of USB HID.

Install:   pip install bleak         (cross-platform BLE; Windows 10 1709+)

If connect fails: remove the device from Windows Settings > Bluetooth
first. GATT access to this (unencrypted) service does not need an OS-level
pairing/bond, and an existing bond sometimes gets in bleak's way.

Protocol (Services/svc_api.c):
  frame = [CMD][LEN][PAYLOAD ...][CRC16_LSB][CRC16_MSB]   (no 64-byte pad on BLE)
  LEN   = payload byte count only
  CRC16 = CRC16-CCITT, poly 0x1021, init 0xFFFF, no reflection, no final XOR,
          over [CMD][LEN][PAYLOAD], little-endian on the wire
"""

import asyncio
import struct
import sys

try:
    from bleak import BleakScanner, BleakClient
except ImportError:
    sys.exit("Need bleak:  pip install bleak")

NAME_PREFIX = sys.argv[1] if len(sys.argv) > 1 else "Leveltronic"

# RN4871 / Microchip "ISSC Transparent UART" UUIDs
SVC_UUID = "49535343-fe7d-4ae5-8fa9-9fafd205e455"
TX_UUID  = "49535343-1e4d-4bd9-ba61-23c647249616"   # module -> host, Notify
RX_UUID  = "49535343-8841-43f4-a8d4-ecbe34729bb3"   # host -> module, Write

CMD_GET_STATUS     = 0x01
CMD_START_STREAM   = 0x04
CMD_STOP_STREAM    = 0x05
CMD_GET_IDENTITY   = 0x0D
RSP_GET_STATUS     = 0x81
RSP_GET_IDENTITY   = 0x8D
RSP_ACK            = 0xA0
RSP_NACK           = 0xA1
NOTIFY_STREAM_DATA = 0xF2

MAX_PAYLOAD = 60


def crc16_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if (crc & 0x8000) else (crc << 1) & 0xFFFF
    return crc


def build_frame(cmd: int, payload: bytes = b"") -> bytes:
    body = bytes([cmd, len(payload)]) + payload
    return body + struct.pack("<H", crc16_ccitt(body))


class Reassembler:
    """Notifications can split or coalesce frames; rebuild [CMD][LEN][..][CRC]."""
    def __init__(self):
        self.buf = bytearray()
        self.frames = asyncio.Queue()

    def feed(self, data: bytes):
        self.buf += data
        while len(self.buf) >= 2:
            paylen = self.buf[1]
            if paylen > MAX_PAYLOAD:
                del self.buf[0]           # misaligned — resync
                continue
            need = 2 + paylen + 2
            if len(self.buf) < need:
                break
            frame = bytes(self.buf[:need])
            del self.buf[:need]
            self.frames.put_nowait(frame)


def parse(frame: bytes):
    return frame[0], frame[2:2 + frame[1]]


async def get_reply(reasm, timeout=3.0):
    try:
        return parse(await asyncio.wait_for(reasm.frames.get(), timeout))
    except asyncio.TimeoutError:
        return None, b""


async def main():
    print(f"Scanning for a device named {NAME_PREFIX}* ...")
    dev = None
    for d in await BleakScanner.discover(timeout=6.0):
        if d.name and d.name.startswith(NAME_PREFIX):
            dev = d
            break
    if not dev:
        sys.exit(f"No {NAME_PREFIX}* device found. Is it advertising / already connected elsewhere?")
    print(f"  found {dev.name}  [{dev.address}]")

    reasm = Reassembler()

    async with BleakClient(dev.address) as client:
        print(f"  connected: {client.is_connected}")
        await client.start_notify(TX_UUID, lambda _s, data: reasm.feed(bytes(data)))

        async def send(cmd, payload=b""):
            await client.write_gatt_char(RX_UUID, build_frame(cmd, payload), response=False)

        await send(CMD_GET_IDENTITY)
        cmd, p = await get_reply(reasm)
        if cmd == RSP_GET_IDENTITY and len(p) >= 27:
            maj, minr, pat = p[0], p[1], p[2]
            prod = p[3:19].split(b"\x00")[0].decode("ascii", "replace")
            ser  = p[19:27].split(b"\x00")[0].decode("ascii", "replace")
            print(f"  IDENTITY  fw v{maj}.{minr}.{pat}  product={prod!r}  serial={ser!r}")
        else:
            print(f"  IDENTITY  unexpected reply cmd={cmd} payload={p.hex()}")

        await send(CMD_GET_STATUS)
        cmd, p = await get_reply(reasm)
        if cmd == RSP_GET_STATUS and len(p) >= 13:
            soc, mv, bst, ble, usb, scl, pc1, pc2, cal, fj, fn, fp = \
                struct.unpack("<BHBBBBBBBBBB", p[:13])
            print(f"  STATUS    soc={soc}%  vbat={mv}mV  batt_state={bst}  "
                  f"usb={usb} ble={ble}  scl3300_ok={scl} pcap1_ok={pc1} pcap2_ok={pc2}  "
                  f"cal_valid={cal}  fw v{fj}.{fn}.{fp}")
        else:
            print(f"  STATUS    unexpected reply cmd={cmd} payload={p.hex()}")

        print("\nStarting stream (Ctrl+C to stop). temp/soc/timestamp should move.\n")
        await send(CMD_START_STREAM)
        cmd, _ = await get_reply(reasm)
        print(f"  START_STREAM -> {'ACK' if cmd == RSP_ACK else ('NACK' if cmd == RSP_NACK else cmd)}")

        n = 0
        try:
            while True:
                cmd, p = await get_reply(reasm, timeout=3.0)
                if cmd is None:
                    print("  (no stream packet in 3 s)")
                    continue
                if cmd != NOTIFY_STREAM_DATA or len(p) < 20:
                    print(f"  other packet cmd={cmd} {p.hex()}")
                    continue
                tp, tx, ty, tc, soc, fl, ts = struct.unpack("<iiihBBI", p[:20])
                n += 1
                print(f"  #{n:<4} t={ts:>9}ms  pcap={tp:+9d}  scl_x={tx:+9d}  scl_y={ty:+9d} "
                      f"(um/m)  temp={tc/100:+.2f}C  soc={soc}%  flags={fl:#04x}")
        except KeyboardInterrupt:
            print("\nStopping stream ...")
            await send(CMD_STOP_STREAM)
            await asyncio.sleep(0.2)
        finally:
            await client.stop_notify(TX_UUID)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
