# Device API v2 — Host Reference (ported subset)

The complete contract for host software talking to this firmware build.
Design rationale is in `api-v2-spec.md`; this document is what a host
developer needs and nothing more.

This build implements a **subset** of the full v2 design: System status,
Commands (test beep / signal-analysis toggle / force-charge), Settings
(every `DeviceSettings` field), Measurements (onboard temp, battery,
BME280), Topic groups (environmental + device status), a Debug-messages
log stream, Raw data (ADS131M04 diagnostics, `0x7/0x00`), and Bulk
transfers (raw ADC capture, `0x8/0x00`). Calibrations (0x2) and the LM35
external-temp resource are not present yet. Sections below cover the
main categories; see `Services/svc_api.h` for the exhaustive resource
list.

---

## Packet format

```
[OPCODE 2B LE] [LEN 2B LE] [PAYLOAD 0..LEN] [CRC16 2B LE]
```

- `LEN` = payload byte count. Total on the wire is always `6 + LEN`. No padding.
- `CRC16` = CRC-16/CCITT-FALSE (poly `0x1021`, init `0xFFFF`, no reflection,
  no final XOR), computed over `OPCODE + LEN + PAYLOAD`, little-endian on the wire.
- Every response **echoes the request's OPCODE**. The first payload byte of
  every response is a **status code** (below); resource data follows only
  when status == `OK`.
The identical framing runs over three transports:

- **USB**: Custom HID, VID `0x04D8` / PID `0xF08F`. One HID OUT report =
  one request packet (all current requests fit in 64 bytes). Responses
  arrive as HID IN reports, zero-padded to 64; read `LEN` to find the
  real end. (`PythonTestCode/hid_test.py`)
- **BLE**: RN4871 Transparent UART, a raw byte stream — the host must
  reassemble packets by `LEN`. Service `49535343-FE7D-4AE5-8FA9-9FAFD205E455`;
  write requests to `49535343-8841-43F4-A8D4-ECBE34729BB3`; enable
  notifications on `49535343-1E4D-4BD9-BA61-23C647249616` for responses.
  Advertises as `Leveltronic_<MAC>`. (`PythonTestCode/ble_test.py`)
- **Wired UART**: USART3 on the J4 / STDC14 debug header, 115200 8N1, a
  raw byte stream reassembled by `LEN` exactly like BLE. No connect
  handshake — it is "connected" whenever the cable is attached; if
  nothing is listening, responses are just transmitted into the void.
  (`PythonTestCode/uart_test.py`)

**Backpressure.** Each transport has a bounded outbound queue. Direct
responses to your requests are prioritised — they use a slice of the
queue reserved for exactly that — so a response always gets out even
when a stream has filled the rest. Subscription/stream frames are
dropped newest-first if you stop draining the link; each drop bumps an
internal `*_tx_dropped_count` and emits one `WARN` on the debug-log
stream at the start of each overflow episode. Keep reading notifications
promptly when subscribed.

## OPCODE

16 bits: `[VERB:4 (top)] [CATEGORY:4] [RESOURCE:8]`.

| Verb | val | | Category | val |
|---|---|---|---|---|
| GET | 0x0 | | System status | 0x0 |
| SET | 0x1 | | Commands | 0x1 |
| EXECUTE | 0x2 | | Settings | 0x3 |
| SUBSCRIBE | 0x3 | | Measurements | 0x4 |
| UNSUBSCRIBE | 0x4 | | Debug messages | 0x6 |

`OPCODE = (verb << 12) | (cat << 8) | res`.

## Status codes (first response payload byte)

| | | | | |
|---|---|---|---|---|
| 0x00 OK | 0x01 UNKNOWN_CATEGORY | 0x02 VERB_NOT_VALID | 0x03 UNKNOWN_RESOURCE | 0x04 BAD_CRC |
| 0x05 BAD_LENGTH | 0x06 BUSY_RESOURCE | 0x07 BUSY_EXCLUSIVE | 0x08 INVALID_PARAMETER | 0x09 NOT_SUBSCRIBED |
| 0x0A NOTHING_TO_CANCEL | | | | |

---

## System status (0x0)

Identity and Device state are GET-only; **RTC (0x02) is GET and SET** — the
one writable system-status resource.

### `GET 0x0/0x00` — Identity  → opcode `0x0000`
Request payload: none. Response data (27 B):

| off | type | field |
|---|---|---|
| 0 | u8 | fw_major |
| 1 | u8 | fw_minor |
| 2 | u8 | fw_patch |
| 3 | char[16] | product string (NUL-padded) |
| 19 | char[8] | serial string (NUL-padded) |

