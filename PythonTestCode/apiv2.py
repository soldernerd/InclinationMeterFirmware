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
OP_CMD_TEST_BEEP       = opcode(EXECUTE, CAT_COMMANDS, 0x00)
OP_CMD_SIGNAL_ANALYSIS = opcode(EXECUTE, CAT_COMMANDS, 0x01)  # payload: 1 byte, 0=stop 1=start

# Bulk raw-ADC capture (category 0x8). START_BULK has no request payload;
# chunks come back under the SAME opcode, each: [status=OK][page:1][sample:16]xN
# with one sample = ch0..ch3 as int32 LE (raw sign-extended 24-bit codes).
BULK_RAW_ADC              = 0x00
OP_BULK_RAW_ADC_START     = opcode(START_BULK,  CAT_BULK, BULK_RAW_ADC)
OP_BULK_RAW_ADC_CANCEL    = opcode(CANCEL_BULK, CAT_BULK, BULK_RAW_ADC)
ADC_BULK_SAMPLE_COUNT     = 6144   # must match Config/config.h
ADC_BULK_CHUNK_SAMPLES    = 10     # must match Config/config.h
ADC_BULK_BYTES_PER_SAMPLE = 12     # 4 ch x 3-byte packed signed LE
ADC_RAW_LSB_V             = 2.4 / (1 << 23)   # 1 raw code = 2.4 V / 2^23 (gain 1)

# Raw data (category 0x7) — ADS131M04 register / capture diagnostics
OP_RAW_ADC_DIAG           = opcode(GET, CAT_RAW, 0x00)
ADC_FCLKIN_HZ             = 5.3333e6
_OSR_TABLE                = {0: 128, 1: 256, 2: 512, 3: 1024,
                            4: 2048, 5: 4096, 6: 8192, 7: 16256}

SYS_IDENTITY, SYS_DEVICE_STATE, SYS_RTC = 0x00, 0x01, 0x02
MEAS_ONBOARD_TEMP, MEAS_BATTERY_MV, MEAS_BATTERY_SOC = 0x00, 0x01, 0x02
DBG_LOG_STREAM = 0x00

# Settings resource indices (DeviceSettings field order) — a few useful ones:
SET_STREAM_INTERVAL_MS = 0x07
SET_TASK_BLE_MS        = 0x03
SET_AUTO_POWEROFF_S    = 0x1B

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


_WD = {1: "Mon", 2: "Tue", 3: "Wed", 4: "Thu", 5: "Fri", 6: "Sat", 7: "Sun"}


def decode_rtc(data: bytes):
    """GET 0x0/0x02 response payload: year u16, month, day, weekday, hour, min, sec, is_set."""
    if len(data) < 9:
        return None
    year, month, day, wd, hh, mm, ss, is_set = struct.unpack("<HBBBBBBB", data[:9])
    tag = "" if is_set else "  (NOT SET)"
    return f"{year:04d}-{month:02d}-{day:02d} {_WD.get(wd, wd)} {hh:02d}:{mm:02d}:{ss:02d}{tag}"


def decode_adc_diag(data: bytes):
    """Raw-data 0x7/0x00 GET response-after-status (24 B). Returns a dict."""
    if len(data) < 24:
        return None
    (rid, rst, rmode, rclk, rgain, rcfg, rclk_exp,
     rd_ok, ads_ok, samp, drops, elapsed) = struct.unpack("<HHHHHHHBBHHI", data[:24])
    osr_field = (rclk >> 2) & 0x7
    osr = _OSR_TABLE[osr_field]
    fdata_nominal = ADC_FCLKIN_HZ / (2 * osr)
    eff = (samp * 1000.0 / elapsed) if elapsed else 0.0
    return {
        "ID": rid, "STATUS": rst, "MODE": rmode,
        "CLOCK": rclk, "CLOCK_expected": rclk_exp,
        "GAIN1": rgain, "CFG": rcfg,
        "regs_read_ok": bool(rd_ok), "ads_ok": bool(ads_ok),
        "OSR_field": osr_field, "OSR": osr,
        "fDATA_nominal_Hz": fdata_nominal,
        "last_capture": {"samples": samp, "drops": drops, "elapsed_ms": elapsed,
                         "effective_Hz": eff},
    }


def _s24le(b, o):
    v = b[o] | (b[o + 1] << 8) | (b[o + 2] << 16)
    return v - 0x1000000 if (v & 0x800000) else v


def decode_bulk_adc_chunk(data: bytes):
    """A bulk raw-ADC chunk's payload-after-status: [page:1][sample:12]xN,
    each sample = ch0..ch3 as 3-byte little-endian signed 24-bit codes.
    Returns (page, [(ch0,ch1,ch2,ch3), ...]) with channels as signed ints."""
    if not data:
        return (None, [])
    page = data[0]
    body = data[1:]
    n = len(body) // ADC_BULK_BYTES_PER_SAMPLE
    rows = [tuple(_s24le(body, ADC_BULK_BYTES_PER_SAMPLE * i + 3 * c) for c in range(4))
            for i in range(n)]
    return (page, rows)


def build_rtc_set(year, month, day, hour, minute, second) -> bytes:
    """SET 0x1/0x02 payload (7 B): year u16 LE, month, day, hour, minute, second."""
    return struct.pack("<HBBBBB", year, month, day, hour, minute, second)
