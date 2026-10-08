#!/usr/bin/env python3
"""Host-side tests for the API v3 client library (PythonTestCode/apiv3.py) and its generated definitions.
No hardware. Run:  python tests/test_apiv3.py"""

import os
import struct
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(REPO, "PythonTestCode"))
sys.path.insert(0, os.path.join(REPO, "tools"))

import apiv3 as a          # noqa: E402
import api_spec as spec    # noqa: E402


class Framing(unittest.TestCase):
    def test_crc_check_value(self):
        self.assertEqual(a.crc16_ccitt(b"123456789"), 0x29B1)

    def test_build_parse_round_trip(self):
        pkt = a.build(a.OP_SYSTEM_RTC_SET, b"\x01\x02\x03")
        self.assertEqual(len(pkt), 6 + 3)
        op, status, data = a.parse(pkt)
        self.assertEqual(op, a.OP_SYSTEM_RTC_SET)
        self.assertEqual(status, 1)                 # a request's first payload byte reads as the "status"
        self.assertEqual(data, b"\x02\x03")

    def test_corruption_is_reported_as_none(self):
        pkt = bytearray(a.build(a.OP_SYSTEM_STATE_GET, bytes([0, 1, 2])))
        pkt[5] ^= 0x40
        op, status, data = a.parse(bytes(pkt))
        self.assertEqual(op, a.OP_SYSTEM_STATE_GET)
        self.assertIsNone(status)
        self.assertIsNone(a.parse(b"\x00\x01")[1])

    def test_byte_stream_reassembler_handles_splits_joins_and_garbage_length(self):
        r = a.Reassembler()
        p1 = a.build(a.OP_SYSTEM_STATE_GET, bytes([0]) + bytes(range(20)))
        p2 = a.build(a.OP_TOPICS_LIVE_GET, bytes([0]))
        out = []
        stream = p1 + p2
        for i in range(0, len(stream), 7):
            out += r.feed(stream[i:i + 7])
        self.assertEqual([o[0] for o in out], [a.OP_SYSTEM_STATE_GET, a.OP_TOPICS_LIVE_GET])
        # an impossible length resyncs instead of waiting forever
        r2 = a.Reassembler()
        r2.feed(b"\x01\x00\xFF\x00")
        self.assertLess(len(r2.buf), 4)

    def test_hid_fragmentation_round_trip(self):
        for n in (0, 1, 57, 58, 59, 100, 116, 121):
            pkt = a.build(a.OP_SYSTEM_IDENTITY_GET, bytes([0]) + bytes(range(n))[:n] if n else b"")
            reports = a.hid_reports(pkt)
            self.assertEqual(len(reports), (len(pkt) + 63) // 64)
            self.assertTrue(all(len(x) == 64 for x in reports))
            h = a.HidReassembler()
            out = []
            for rep in reports:
                out += h.feed(rep)
            self.assertEqual(len(out), 1, n)
            self.assertEqual(out[0][0], a.OP_SYSTEM_IDENTITY_GET)
            if n:
                self.assertEqual(len(out[0][2]), n)

    def test_hid_reassembler_survives_back_to_back_packets(self):
        h = a.HidReassembler()
        out = []
        for pkt in (a.build(a.OP_TOPICS_LIVE_GET, bytes([0]) * 70), a.build(a.OP_SYSTEM_STATE_GET, bytes([0]))):
            for rep in a.hid_reports(pkt):
                out += h.feed(rep)
        self.assertEqual([o[0] for o in out], [a.OP_TOPICS_LIVE_GET, a.OP_SYSTEM_STATE_GET])


class Generated(unittest.TestCase):
    def test_opcodes_are_unique_and_follow_the_layout(self):
        seen = {}
        for cat in spec.CATEGORIES:
            for r in cat.resources:
                for v in r.verbs:
                    op = getattr(a, "OP_%s_%s_%s" % (cat.name, r.name, v))
                    self.assertEqual(op, (spec.VERB_VALUE[v] << 12) | (cat.id << 8) | r.id)
                    self.assertNotIn(op, seen)
                    seen[op] = (cat.name, r.name, v)
        self.assertGreater(len(seen), 150)

    def test_every_payload_round_trips_through_its_encoder_and_decoder(self):
        def value(ctype, i):
            if ctype.startswith("char["):
                return ("t%d" % i).encode()
            if ctype == "f32":
                return float(i) + 0.5
            return i % 100
        for cat in spec.CATEGORIES:
            for r in cat.resources:
                for role, fields in (("response", r.rsp), ("request", r.req), ("push", r.push)):
                    if not fields or any(f.ctype == "char[0]" for f in fields):
                        continue
                    base = "%s_%s_%s" % (cat.name.lower(), r.name.lower(), role)
                    fmt = getattr(a, "FMT_" + base.upper(), None)
                    if fmt is None:
                        continue                       # field resources only carry a Value layout
                    vals = [value(f.ctype, i) for i, f in enumerate(fields)]
                    raw = struct.pack(fmt, *vals)
                    self.assertEqual(len(raw), getattr(a, "SIZE_" + base.upper()))
                    d = getattr(a, "decode_" + base)(raw)
                    for f, v in zip(fields, vals):
                        got = d[f.name]
                        if isinstance(v, bytes):
                            v = v.decode()
                        self.assertAlmostEqual(got, v, places=4 if isinstance(v, float) else 7, msg=base)
                    self.assertIsNone(getattr(a, "decode_" + base)(raw[:-1]) if len(raw) else None)

    def test_state_changing_resources_sit_at_or_above_0x40(self):
        for cat in spec.CATEGORIES:
            for r in cat.resources:
                if ("SET" in r.verbs or "EXECUTE" in r.verbs) and cat.name != "SYSTEM":
                    self.assertGreaterEqual(r.id, 0x40, (cat.name, r.name))

    def test_field_tables_cover_calibrations_and_settings(self):
        self.assertIn("S1_K", a.CALIBRATIONS)
        self.assertIn("RTC_TRIM", a.CALIBRATIONS)
        self.assertIn("AUTO_POWEROFF", a.SETTINGS)
        get_op, set_op, fmt, lo, hi, unit = a.setting_ops("AUTO_POWEROFF")
        self.assertEqual(get_op, a.OP_SETTINGS_AUTO_POWEROFF_GET)
        self.assertEqual(set_op, a.OP_SETTINGS_AUTO_POWEROFF_SET)
        self.assertEqual((fmt, lo, hi), ("<H", 0, 65535))

    def test_helpers_decode_known_payloads(self):
        state = struct.pack("<BBHBBBBBBBBBBB", 3, 87, 4012, 1, 0, 1, 0, 0, 1, 1, 1, 0, 0, 1)
        self.assertIn("CHARGING", a.format_state(state))
        self.assertIn("service=1", a.format_state(state))
        rtc = struct.pack("<HBBBBBBBB", 2026, 10, 8, 4, 19, 52, 30, 1, 128)
        self.assertIn("19:52:30.500", a.format_rtc(rtc))
        chunk = bytes([7]) + (bytes([0x01, 0x00, 0x00]) + bytes([0xFF, 0xFF, 0xFF]) + bytes(6))
        page, rows = a.decode_bulk_adc_chunk(chunk)
        self.assertEqual((page, rows[0][0], rows[0][1]), (7, 1, -1))
        self.assertEqual(a.level_state_word(0b011), "valid  S1 doubtful")

    def test_generated_files_are_current(self):
        import subprocess
        r = subprocess.run([sys.executable, os.path.join(REPO, "tools", "gen_api.py"), "--check"],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=1)
