#!/usr/bin/env python3
"""CRC-16/CCITT-FALSE reference oracle (poly 0x1021, init 0xFFFF, no
reflect, no xor-out) — the same algorithm as Math/math_crc.c and
PythonTestCode/apiv2.py's crc16_ccitt.

Runs with plain CPython (no compiler needed), unlike the C tests. Use it
to (a) sanity-check the algorithm against the catalogue check value and
(b) regenerate the pinned expected values in tests/test_math_crc.c.

  python tests/oracle_crc.py            # print the vectors used by the C test
  python tests/oracle_crc.py --verify   # also cross-check apiv2.crc16_ccitt
"""
import sys


def crc16_ccitt_false(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if (crc & 0x8000) else (crc << 1) & 0xFFFF
    return crc


ADS_FRAME15 = bytes([
    0x01, 0x0F, 0x00,
    0x00, 0x00, 0x00,
    0x7F, 0xFF, 0xC0,
    0x80, 0x00, 0x40,
    0x00, 0x00, 0x00,
])

VECTORS = {
    '"123456789"':      b"123456789",
    'empty':            b"",
    'one 0x00 byte':    b"\x00",
    '"A"':              b"A",
    '"deadbeef"':       b"deadbeef",
    'ADS131M04 frame0..14': ADS_FRAME15,
}


def main() -> int:
    print("CRC-16/CCITT-FALSE:")
    for name, v in VECTORS.items():
        print(f"  {name:<24} 0x{crc16_ccitt_false(v):04X}")

    check = crc16_ccitt_false(b"123456789")
    ok = check == 0x29B1
    print(f'\ncatalogue check value ("123456789") = 0x{check:04X}  '
          f'{"OK" if ok else "MISMATCH — algorithm is wrong"}')

    if "--verify" in sys.argv:
        try:
            sys.path.insert(0, "PythonTestCode")
            import apiv2
            same = all(crc16_ccitt_false(v) == apiv2.crc16_ccitt(v) for v in VECTORS.values())
            print(f"apiv2.crc16_ccitt agrees on every vector: {same}")
            ok = ok and same
        except Exception as e:  # noqa: BLE001
            print(f"(could not cross-check apiv2: {e})")

    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
