# Device API v3 -- protocol specification

Firmware 0.11.0 and later. **This page is the protocol** (framing, verbs, status codes, how a request is validated,
subscriptions, bulk transfers, the transports). **[api-reference.md](api-reference.md) is the list of every resource**
with its opcode, request and response layout; it is generated from `tools/api_spec.py`, the single source of truth that
also produces the firmware's ID definitions and tables and the host-side constants (`PythonTestCode/apiv3_defs.py`).

v3 keeps the v2 framing and CRC and regroups the resources; it is a deliberate, one-time break with v2 (section 9 maps
every v2 resource to its v3 place). A client can tell: System IDENTITY reports `api_version = 3`.

---

## 1. Goals

* One packet format, one dispatcher, reused verbatim by every transport (USB HID, BLE, wired UART). The transports are
  thin adapters that move bytes.
* Everything the instrument does is reachable: all calibrations and settings (get / set), all status, every control
  command including the debug ones, all auxiliary measurements, the live reading, precision measurements, the phasor
  stream and the raw ADC capture.
* The wire format is described once. Adding a resource means editing `tools/api_spec.py`, running
  `python tools/gen_api.py` and writing the handler.

## 2. Packet format

```
[OPCODE 2 B][LEN 2 B][PAYLOAD LEN B][CRC16 2 B]       total on the wire: 6 + LEN
```

* All multi-byte values are **little-endian**; floats are IEEE-754 binary32.
* `CRC16` is CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection, no final XOR) over opcode, length and payload.
  It is enforced at the API layer on every transport, because the wired UART has no link-layer error detection.
* `LEN` is at most **122** (a packet is at most 128 bytes). A response payload is `[status][data...]`, so a response
  carries at most 121 data bytes; a subscription push `[issue_seq][page][data...]` at most 119.
* Every response echoes the opcode of the request. Subscription pushes and bulk chunks use the opcode of the request
  that started them (SUBSCRIBE / START_BULK); there is no separate "unsolicited" opcode.

### 2.1 Byte-stream transports (BLE, UART)

Bytes are fed to a reassembler: once the 4-byte header is in, the length is known and the packet is complete after
`6 + LEN` bytes. A declared length above the packet limit is dropped (counted in System HEALTH `rx_malformed`), a partial
packet that stalls for 250 ms is abandoned.

### 2.2 USB HID

The HID report is a fixed 64 bytes. An API packet always **starts at the beginning of a report** and occupies
`ceil((6 + LEN) / 64)` consecutive reports; the last one is zero padded. The receiver learns the packet length from the
`LEN` field in the first report, so it knows how many reports belong to the packet. Both directions use this; a packet of
up to 64 bytes is exactly one report as before.

### 2.3 Counters

Two independent wrapping 1-byte counters in subscription pushes: **issue_seq** counts the pushes of one subscription
(gap detection: compare modulo 256), **page** is reserved for multi-piece deliveries (always 0 today; bulk chunks carry
their own page counter as the first data byte).

## 3. Opcode

16 bits: `[VERB:4][CATEGORY:4][RESOURCE:8]`.

| Verb | Value | Meaning |
|---|---:|---|
| `GET` | 0 | read |
| `SET` | 1 | write and persist (calibrations, settings, the clock) |
| `EXECUTE` | 2 | fire a one-shot action |
| `SUBSCRIBE` | 3 | start pushes of a resource |
| `UNSUBSCRIBE` | 4 | stop them |
| `START_BULK` | 5 | start a bulk transfer |
| `CANCEL_BULK` | 6 | abort it |

Categories (full lists in the reference): `0x0` System, `0x1` Commands, `0x2` Calibrations, `0x3` Settings,
`0x4` Measurements, `0x5` Topics, `0x6` Debug log, `0x7` Diagnostics, `0x8` Bulk, `0x9` Procedures.

**ID ranges.** Every resource that changes state (SET, EXECUTE) has an ID **>= 0x40** (System RTC aside). All v2 IDs were
below 0x40, so a stale v2 client that sends an old state-changing opcode gets `UNKNOWN_RESOURCE` and can never trigger a
different action by accident. Within Commands, Calibrations and Settings the high nibble groups related resources
(for example Commands 0x4x power, 0x5x measurement, 0x6x maintenance, 0x7x debug).

## 4. How a request is validated

The dispatcher checks, in this order, and answers with the first failure; nothing after a failed stage runs.

