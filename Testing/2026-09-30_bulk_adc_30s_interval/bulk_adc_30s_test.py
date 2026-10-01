"""
Long-term monitor via periodic raw-ADC bulk captures instead of the
continuous demod stream (2026-09-30, at the user's request): every
INTERVAL_S seconds, pull one full-rate (20833.33 Hz) 6144-sample x 4-channel
raw ADC capture and append it to a single growing binary log.

Bulk capture and the real-time displacement demod are mutually exclusive by
firmware design (svc_displacement_is_running() gate, Services/svc_api.c's
dispatch_bulk()) -- there is no continuous displacement stream during this
test. The demod is explicitly stopped once at the start and left stopped for
the whole run; only periodic bulk captures happen.

Binary log format (data/bulk_adc_log.bin), one record per capture, no
padding, little-endian:
  float64  t_s          seconds since script start
  uint32   capture_idx  0-based
  uint32   sample_count number of samples actually received (== 6144 unless truncated by a timeout)
  uint32   gap_count    page-sequence gaps / bad-CRC events during this capture's transfer
  bytes    sample_count * 12   raw ch0..ch3, each 3-byte little-endian signed, packed (ADC_BULK_BYTES_PER_SAMPLE)
Record size when sample_count==6144: 8+4+4+4 + 6144*12 = 73748 bytes.

Reuses bulk_adc_csv.py's proven capture() logic (same file, PythonTestCode/)
rather than re-deriving the chunked-transfer reassembly.
"""
import struct
import sys
import time

sys.path.insert(0, r"C:\Users\lfaes\OneDrive\EmbeddedSystems\InclinationMeterFirmware\PythonTestCode")
import apiv2 as a
import serial

PORT = "COM5"
INTERVAL_S = 30.0
DURATION_S = 24.0 * 3600.0     # 24h, matching the earlier long-term-test precedent
STATUS_INTERVAL_S = 1800.0     # console STATUS line cadence (same as the 24h granite test)
CAPTURE_TIMEOUT_S = 20.0
OUT_BIN = r"C:\Users\lfaes\OneDrive\EmbeddedSystems\InclinationMeterFirmware\Testing\2026-09-30_bulk_adc_30s_interval\data\bulk_adc_log.bin"


def modular_gap(prev, cur):
    return prev is not None and cur != ((prev + 1) & 0xFF)


def do_capture(ser, timeout):
    """One bulk raw-ADC capture -> (list of (ch0,ch1,ch2,ch3) tuples, gap_count).
    Same reassembly logic as PythonTestCode/bulk_adc_csv.py's capture()."""
    reasm = a.Reassembler()
    op = a.OP_BULK_RAW_ADC_START

    ser.reset_input_buffer()
    ser.write(a.build(op))

    rows = []
    acked = False
    last_page = None
    gaps = 0
    deadline = time.time() + timeout

    while len(rows) < a.ADC_BULK_SAMPLE_COUNT:
        if time.time() > deadline:
            ser.write(a.build(a.OP_BULK_RAW_ADC_CANCEL))
            return rows, gaps  # truncated -- caller records sample_count < 6144
        chunk = ser.read(4096)
        if not chunk:
            continue
        for pkt_op, status, data in reasm.feed(chunk):
            if pkt_op != op:
                continue
            if status is None:
                gaps += 1
                continue
            if not acked:
                if status != 0x00:
                    raise RuntimeError(f"START_BULK NACK: {a.STATUS.get(status, status)}")
                acked = True
                continue
            if status != 0x00:
                continue
            page, samples = a.decode_bulk_adc_chunk(data)
            if modular_gap(last_page, page):
                gaps += 1
            last_page = page
            rows.extend(samples)

    return rows[:a.ADC_BULK_SAMPLE_COUNT], gaps