Worked example — request `00 00 00 00 <crc>`, response
`00 00 1C 00 00 <27 bytes> <crc>` (status OK, then payload).

### `GET 0x0/0x01` — Device state  → opcode `0x0001`
Request payload: none. Response data (7 B):

| off | type | field |
|---|---|---|
| 0 | u8 | battery_state (0 NORMAL, 1 LOW, 2 CRITICAL, 3 CHARGING, 4 FULL) |
| 1 | u8 | battery_soc_pct |
| 2 | u16 | battery_mv |
| 4 | u8 | usb_connected (0/1) |
| 5 | u8 | ble_connected (0/1) |
| 6 | u8 | calibration_valid (0/1) |

### `GET 0x0/0x02` — RTC datetime  → opcode `0x0002`
Request payload: none. Response data (9 B):

| off | type | field |
|---|---|---|
| 0 | u16 | year (e.g. 2026) |
| 2 | u8 | month (1–12) |
| 3 | u8 | day (1–31) |
| 4 | u8 | weekday (1 Mon … 7 Sun) |
| 5 | u8 | hour (0–23) |
| 6 | u8 | minute (0–59) |
| 7 | u8 | second (0–59) |
| 8 | u8 | is_set (0 = never set since power-up / 1 = set) |

### `SET 0x0/0x02` — RTC datetime  → opcode `0x1002`
Request payload (7 B): `year u16 LE, month, day, hour, minute, second`
(weekday is recomputed from the date). `INVALID_PARAMETER` for an
out-of-range field, `BUSY_RESOURCE` if the RTC write fails. The calendar
keeps running through the auto power-off Standby; it is lost only on full
power removal (no backup battery), after which `is_set` reads 0.

---

## Commands (0x1) — EXECUTE only

### `EXECUTE 0x1/0x00` — Test beep  → opcode `0x2100`
Request payload: none. Response: status only. Beeps the buzzer ~100 ms.

---

## Measurements (0x4) — GET, SUBSCRIBE, UNSUBSCRIBE

| res | opcode (GET) | value |
|---|---|---|
| 0x00 onboard temp | `0x0400` | i16, centi-°C (TMP236) |
| 0x01 battery mV | `0x0401` | u16, mV |
| 0x02 battery SoC | `0x0402` | u8, percent |
| 0x03 BME280 temp | `0x0403` | i16, centi-°C |
| 0x04 BME280 pressure | `0x0404` | u32, Pa |
| 0x05 BME280 humidity | `0x0405` | u16, centi-%RH |
| 0x06 BME280 fresh | `0x0406` | u8, 0/1 (1 = last reading current) |

- **GET**: request payload none → response `[OK][value]`.
- **SUBSCRIBE** (`0x34xx`): request payload = `u32 interval_ms` LE, range
  50 … 3 600 000. Ack is status-only. Then periodic pushes follow **under
  the same opcode**, payload `[OK][issue_seq u8][page u8=0][value]`.
  Re-SUBSCRIBE to the same resource updates the interval in place.
- **UNSUBSCRIBE** (`0x44xx`): payload none. `NOT_SUBSCRIBED` if there was
  no active subscription on this transport.
- All subscriptions are per-transport and cleared on connect/disconnect.

---

## Topic groups (0x5) — GET, SUBSCRIBE, UNSUBSCRIBE

Fixed compile-time bundles of related values — subscribe once instead of
to many individual Measurements resources. SUBSCRIBE payload / push
framing / lifecycle are identical to Measurements (`u32 interval_ms` LE,
pushes `[OK][issue_seq][page=0][payload]` under the same opcode). GET
returns `[OK][payload]`. All fields little-endian.

### `0x5/0x00` — Environmental  → GET `0x0500`, SUBSCRIBE `0x3500` (14 B payload)

| off | type | field |
|---|---|---|
| 0 | i16 | BME280 temperature, centi-°C |
| 2 | u32 | BME280 pressure, Pa |
| 6 | u16 | BME280 humidity, centi-%RH |
| 8 | u8  | BME280 fresh (0/1) |
| 9 | i16 | onboard temperature, centi-°C (TMP236) |
| 11 | i16 | external temperature, centi-°C (LM35 — 0, not wired yet) |
| 13 | u8 | external temperature valid (0 until the LM35 driver lands) |

### `0x5/0x01` — Device status  → GET `0x0501`, SUBSCRIBE `0x3501` (18 B payload)

