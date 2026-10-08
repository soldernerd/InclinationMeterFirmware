"""
Device API v3 host library: framing, CRC, reassembly (byte streams and USB HID reports), helpers.

The resource constants, opcodes and payload encoders/decoders are GENERATED from tools/api_spec.py into
apiv3_defs.py and re-exported here (`import apiv3 as a; a.OP_SYSTEM_IDENTITY_GET`, `a.decode_system_state_response`).
Protocol: docs/api-v3-spec.md; every resource: docs/api-reference.md.

Packet on the wire:  [OPCODE 2B LE][LEN 2B LE][PAYLOAD 0..LEN][CRC16 2B LE]
  CRC16 = CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflect, no xor-out) over OPCODE+LEN+PAYLOAD.
Every response echoes the request opcode; payload[0] is the status, followed by resource data only when status == OK.
"""

import struct

from apiv3_defs import *            # noqa: F401,F403  (constants, opcodes, FMT_/SIZE_/decode_/encode_)
import apiv3_defs as _defs

HDR = 4
CRC = 2
MAX_PACKET = 128
HID_REPORT = 64

STATUS = {0x00: "OK", 0x01: "UNKNOWN_CATEGORY", 0x02: "VERB_NOT_VALID", 0x03: "UNKNOWN_RESOURCE", 0x04: "BAD_CRC",
          0x05: "BAD_LENGTH", 0x06: "BUSY_RESOURCE", 0x07: "BUSY_EXCLUSIVE", 0x08: "INVALID_PARAMETER",
          0x09: "NOT_SUBSCRIBED", 0x0A: "NOTHING_TO_CANCEL",
          0x0B: "SERVICE_MODE_REQUIRED"}
SEVERITY = {0: "INFO", 1: "WARN", 2: "ERROR"}
BATTERY_STATE = {0: "NORMAL", 1: "LOW", 2: "CRITICAL", 3: "CHARGING", 4: "FULL"}

# Power-test mask bits (Commands POWER_TEST)
PWR_5V_RAIL   = 1 << 0
PWR_3V3_RAIL  = 1 << 1
PWR_AD9833    = 1 << 2
PWR_ADS131M04 = 1 << 3
PWR_BLE       = 1 << 4
PWR_DISPLAY   = 1 << 5
PWR_LEDS      = 1 << 6
PWR_CPU_SPIN  = 1 << 7
PWR_ALL       = 0xFF
PWR_BITS = [("5V rail", PWR_5V_RAIL), ("3V3 rail", PWR_3V3_RAIL), ("AD9833", PWR_AD9833),
            ("ADS131M04", PWR_ADS131M04), ("BLE (RN4871)", PWR_BLE), ("display", PWR_DISPLAY),
            ("LEDs", PWR_LEDS), ("CPU spin (no WFI)", PWR_CPU_SPIN)]

# Raw ADC bulk capture (Bulk RAW_ADC): chunks [status][page][sample x 12 B], a sample = ch0..ch3 as 3-byte signed LE
ADC_BULK_SAMPLE_COUNT     = 6144   # must match Config/config.h
ADC_BULK_CHUNK_SAMPLES    = 10
ADC_BULK_BYTES_PER_SAMPLE = 12
ADC_RAW_LSB_V             = 2.4 / (1 << 24)   # 1 raw code = 2.4 V / 2^24 (gain 1; full scale +-1.2 V)
ADC_FCLKIN_HZ             = 5.3333e6
_OSR_TABLE = {0: 128, 1: 256, 2: 512, 3: 1024, 4: 2048, 5: 4096, 6: 8192, 7: 16256}

PHASOR_ENTRY_BYTES = 34            # one PHASOR_STREAM push body: 8 x float32 + u16 seq


def build_interval(ms: int) -> bytes:
    """SUBSCRIBE request of interval resources (and the ignored field of event resources)."""
    return struct.pack("<I", ms)


# ---------------------------------------------------------------------------------------------- framing
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


def parse(pkt: bytes):
    """-> (opcode, status, data_bytes). status / data are None if the packet is malformed or fails its CRC."""
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


class Reassembler:
    """Rebuilds packets from a byte stream (BLE, UART). feed() returns a list of parse() results."""
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


def hid_reports(pkt: bytes):
    """Splits a packet into 64-byte HID OUT reports (zero padded); a packet starts at the beginning of a report."""
    return [pkt[i:i + HID_REPORT].ljust(HID_REPORT, b"\0") for i in range(0, len(pkt), HID_REPORT)]