def pack_record(t_s, capture_idx, rows, gaps):
    hdr = struct.pack("<dIII", t_s, capture_idx, len(rows), gaps)
    # Each 24-bit signed channel value packed into 3 little-endian bytes
    # directly (not a 4-byte int), matching the on-wire/RAM layout exactly --
    # same ADC_BULK_BYTES_PER_SAMPLE stride bulk_adc_csv.py assumes.
    body = bytearray(len(rows) * 12)
    off = 0
    for (c0, c1, c2, c3) in rows:
        for v in (c0, c1, c2, c3):
            vv = v & 0xFFFFFF
            body[off] = vv & 0xFF
            body[off + 1] = (vv >> 8) & 0xFF
            body[off + 2] = (vv >> 16) & 0xFF
            off += 3
    return hdr + bytes(body)


def main():
    ser = serial.Serial(PORT, 115200, timeout=0.5)
    ser.reset_input_buffer()

    def req(op, payload=b"", timeout=2.0):
        ser.write(a.build(op, payload))
        deadline = time.time() + timeout
        buf = b""
        while time.time() < deadline:
            chunk = ser.read(256)
            if chunk:
                buf += chunk
                reasm = a.Reassembler()
                for op2, st, data in reasm.feed(buf):
                    if op2 == op:
                        return st, data
        return None, None

    st, d = req(a.OP_SYS_IDENTITY)
    print("IDENTITY:", st, a.decode_identity(d) if st == 0 else d, flush=True)
    if st != 0:
        sys.exit("IDENTITY failed")

    # Stop the demod once -- required for bulk capture (mutually exclusive),
    # and we want it OFF for the whole run per the user's request (periodic
    # bulk captures only, no continuous stream this time).
    st, d = req(a.OP_CMD_SIGNAL_ANALYSIS, bytes([0]))
    print("stop displacement demod ->", st, d, flush=True)

    f = open(OUT_BIN, "ab")
    t0 = time.time()
    idx = 0
    next_capture = 0.0
    next_status = STATUS_INTERVAL_S
    total_gaps = 0
    total_truncated = 0

    print(f"STATUS t=0.0min n=0 -- periodic bulk-ADC capture test starting, "
          f"every {INTERVAL_S:.0f}s, PORT={PORT}", flush=True)

    try:
        while (time.time() - t0) < DURATION_S:
            now = time.time() - t0
            if now < next_capture:
                time.sleep(min(0.5, next_capture - now))
                continue

            cap_t0 = time.time()
            rows, gaps = do_capture(ser, CAPTURE_TIMEOUT_S)
            cap_dt = time.time() - cap_t0
            truncated = len(rows) < a.ADC_BULK_SAMPLE_COUNT
            if truncated:
                total_truncated += 1
            total_gaps += gaps

            rec = pack_record(time.time() - t0, idx, rows, gaps)
            f.write(rec)
            f.flush()

            if truncated or gaps:
                print(f"  capture {idx}: {len(rows)}/{a.ADC_BULK_SAMPLE_COUNT} samples, "
                      f"{gaps} gaps, took {cap_dt:.1f}s" +
                      (" TRUNCATED" if truncated else ""), flush=True)

            idx += 1
            next_capture += INTERVAL_S
            # If a capture ever took longer than INTERVAL_S, don't try to
            # "catch up" with back-to-back captures -- just resume on the
            # next multiple of INTERVAL_S from start.
            if next_capture < (time.time() - t0):
                next_capture = (time.time() - t0)

            t = time.time() - t0
            if t >= next_status:
                print(f"STATUS t={t/60:6.1f}min captures={idx} total_gaps={total_gaps} "
                      f"truncated={total_truncated}", flush=True)
                next_status += STATUS_INTERVAL_S

    except Exception as e:
        print(f"ERROR at t={time.time()-t0:.1f}s: {e}", flush=True)
        raise
    finally:
        f.close()
        print(f"DONE. {idx} captures, total_gaps={total_gaps}, truncated={total_truncated}, "
              f"wrote {OUT_BIN}", flush=True)
        # Leave the demod stopped -- deliberately not restarting it here;
        # the user asked for a bulk-only session, and re-enabling the
        # continuous demod is a separate, explicit step afterward.
        ser.close()


if __name__ == "__main__":
    main()
