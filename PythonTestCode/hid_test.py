#!/usr/bin/env python3
"""
Quick HID data-flow test for the InclinationMeter  (VID 0x04D8 / PID 0xF08F).

No dedicated host app exists yet; this drives the svc_api protocol directly
over the vendor-defined HID interface, to prove reports flow both ways.

Install:   pip install hidapi        (the cython-hidapi package -> "import hid")
           Windows: that's all. Linux: may need a udev rule or sudo.

Protocol (Services/svc_api.c):
  frame = [CMD][LEN][PAYLOAD ...][CRC16_LSB][CRC16_MSB], zero-padded to 64 bytes
  LEN   = payload byte count only
  CRC16 = CRC16-CCITT, poly 0x1021, init 0xFFFF, no reflection, no final XOR,
          computed over [CMD][LEN][PAYLOAD], sent little-endian
"""

import struct
import sys
import time

try:
    import hid
except ImportError:
    sys.exit("Need the hidapi module:  pip install hidapi")

VID, PID   = 0x04D8, 0xF08F
REPORT_LEN = 64

CMD_GET_STATUS     = 0x01
CMD_START_STREAM   = 0x04
CMD_STOP_STREAM    = 0x05
CMD_GET_IDENTITY   = 0x0D
RSP_GET_STATUS     = 0x81
RSP_GET_IDENTITY   = 0x8D
RSP_ACK            = 0xA0
RSP_NACK           = 0xA1
NOTIFY_STREAM_DATA = 0xF2


def crc16_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if (crc & 0x8000) else (crc << 1) & 0xFFFF
    return crc


def build_frame(cmd: int, payload: bytes = b"") -> bytes:
    body  = bytes([cmd, len(payload)]) + payload
    frame = body + struct.pack("<H", crc16_ccitt(body))
    return frame + b"\x00" * (REPORT_LEN - len(frame))


def send(dev, cmd, payload=b""):
    # hidapi wants a leading report-ID byte; this device uses no report IDs -> 0x00
    dev.write(b"\x00" + build_frame(cmd, payload))


def recv(dev, timeout_ms=1000):
    data = dev.read(REPORT_LEN, timeout_ms=timeout_ms)
    if not data:
        return None, b""
    frame = bytes(data)
    cmd, ln = frame[0], frame[1]
    return cmd, frame[2:2 + ln]


def main():
    print(f"Opening {VID:#06x}:{PID:#06x} ...")
    dev = hid.device()
    try:
        dev.open(VID, PID)
    except OSError as e:
        sys.exit(f"open failed ({e}). Device unplugged, or another program has it open.")
    dev.set_nonblocking(False)

    try:
        print(f"  manufacturer: {dev.get_manufacturer_string()!r}   "
              f"product: {dev.get_product_string()!r}")

        send(dev, CMD_GET_IDENTITY)
        cmd, p = recv(dev)
        if cmd == RSP_GET_IDENTITY and len(p) >= 27:
            maj, minr, pat = p[0], p[1], p[2]
            prod = p[3:19].split(b"\x00")[0].decode("ascii", "replace")
            ser  = p[19:27].split(b"\x00")[0].decode("ascii", "replace")
            print(f"  IDENTITY  fw v{maj}.{minr}.{pat}  product={prod!r}  serial={ser!r}")
        else:
            print(f"  IDENTITY  unexpected reply cmd={cmd} payload={p.hex()}")

        send(dev, CMD_GET_STATUS)
        cmd, p = recv(dev)
        if cmd == RSP_GET_STATUS and len(p) >= 13:
            # ApiStatusPayload packs to 13 bytes: u8 u16 u8*10  (little-endian)
            soc, mv, bst, ble, usb, scl, pc1, pc2, cal, fj, fn, fp = \
                struct.unpack("<BHBBBBBBBBBB", p[:13])
            print(f"  STATUS    soc={soc}%  vbat={mv}mV  batt_state={bst}  "
                  f"usb={usb} ble={ble}  scl3300_ok={scl} pcap1_ok={pc1} pcap2_ok={pc2}  "
                  f"cal_valid={cal}  fw v{fj}.{fn}.{fp}")
        else:
            print(f"  STATUS    unexpected reply cmd={cmd} payload={p.hex()}")

        print("\nStarting stream (Ctrl+C to stop). Tilt the device — numbers should move.\n")
        send(dev, CMD_START_STREAM)
        cmd, _ = recv(dev)
        print(f"  START_STREAM -> {'ACK' if cmd == RSP_ACK else ('NACK' if cmd == RSP_NACK else cmd)}")

        n = 0
        while True:
            cmd, p = recv(dev, timeout_ms=2000)
            if cmd is None:
                print("  (no stream packet in 2 s)")
                continue
            if cmd != NOTIFY_STREAM_DATA or len(p) < 20:
                print(f"  other packet cmd={cmd} {p.hex()}")
                continue
            # ApiStreamPayload: i32 i32 i32 i16 u8 u8 u32  (packed, little-endian)
            tp, tx, ty, tc, soc, fl, ts = struct.unpack("<iiihBBI", p[:20])
            n += 1
            print(f"  #{n:<4} t={ts:>9}ms  pcap={tp:+9d}  scl_x={tx:+9d}  scl_y={ty:+9d} "
                  f"(um/m)  temp={tc/100:+.2f}C  soc={soc}%  flags={fl:#04x}")

    except KeyboardInterrupt:
        print("\nStopping stream ...")
        try:
            send(dev, CMD_STOP_STREAM)
            time.sleep(0.1)
        except OSError:
            pass
    finally:
        dev.close()


if __name__ == "__main__":
    main()
