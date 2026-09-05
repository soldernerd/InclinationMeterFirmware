"""
Device API v2 framing + opcode helpers, shared by hid_test.py (USB) and
ble_test.py (BLE). See docs/api-v2-spec.md / docs/api-reference.md.

Packet on the wire:  [OPCODE 2B LE][LEN 2B LE][PAYLOAD 0..LEN][CRC16 2B LE]
  LEN   = payload byte count
  CRC16 = CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflect, no xor-out)
          over OPCODE+LEN+PAYLOAD, little-endian on the wire.
Every response echoes the request opcode; payload[0] is an Api2Status,
followed by resource data only when status == OK.
"""

import struct

# ---- verbs / categories ----
GET, SET, EXECUTE, SUBSCRIBE, UNSUBSCRIBE, START_BULK, CANCEL_BULK = range(7)
CAT_SYSTEM, CAT_COMMANDS, CAT_CALIB, CAT_SETTINGS, CAT_MEAS, CAT_TOPICS, CAT_DEBUG, CAT_RAW, CAT_BULK = range(9)

def opcode(verb, cat, res):
    return ((verb & 0xF) << 12) | ((cat & 0xF) << 8) | (res & 0xFF)

# ---- well-known opcodes ----
OP_SYS_IDENTITY     = opcode(GET, CAT_SYSTEM, 0x00)
OP_SYS_DEVICE_STATE = opcode(GET, CAT_SYSTEM, 0x01)
OP_CMD_TEST_BEEP    = opcode(EXECUTE, CAT_COMMANDS, 0x00)

MEAS_ONBOARD_TEMP, MEAS_BATTERY_MV, MEAS_BATTERY_SOC = 0x00, 0x01, 0x02
DBG_LOG_STREAM = 0x00

# Settings resource indices (DeviceSettings field order) — a few useful ones:
SET_STREAM_INTERVAL_MS = 0x07
SET_TASK_BLE_MS        = 0x03

# ---- status codes ----
STATUS = {
    0x00: "OK", 0x01: "UNKNOWN_CATEGORY", 0x02: "VERB_NOT_VALID",
    0x03: "UNKNOWN_RESOURCE", 0x04: "BAD_CRC", 0x05: "BAD_LENGTH",
    0x06: "BUSY_RESOURCE", 0x07: "BUSY_EXCLUSIVE", 0x08: "INVALID_PARAMETER",
    0x09: "NOT_SUBSCRIBED", 0x0A: "NOTHING_TO_CANCEL",
}
SEVERITY = {0: "INFO", 1: "WARN", 2: "ERROR"}

HDR = 4
CRC = 2
MAX_PACKET = 128


def crc16_ccitt(data: bytes) -> int:
    c = 0xFFFF
    for b in data:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if (c & 0x8000) else (c << 1) & 0xFFFF
    return c


def build(op: int, payload: bytes = b"") -> bytes:
    body = struct.pack("<HH", op, len(payload)) + payload
    return body + struct.pack("<H", crc16_ccitt(body))


class Reassembler:
    """Rebuilds packets from a byte stream (BLE). USB delivers whole reports,
    so it can hand each report straight to parse() instead."""
    def __init__(self):
        self.buf = bytearray()

    def feed(self, data: bytes):
        out = []
        self.buf += data
        while len(self.buf) >= HDR:
            paylen = struct.unpack_from("<H", self.buf, 2)[0]
            total = HDR + paylen + CRC
            if total > MAX_PACKET:
                del self.buf[0]                 # resync
                continue
            if len(self.buf) < total:
                break
            out.append(parse(bytes(self.buf[:total])))
            del self.buf[:total]
        return out


def parse(pkt: bytes):
    """-> (opcode, status, data_bytes). status/data are None if malformed."""
    if len(pkt) < HDR + CRC:
        return (None, None, None)
    op, paylen = struct.unpack_from("<HH", pkt, 0)
    if HDR + paylen + CRC > len(pkt):
        return (op, None, None)
    body = pkt[:HDR + paylen]
    got = struct.unpack_from("<H", pkt, HDR + paylen)[0]
    if crc16_ccitt(body) != got:
        return (op, None, None)
    payload = pkt[HDR:HDR + paylen]
    if not payload:
        return (op, None, b"")
    return (op, payload[0], payload[1:])


def decode_identity(data: bytes):
    if len(data) < 27:
        return None
    maj, minr, pat = data[0], data[1], data[2]
    prod = data[3:19].split(b"\x00")[0].decode("ascii", "replace")
    ser = data[19:27].split(b"\x00")[0].decode("ascii", "replace")
    return f"fw v{maj}.{minr}.{pat}  product={prod!r}  serial={ser!r}"


def decode_device_state(data: bytes):
    if len(data) < 7:
        return None
    bst, soc, mv, usb, ble, cal = struct.unpack("<BBHBBB", data[:7])
    names = {0: "NORMAL", 1: "LOW", 2: "CRITICAL", 3: "CHARGING", 4: "FULL"}
    return (f"battery={names.get(bst, bst)} {soc}% {mv}mV  "
            f"usb={usb} ble={ble}  cal_valid={cal}")