1. the category exists -> `UNKNOWN_CATEGORY`
2. the verb is valid for the category -> `VERB_NOT_VALID`
3. the resource exists -> `UNKNOWN_RESOURCE`
4. the verb is valid for that resource -> `VERB_NOT_VALID`
5. the CRC is correct -> `BAD_CRC`
6. the payload length is what the resource expects -> `BAD_LENGTH` (GET, UNSUBSCRIBE, START/CANCEL_BULK: none; SET and
   EXECUTE: the request size in the reference, a range for the variable ones; SUBSCRIBE: 4 bytes (`u32 interval_ms`), or
   what the reference states for event resources)
7. the resource's handler runs -> any of `OK`, `BUSY_RESOURCE`, `BUSY_EXCLUSIVE`, `INVALID_PARAMETER`,
   `NOTHING_TO_CANCEL`, ...

### Status codes

| Code | Name | Meaning |
|---:|---|---|
| 0x00 | `OK` | done |
| 0x01 | `UNKNOWN_CATEGORY` | no such category |
| 0x02 | `VERB_NOT_VALID` | the verb does not apply to this category / resource |
| 0x03 | `UNKNOWN_RESOURCE` | no such resource in the category |
| 0x04 | `BAD_CRC` | the request failed its CRC |
| 0x05 | `BAD_LENGTH` | the payload length is wrong |
| 0x06 | `BUSY_RESOURCE` | try again shortly: the resource is busy (EEPROM write in progress, a prerequisite is not met) |
| 0x07 | `BUSY_EXCLUSIVE` | another exclusive activity owns the ADC (a bulk capture or the phasor stream): wait for it to finish |
| 0x08 | `INVALID_PARAMETER` | the payload parses but a value is out of range, or violates a cross-field rule |
| 0x09 | `NOT_SUBSCRIBED` | UNSUBSCRIBE without an active subscription on this transport |
| 0x0A | `NOTHING_TO_CANCEL` | CANCEL_BULK with no transfer active |

An error response carries the status byte only. No request goes unanswered, except a frame too short or inconsistent
to be parsed at all (counted in `rx_malformed`).

## 5. Subscriptions

A subscription belongs to one transport; the opcode (SUBSCRIBE + category + resource) is its identity, so subscribing
again updates it instead of creating a second one. SUBSCRIBE and UNSUBSCRIBE are acknowledged with a status byte first;
the pushes follow as separate packets under the SUBSCRIBE opcode: `[status OK][issue_seq][page 0][data]`.

* **Interval** (`Measurements`, most `Topics`): request `u32 interval_ms`, 50 .. 3 600 000. The data is what GET returns.
* **Event** (`Topics` LIVE and PHASOR_STREAM, `Debug log`, `Procedures`): the device pushes when there is something new;
  the interval field must be present (4 bytes) but is ignored, except where the reference gives a different request
  (the log takes one byte, the minimum severity).
* Pushes are **not urgent**: they never use the transport's reserved transmit space, so a response can always get out.
  A full transmit ring drops the push (counted in System HEALTH, `*_tx_dropped`).
* A connect or disconnect of the transport clears all its subscriptions. The phasor stream releases the ADC.

## 6. Bulk transfers

`START_BULK` is acknowledged with a status byte, then the data follows as chunks under the START_BULK opcode:
`[status OK][page][data...]`, paced to the transport's transmit ring. At most one transfer exists device-wide (it uses
the RAM capture buffer and stops the demodulation). `CANCEL_BULK` aborts it at once. There is no retransmission: after
a CRC error or a page gap, cancel and restart.

## 7. Concurrency

* GET, SET and EXECUTE never block and answer within one scheduler tick.
* The **ADC has one owner**: the running demodulation, the phasor stream (a tap on the demodulation) or a bulk capture.
  Starting or stopping the demodulation while a capture or the stream is active answers `BUSY_EXCLUSIVE`.
* Persisting a SET waits for any earlier EEPROM write: while one is in flight, further SETs answer `BUSY_RESOURCE`.

## 8. Transports

| | USB HID | BLE | UART |
|---|---|---|---|
| link | Custom HID, VID 0x04D8 / PID 0xF08F, 64-byte reports (section 2.2) | RN4871 transparent UART service `Leveltronic_<MAC>` | USART3 on the debug header, 115200 8N1 |
| framing | report fragmentation | byte stream | byte stream |

All three can be connected at once; each has its own subscriptions and transmit ring (1 KiB, drop-newest, a 64-byte
reserve for responses).