class HidReassembler:
    """Rebuilds packets from 64-byte HID IN reports: the first report of a packet carries its LEN, the packet then
    occupies ceil((6 + LEN) / 64) reports (docs/api-v3-spec.md section 2.2). feed(report) returns parse() results."""
    def __init__(self):
        self.buf = bytearray()
        self.total = 0

    def feed(self, report: bytes):
        out = []
        if self.total == 0:
            if len(report) < HDR:
                return out
            paylen = struct.unpack_from("<H", report, 2)[0]
            self.total = HDR + paylen + CRC
            if self.total > MAX_PACKET:
                self.total = 0
                return out
            self.buf = bytearray()
        self.buf += report[:self.total - len(self.buf)]
        if len(self.buf) >= self.total:
            out.append(parse(bytes(self.buf)))
            self.buf = bytearray()
            self.total = 0
        return out


# ---------------------------------------------------------------------------------------------- readable forms
def format_identity(data: bytes):
    d = decode_system_identity_response(data)
    if d is None:
        return None
    return (f"fw v{d['fw_major']}.{d['fw_minor']}.{d['fw_patch']}  api v{d['api_version']}  "
            f"product={d['product']!r}  serial={d['serial']!r}  built {d['build']!r}")


def format_state(data: bytes):
    d = decode_system_state_response(data)
    if d is None:
        return None
    return (f"battery={BATTERY_STATE.get(d['battery_state'], d['battery_state'])} {d['battery_soc_pct']}% "
            f"{d['battery_mv']}mV  usb={d['usb_connected']} ble={d['ble_connected']} charging={d['charging']} "
            f"force={d['force_charging']} inhibit={d['charge_inhibited']}  3v3={d['rail_3v3_on']} "
            f"5v={d['rail_5v_on']}  demod={d['displacement_running']} stream={d['phasor_stream_active']} "
            f"bulk={d['bulk_active']} service={d['service_mode']}")


_WD = {1: "Mon", 2: "Tue", 3: "Wed", 4: "Thu", 5: "Fri", 6: "Sat", 7: "Sun"}


def format_rtc(data: bytes):
    d = decode_system_rtc_response(data)
    if d is None:
        return None
    tag = "" if d["is_set"] else "  (NOT SET)"
    return (f"{d['year']:04d}-{d['month']:02d}-{d['day']:02d} {_WD.get(d['weekday'], d['weekday'])} "
            f"{d['hour']:02d}:{d['minute']:02d}:{d['second']:02d}.{d['subsecond'] * 1000 // 256:03d}{tag}")


def build_rtc_set(year, month, day, hour, minute, second) -> bytes:
    return encode_system_rtc_request(year, month, day, hour, minute, second)


def _s24le(b, o):
    v = b[o] | (b[o + 1] << 8) | (b[o + 2] << 16)
    return v - 0x1000000 if (v & 0x800000) else v


def decode_bulk_adc_chunk(data: bytes):
    """Chunk payload after the status: [page:1][sample:12]xN, each sample ch0..ch3 as 3-byte signed LE codes.
    Returns (page, [(ch0, ch1, ch2, ch3), ...])."""
    if not data:
        return (None, [])
    body = data[1:]
    n = len(body) // ADC_BULK_BYTES_PER_SAMPLE
    rows = [tuple(_s24le(body, ADC_BULK_BYTES_PER_SAMPLE * i + 3 * c) for c in range(4)) for i in range(n)]
    return (data[0], rows)


def decode_phasor_entry(d: bytes):
    """One PHASOR_STREAM push body (after [issue_seq][page]): 8 x float32 LE + u16 seq."""
    return decode_topics_phasor_stream_push(d)