| off | type | field |
|---|---|---|
| 0 | u16 | battery mV |
| 2 | u8 | battery SoC, percent |
| 3 | u8 | battery state (0 NORMAL / 1 LOW / 2 CRITICAL / 3 CHARGING / 4 FULL) |
| 4 | u8 | USB connected |
| 5 | u8 | BLE connected |
| 6 | u8 | charging (TP4056 CHRG) |
| 7 | u8 | force-charge armed |
| 8 | u8 | 3V3 rail on |
| 9 | u8 | 5V rail on |
| 10 | u16 | RTC year |
| 12 | u8×5 | RTC month, day, hour, minute, second |
| 17 | u8 | RTC set (1 once ever set) |

---

## Settings (0x3) — GET, SET

Every `DeviceSettings` field, resource index = field order. All are `u16`
except `settling_threshold` (`i32`) and `settling_timeout_ms` (`u32`).
Payload is the raw little-endian field bytes.

| res | field | width | range |
|---|---|---|---|
| 0x00 | task_sensors_ms | u16 | 1…60000 |
| 0x01 | task_processing_ms | u16 | 1…60000 |
| 0x02 | task_display_ms | u16 | 1…60000 |
| 0x03 | task_ble_ms | u16 | 1…60000 |
| 0x04 | task_usb_ms | u16 | 1…60000 |
| 0x05 | task_battery_ms | u16 | 1…60000 |
| 0x06 | task_temperature_ms | u16 | 1…60000 |
| 0x07 | stream_interval_ms | u16 | 1…60000 |
| 0x08 | settling_threshold_umpm | i32 | 1…100000 |
| 0x09 | settling_timeout_ms | u32 | 1…60000 |
| 0x0A | filter_cutoff_hz_num | u16 | 1…10000 |
| 0x0B | filter_cutoff_hz_den | u16 | 1…10000 |
| 0x0C | battery_critical_mv | u16 | 2500…4200, must stay `< battery_low_mv` |
| 0x0D | battery_low_mv | u16 | 2500…4200, must stay `> battery_critical_mv` |
| 0x0E | battery_charge_start_mv | u16 | 2500…4200 |
| 0x0F | vbat_scale_num | u16 | 1…10000 |
| 0x10 | vbat_scale_den | u16 | 1…10000 |
| 0x11 | tmp236_seg1_voffs_mv | u16 | 0…3300 |
| 0x12 | tmp236_seg1_num | u16 | 1…10000 |
| 0x13 | tmp236_seg1_den | u16 | 1…10000 |
| 0x14 | tmp236_seg_boundary_mv | u16 | 0…3300 |
| 0x15 | tmp236_seg2_voffs_mv | u16 | 0…3300 |
| 0x16 | tmp236_seg2_num | u16 | 1…10000 |
| 0x17 | tmp236_seg2_den | u16 | 1…10000 |
| 0x18 | tmp236_seg2_tinfl_cdeg | u16 | 0…20000 |
| 0x19 | lm35_scale_mv_per_c | u16 | 1…1000 |
| 0x1A | encoder_counts_per_detent | u16 | 1…100 |
| 0x1B | auto_poweroff_s | u16 | 0…65535 (0 = disabled; idle seconds before Standby power-off) |

> `0x1B` is appended out of struct order — its `DeviceSettings` field sits
> mid-struct in the battery EEPROM page — so `0x00`–`0x1A` keep their wire
> indices.

- **GET** (`0x03xx`): payload none → `[OK][field bytes]`.
- **SET** (`0x13xx`): payload = field bytes (`BAD_LENGTH` if wrong width).
  On success the value is persisted to EEPROM and, for a `task_*_ms` field,
  the scheduler reloads its periods immediately. `BUSY_RESOURCE` if an
  EEPROM write is already in flight or the save failed;
  `INVALID_PARAMETER` if out of range or it would invert the
  critical/low battery pair.

---

## Debug messages (0x6) — SUBSCRIBE, UNSUBSCRIBE only

A live stream of the device's internal log. No GET.

### `SUBSCRIBE 0x6/0x00`  → opcode `0x3600`
Request payload: 1 byte — minimum severity to receive
(`0` INFO, `1` WARN, `2` ERROR). Ack is status-only. Then pushes follow
under the same opcode:

`[OK][issue_seq u8][page u8=0][severity u8][message bytes (≤ 48, no NUL)]`

Lines produced faster than the link drains them, or before the
subscription started, may be missed — this is a best-effort stream, not a
reliable log (spec §4.5's "no retransmission" tradeoff). `issue_seq` wraps
at 256; use modular comparison to spot a gap.

### `UNSUBSCRIBE 0x6/0x00`  → opcode `0x4600`
Payload none. `NOT_SUBSCRIBED` if not currently subscribed.