## 9. Migrating from v2

| v2 | v3 |
|---|---|
| System 0x00 IDENTITY | System 0x00 (payload gained `api_version`, `max_payload`, `build`) |
| System 0x01 DEVICE_STATE | System 0x01 STATE (14 bytes; also Topics 0x01 STATUS) |
| System 0x02 RTC | System 0x03 RTC (GET gained `subsecond`) |
| Commands 0x00 TEST_BEEP / 0x01 DISPLACEMENT / 0x02 FORCE_CHARGE | 0x70 / 0x50 / 0x43 |
| Commands 0x03 POWER_TEST / 0x04 PIN_TEST / 0x05 REBOOT_DFU | 0x71 / 0x72 / 0x42 |
| Commands 0x06 ZERO_CAL / 0x07 PRECISION_MEASURE | 0x51 / 0x52 |
| Commands 0x08 END_CHARGING / 0x09 CHARGE_INHIBIT | 0x44 / 0x45 |
| *new* | POWER_OFF 0x40, REBOOT 0x41, FACTORY_DEFAULTS 0x60, CLEAR_COUNTERS 0x61, RAIL 0x73 |
| Calibrations 0x01,0x03,0x0D,0x0B (S1 k, zero, phase, invert) | 0x40, 0x41, 0x42, 0x43 |
| Calibrations 0x04,0x06,0x0E,0x0C (S2) | 0x44, 0x45, 0x46, 0x47 |
| Settings 0x0F,0x10,0x1C (vbat scale num/den, offset) | **Calibrations** 0x50, 0x51, 0x52 |
| Settings 0x11..0x18 (TMP236), 0x19 (LM35) | **Calibrations** 0x60..0x67, 0x68 |
| *new* | Calibrations 0x70 RTC_TRIM |
| Settings 0x00,0x03,0x04,0x05,0x06 (task periods) | Settings 0x40..0x44 (the display period, 0x02, is gone: it paced nothing) |
| Settings 0x0C,0x0D,0x0E (battery thresholds) | Settings 0x50, 0x51, 0x52 |
| Settings 0x1B AUTO_POWEROFF / 0x1A ENCODER | Settings 0x60 / 0x61 |
| Measurements 0x00..0x08 | unchanged |
| Measurements 0x09 / 0x0B / 0x0E (S1, S2, diff) | 0x09 / 0x0A / 0x0B (TILT_S1, TILT_S2, TILT_DIFF) |
| Measurements 0x0D (disp ok), 0x0A / 0x0C (residuals) | TILT_FLAGS 0x0C (bit0 valid, bit1/2 doubtful); residuals only in Topics RAW; new TILT_SEQ 0x0D |
| Topics 0x00 ENV | unchanged |
| Topics 0x01 STATUS | same id; payload = System STATE (the clock moved to System RTC) |
| Topics 0x02 PHASORS (snapshot) | removed (it aliased the 20 Hz resonance): use PHASOR_STREAM |
| Topics 0x03 RAW_DISPLACEMENT | Topics 0x03 RAW (quality bytes are now the window verdict) |
| Topics 0x04 SIGNAL_DIAG / 0x05 PHASOR_STREAM | 0x05 / 0x04 |
| *new* | Topics 0x02 LIVE |
| Debug 0x00 LOG | unchanged |
| Raw data 0x00 / 0x01 / 0x02 | **Diagnostics** 0x00 ADC / 0x01 POWER / 0x02 DISPLACEMENT (two dead fields dropped) |
| Raw data 0x03 ZERO_CAL_STATUS / 0x04 PRECISION_STATUS | **Procedures** 0x00 ZERO_CAL / 0x01 PRECISION (now also pushed on change) |
| Bulk 0x00 RAW_ADC | unchanged |

## 10. Changing the API

1. Edit `tools/api_spec.py` (resource, id, verbs, payload fields, documentation).
2. `python tools/gen_api.py` regenerates `Services/svc_api_defs.h`, `Services/svc_api_tables.c`,
   `PythonTestCode/apiv3_defs.py`, `docs/api-reference.md` and `tests/api_handler_stubs.inc`. `--check` (run by the
   tests' `make`) fails when a committed file is out of date.
3. Write the handler `api_h_<category>_<resource>` in `Services/svc_api_res_*.c` (a missing one is a link error).
   Settings and calibrations are rows in the spec and need no code except an optional cross-field check.
4. `tests/test_api_tables.c` exercises every generated row.