def format_adc_diag(data: bytes):
    """Multi-line human readable form of Diagnostics ADC."""
    d = decode_diagnostics_adc_response(data)
    if d is None:
        return None
    faults = {0: "none", 1: "OVERRUN", 2: "FRAMING", 3: "CRC", 4: "SLIP"}
    osr = _OSR_TABLE.get((d["clock"] >> 2) & 0x7, "?")
    lines = [
        f"ADS131M04 ID=0x{d['id']:04X} STATUS=0x{d['status']:04X} MODE=0x{d['mode']:04X} CLOCK=0x{d['clock']:04X} "
        f"(OSR {osr}) GAIN1=0x{d['gain1']:04X} CFG=0x{d['cfg']:04X}  readback "
        f"{'ok' if d['regs_read_ok'] else 'FAILED'}, clock {'matches' if d['clock'] == d['clock_expected'] else 'DIFFERS from'} "
        f"what was written",
        f"acquisition: produced {d['frames_produced']} drained {d['frames_drained']} overflow {d['ring_overflow']} "
        f"framing_err {d['framing_err']} crc_err {d['crc_err']} fault={faults.get(d['fault_code'], d['fault_code'])} "
        f"run {d['run_ms']} ms  deficit {d['frame_deficit']} [{d['frame_deficit_min']}..{d['frame_deficit_max']}]",
        f"last capture: {d['last_capture_samples']} samples, {d['last_capture_drops']} drops, {d['last_capture_ms']} ms",
    ]
    return "\n".join(lines)


def level_state_word(flags: int) -> str:
    """TILT_FLAGS / LIVE flags byte as text."""
    return ("valid" if flags & 1 else "NOT valid") + ("  S1 doubtful" if flags & 2 else "") + \
           ("  S2 doubtful" if flags & 4 else "")


def calibration_ops(name: str):
    """(get_opcode, set_opcode, struct_format, lo, hi, unit) of a Calibrations field by its spec name, e.g. 'S1_K'."""
    res, fmt, lo, hi, unit = _defs.CALIBRATIONS[name]
    return (opcode(GET, CAT_CALIBRATIONS, res), opcode(SET, CAT_CALIBRATIONS, res), fmt, lo, hi, unit)


def setting_ops(name: str):
    """Same for Settings fields, e.g. 'AUTO_POWEROFF'."""
    res, fmt, lo, hi, unit = _defs.SETTINGS[name]
    return (opcode(GET, CAT_SETTINGS, res), opcode(SET, CAT_SETTINGS, res), fmt, lo, hi, unit)


def decode_adc_diag(data: bytes):
    """Diagnostics ADC response -> the nested dict adc_diag.py prints (registers, derived OSR / fDATA, last capture,
    acquisition integrity)."""
    d = decode_diagnostics_adc_response(data)
    if d is None:
        return None
    osr_field = (d["clock"] >> 2) & 0x7
    osr = _OSR_TABLE[osr_field]
    elapsed = d["last_capture_ms"]
    faults = {0: "none", 1: "overrun", 2: "framing", 3: "crc", 4: "slip"}
    run_ms = d["run_ms"]
    return {
        "ID": d["id"], "STATUS": d["status"], "MODE": d["mode"],
        "CLOCK": d["clock"], "CLOCK_expected": d["clock_expected"],
        "GAIN1": d["gain1"], "CFG": d["cfg"],
        "regs_read_ok": bool(d["regs_read_ok"]), "ads_ok": bool(d["ads_ok"]),
        "OSR_field": osr_field, "OSR": osr,
        "fDATA_nominal_Hz": ADC_FCLKIN_HZ / (2 * osr),
        "last_capture": {"samples": d["last_capture_samples"], "drops": d["last_capture_drops"],
                         "elapsed_ms": elapsed,
                         "effective_Hz": (d["last_capture_samples"] * 1000.0 / elapsed) if elapsed else 0.0},
        "integrity": {
            "frames_produced": d["frames_produced"], "frames_drained": d["frames_drained"],
            "tim7_fires": d["tim7_fires"], "backlog": d["frames_produced"] - d["frames_drained"],
            "ring_overflow": d["ring_overflow"], "drain_clamped": d["drain_clamped"],
            "drain_clamp_max": d["drain_clamp_max"], "framing_err": d["framing_err"], "crc_err": d["crc_err"],
            "run_ms": run_ms, "expected_frames": int(run_ms * 64000 / 3072) if run_ms else 0,
            "frame_deficit": d["frame_deficit"], "frame_deficit_min": d["frame_deficit_min"],
            "frame_deficit_max": d["frame_deficit_max"],
            "word0_last": d["word0_last"], "crc_rx_last": d["crc_rx_last"], "crc_calc_last": d["crc_calc_last"],
            "crc_match": d["crc_rx_last"] == d["crc_calc_last"],
            "fault_code": d["fault_code"], "fault": faults.get(d["fault_code"], "?%d" % d["fault_code"]),
            "now_ms": d["now_ms"],
        },
    }
