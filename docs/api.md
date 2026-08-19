# Device API Reference

Implementer's reference for the host-facing command/notification protocol implemented in
`Services/svc_api.c`/`svc_api.h`. Describes the wire format as it exists in firmware today —
not a design proposal.

---

## 1. Philosophy

- **Transport-agnostic.** One packet format, one command/opcode set, one per-transport state
  machine (`ApiMode`) — implemented once in `svc_api.c` and reused verbatim by every
  transport. A transport's only job is to deliver bytes in and call `svc_api_send_fn`-style
  bytes out; `svc_api.c` never talks to hardware directly (`Services/svc_usb.c`,
  `Services/svc_ble.c`, `Services/svc_uart.c` are the transport adapters — see `CLAUDE.md`
  §8.1's layer model).
- **Each transport is an independent session.** USB, BLE, and UART each get their own
  connection state and mode (`ApiTransportState`, one per `ApiTransport`) — starting a stream
  on one transport does not affect another, and a command from one transport never affects
  another's state.
- **Host-driven.** The device never initiates a connection or a command; everything is either
  a direct command/response, or an unsolicited *notification* the device pushes once a host
  has opted in (`START_STREAM`, `START_RAW_STREAM`, `START_DISP_STREAM`) or triggered
  (`REQUEST_SINGLE`).
- **Fixed 64-byte packets.** Every packet — request or response, on every transport — is
  exactly `API_PACKET_MAX_SIZE` (64) bytes, matching the USB HID report size. Unused payload
  bytes are zero-padded.

---

## 2. Transports

| Transport | Enum | Framing | Notes |
|---|---|---|---|
| USB | `API_TRANSPORT_USB` | Whole 64-byte HID reports, already framed by the endpoint | `Services/svc_usb.c`. No TX queue — a busy IN endpoint drops the packet (counted in `usb_tx_dropped_count`). |
| BLE | `API_TRANSPORT_BLE` | Raw byte stream over RN4871 transparent UART, reassembled — see §3.2 | `Services/svc_ble.c`, 115200 baud 8N1 to the RN4871 module. |
| UART | `API_TRANSPORT_UART` | Raw byte stream over the wired debug UART, reassembled — see §3.2 | `Services/svc_uart.c`, 115200 baud 8N1 (USART3). |

A transport calls `svc_api_connected(t)` / `svc_api_disconnected(t)` on its own connect/
disconnect edge. Both reset that transport's mode to `API_MODE_IDLE` — reconnecting always
starts from idle, never resumes a prior stream.

---

## 3. Packet format

### 3.1 Layout

```
[CMD 1B][LEN 1B][PAYLOAD 0..60B][CRC16_LO 1B][CRC16_HI 1B]
```

- `CMD` — command, response, or notification opcode (§4–§5).
- `LEN` — payload length in bytes, `0..60` (`MAX_PAYLOAD = 64 − 2 − 2`).
- `PAYLOAD` — opcode-specific, little-endian (native STM32G0 byte order), `LEN` bytes.
- `CRC16` — `math_crc16()` over `CMD + LEN + PAYLOAD` (CRC-16/CCITT-FALSE: poly `0x1021`,
  init `0xFFFF`, no input/output reflection, no XOR-out), little-endian on the wire.
- Total packet size on the wire is always `4 + LEN`; USB pads the remainder of the 64-byte
  report with zeros, BLE/UART only send the `4 + LEN` bytes that actually exist (§3.2).

A malformed packet (short read, or CRC mismatch) gets `API_RSP_NACK` back — no partial
dispatch, no error detail beyond NACK.

### 3.2 Byte-stream transports (BLE, UART)

USB delivers whole reports pre-framed by the endpoint. BLE and UART deliver a raw byte
stream, so `svc_ble.c`/`svc_uart.c` reassemble packets themselves via
`svc_api_reassembler_feed_byte()`: bytes accumulate into an `ApiByteReassembler` buffer;
once 2 bytes are in hand the declared length is known, and once `4 + LEN` bytes have arrived
the packet is dispatched via `svc_api_receive()` and the reassembler resets. A partial packet
that stalls for more than `API_RX_PACKET_TIMEOUT_MS` (100 ms) is silently abandoned
(`svc_api_reassembler_check_timeout()`, polled by each transport's own scheduler task).

---

## 4. Per-transport mode / streaming model

Each transport carries one `ApiMode`, independent of the others:

| Mode | Entered by | Meaning |
|---|---|---|
| `API_MODE_IDLE` | Default; also forced on connect/disconnect | No unsolicited pushes active. |
| `API_MODE_STREAM` | `API_CMD_START_STREAM` | Periodic `STREAM_DATA` notifications, every `stream_interval_ms` (device setting, default 200 ms). |
| `API_MODE_RAW_STREAM` | `API_CMD_START_RAW_STREAM` | Periodic `RAW_STREAM_DATA` notifications, every `task_sensors_ms` (device setting, default 100 ms). |
| `API_MODE_DISP_STREAM` | `API_CMD_START_DISP_STREAM` | Batched `DISP_STREAM_DATA` notifications, drained every 1 ms scheduler tick, not interval-gated. USB-only (§5, §6.5). |

`API_MODE_SINGLE` exists in the `ApiMode` enum but is **never actually assigned** by the
current firmware — the single-shot measurement flow (§6.2) is driven entirely by
`Services/svc_measurement.c`'s own state machine, independent of a transport's `ApiMode`, and
pushes its progress/ready notifications to *every* connected transport regardless of which
one issued `REQUEST_SINGLE`. `svc_api_get_mode()` will never return `API_MODE_SINGLE`.

`STREAM` and `RAW_STREAM` can't both be active on the same transport at once (starting one
doesn't stop the other automatically — starting `RAW_STREAM` while in `STREAM` mode simply
overwrites the mode field, silently ending the first). `DISP_STREAM` shares the same single
`mode` field, so the same applies there.

---

## 5. Command reference

All commands are sent as `CMD` with the request payload (if any); the device always responds
with either the listed response/`ACK`, or `NACK` (`API_RSP_NACK`, no payload) on any
validation failure. Where "→ `ACK`/`NACK`" is listed, there is no dedicated response opcode —
only the generic ack/nack.

| CMD | Value | Request payload | Response | Semantics |
|---|---|---|---|---|
| `GET_STATUS` | `0x01` | — | `RSP_GET_STATUS` (§7.1) | One-shot device status snapshot. |
| `REQUEST_SINGLE` | `0x02` | — | → `ACK` | Triggers `Services/svc_measurement.c`'s single-shot capture (settle → capture → `SINGLE_READY` notification, §6.2). |
| `CANCEL_SINGLE` | `0x03` | — | → `ACK` | Cancels an in-progress single-shot capture. `ACK` unconditionally, even if nothing was in progress. |
| `START_STREAM` | `0x04` | — | → `ACK` | Enters `API_MODE_STREAM` on the requesting transport (§4, §6.3). |
| `STOP_STREAM` | `0x05` | — | → `ACK` | Returns to `API_MODE_IDLE` if currently in `STREAM` mode; `ACK` either way. |
| `START_RAW_STREAM` | `0x06` | — | → `ACK` | Enters `API_MODE_RAW_STREAM` (§4, §6.4). |
| `STOP_RAW_STREAM` | `0x07` | — | → `ACK` | Returns to `API_MODE_IDLE` if currently in `RAW_STREAM` mode; `ACK` either way. |
| `SET_ZERO` | `0x08` | — | → `ACK`/`NACK` | Stub — accepts the command and re-saves the current `CalibrationData` to EEPROM; does not yet capture an actual zero offset (no live sensor fusion into `g_system_state` yet). `NACK` if EEPROM is busy with another save. |
| `GET_CALIBRATION` | `0x09` | — | `RSP_GET_CALIBRATION` (§7.6) | Reads back the legacy `CalibrationData` blob (§7.6 — references REV-A sensors, see caveats §8). |
| `SET_CALIBRATION` | `0x0A` | `CalibrationData`, 28 bytes | → `ACK`/`NACK` | Overwrites `CalibrationData` and persists to EEPROM. `NACK` if `LEN != 28` or EEPROM busy. Recomputes `calibration_valid` immediately (visible in the next `GET_STATUS`). |
| `GET_SETTINGS` | `0x0B` | — | `RSP_GET_SETTINGS` (§7.7) | Reads back the full `DeviceSettings` blob. |
| `SET_SETTINGS` | `0x0C` | `DeviceSettings`, 60 bytes | → `ACK`/`NACK` | Overwrites `DeviceSettings`, re-validates (clamps/repairs any zero/invalid divisor-type fields the same way boot-time EEPROM load does), persists to EEPROM, and reloads scheduler task periods immediately. `NACK` if `LEN != 60` or EEPROM busy. |
| `GET_IDENTITY` | `0x0D` | — | `RSP_GET_IDENTITY` (§7.8) | Firmware version + product/serial strings — static, doesn't change at runtime. |
| `START_DISP_STREAM` | `0x0E` | — | → `ACK`/`NACK` | Enters `API_MODE_DISP_STREAM` (§4, §6.5). **NACKs on any transport other than USB** — BLE/UART can't sustain the data rate (§8). |
| `STOP_DISP_STREAM` | `0x0F` | — | → `ACK` | Returns to `API_MODE_IDLE` if currently in `DISP_STREAM` mode; `ACK` either way. |
| *(unrecognized opcode)* | — | — | → `NACK` | `dispatch()`'s default case. |

`SET_CALIBRATION`/`SET_SETTINGS` practical usage pattern: `GET_*` first, modify only the
fields you understand in place, `SET_*` the whole blob back — both structs are sent as a raw
compiler-layout memory image (not a hand-packed wire format), so round-tripping the exact
bytes you received is the reliable way to avoid corrupting fields your implementation doesn't
know about yet.

---

## 6. Notifications (unsolicited)

Pushed by the device without a corresponding request, once a host has opted in or triggered
the underlying action.

| NOTIFY | Value | Sent when | Payload |
|---|---|---|---|
| `SINGLE_READY` | `0xF0` | A single-shot capture (`REQUEST_SINGLE`) completes | `ApiSinglePayload` (§7.5) |
| `SINGLE_PROGRESS` | `0xF1` | Every 500 ms while a single-shot capture is settling/capturing | 1 byte: progress percent (`0..100`) |
| `STREAM_DATA` | `0xF2` | Every `stream_interval_ms` while a transport is in `STREAM` mode | `ApiStreamPayload` (§7.2) |
| `RAW_STREAM_DATA` | `0xF3` | Every `task_sensors_ms` while a transport is in `RAW_STREAM` mode | `ApiRawStreamPayload` (§7.3) |
| `STATUS_CHANGED` | `0xF4` | **Opcode reserved, not currently emitted** — defined in `svc_api.h` but no code path sends it yet. | — |
| `DISP_STREAM_DATA` | `0xF5` | Every scheduler tick that has ≥1 pending displacement cycle, while USB is in `DISP_STREAM` mode | `ApiDispStreamPayload` (§7.9, variable length) |

`SINGLE_READY`/`SINGLE_PROGRESS` go to **every connected transport** unconditionally (not
gated by any transport's `ApiMode`) — see §4's note on `API_MODE_SINGLE` being unused.

### 6.1 Status snapshot (`GET_STATUS`)
One-shot pull — no push variant beyond the still-unused `STATUS_CHANGED` opcode. Poll it.

### 6.2 Single-shot measurement
`REQUEST_SINGLE` → device settles then captures (state machine in `svc_measurement.c`) →
`SINGLE_PROGRESS` pushed every 500 ms during settle/capture → `SINGLE_READY` pushed once on
completion, to every connected transport. `CANCEL_SINGLE` aborts it early; no notification is
sent on cancel.

### 6.3 `STREAM` — fused/derived tilt, low rate
Periodic snapshot of processed tilt + status, at `stream_interval_ms` (default 200 ms) — the
"normal operating" telemetry feed.

### 6.4 `RAW_STREAM` — legacy raw sensor snapshot
Periodic snapshot including raw PCAP04/SCL3300 readings, at `task_sensors_ms` (default
100 ms). **See §8 — the sensors this payload describes do not exist on REV B hardware.**

### 6.5 `DISP_STREAM` — WP10 displacement cycle stream
Batches up to 3 `Services/svc_displacement.c` cycles (produced at ~2604.17 Hz) per report
into `ApiDispRecord` entries (§7.9), USB-only, drained every scheduler tick rather than on a
settings-driven interval — the production rate is far too fast for the other streams'
100–200 ms polling. See §8 for the `seq` field's wraparound contract and the unconfirmed
USB `bInterval` assumption behind its throughput margin.

---

## 7. Payload layouts

All structs below are `__attribute__((packed))` unless noted — byte offsets are exact, no
padding. Multi-byte fields are little-endian (native STM32G0 order).

### 7.1 `ApiStatusPayload` — `RSP_GET_STATUS` (13 bytes)

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 0 | `battery_soc_pct` | `uint8` | State of charge, 0–100% |
| 1 | `battery_mv` | `uint16` | Battery voltage, mV |
| 3 | `battery_state` | `uint8` | `0`=normal, `2`=critical, `3`=charging (`1` unused/reserved) |
| 4 | `ble_connected` | `uint8` | 0/1 |
| 5 | `usb_connected` | `uint8` | 0/1 |
| 6 | `sensor_scl3300_ok` | `uint8` | 0/1 — see §8, legacy REV-A sensor |
| 7 | `sensor_pcap04_1_ok` | `uint8` | 0/1 — see §8, legacy REV-A sensor |
| 8 | `sensor_pcap04_2_ok` | `uint8` | 0/1 — see §8, legacy REV-A sensor |
| 9 | `calibration_valid` | `uint8` | 0/1 — `CalibrationData.scale_valid && zero_valid` |
| 10 | `fw_major` | `uint8` | |
| 11 | `fw_minor` | `uint8` | |
| 12 | `fw_patch` | `uint8` | |

### 7.2 `ApiStreamPayload` — `STREAM_DATA` (20 bytes)

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 0 | `tilt_pcap04_umpm` | `int32` | Tilt, µm/m — see §8 |
| 4 | `tilt_scl3300_x_umpm` | `int32` | Tilt, µm/m — see §8 |
| 8 | `tilt_scl3300_y_umpm` | `int32` | Tilt, µm/m — see §8 |
| 12 | `temperature_cdeg` | `int16` | Centidegrees C |
| 14 | `battery_soc_pct` | `uint8` | 0–100% |
| 15 | `status_flags` | `uint8` | Bit0=SCL3300 ok, Bit1=PCAP04#1 ok, Bit2=PCAP04#2 ok |
| 16 | `timestamp_ms` | `uint32` | `hal_systick_get_ms()` at fill time |

### 7.3 `ApiRawStreamPayload` — `RAW_STREAM_DATA` (34 bytes)

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 0 | `pcap04_1_af` | `int32` | Raw PCAP04 #1 reading, attofarads — see §8 |
| 4 | `pcap04_2_af` | `int32` | Raw PCAP04 #2 reading, attofarads — see §8 |
| 8 | `pcap04_diff_af` | `int32` | `pcap04_1_af − pcap04_2_af` |
| 12 | `scl3300_x_cdeg` | `int16` | Raw SCL3300 X, centidegrees — see §8 |
| 14 | `scl3300_y_cdeg` | `int16` | Raw SCL3300 Y, centidegrees — see §8 |
| 16 | `scl3300_z_cdeg` | `int16` | Raw SCL3300 Z, centidegrees — see §8 |
| 18 | `tilt_pcap04_umpm` | `int32` | Derived tilt, µm/m |
| 22 | `tilt_scl3300_x_umpm` | `int32` | Derived tilt, µm/m |
| 26 | `temperature_cdeg` | `int16` | Centidegrees C |
| 28 | `battery_soc_pct` | `uint8` | 0–100% |
| 29 | `status_flags` | `uint8` | Same bit layout as §7.2 |
| 30 | `timestamp_ms` | `uint32` | |

### 7.4 `ApiIdentityPayload` — `RSP_GET_IDENTITY` (27 bytes)

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 0 | `fw_major` | `uint8` | |
| 1 | `fw_minor` | `uint8` | |
| 2 | `fw_patch` | `uint8` | |
| 3 | `product_str` | `char[16]` | Zero-padded, **not NUL-terminated if it exactly fills 16 bytes** — parse by fixed length, not `strlen` |
| 19 | `serial_str` | `char[8]` | Same convention as `product_str` |

### 7.5 `ApiSinglePayload` — `SINGLE_READY` (22 bytes)

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 0 | `tilt_pcap04_umpm` | `int32` | µm/m |
| 4 | `tilt_scl3300_x_umpm` | `int32` | µm/m |
| 8 | `tilt_scl3300_y_umpm` | `int32` | µm/m |
| 12 | `temperature_cdeg` | `int16` | Centidegrees C |
| 14 | `battery_soc_pct` | `uint8` | 0–100% |
| 15 | `status_flags` | `uint8` | Same bit layout as §7.2 |
| 16 | `timestamp_ms` | `uint32` | |
| 20 | `sample_count` | `uint16` | Samples averaged during capture |

### 7.6 `CalibrationData` — `GET_CALIBRATION` request / `SET_CALIBRATION` response (28 bytes)

**Not `__attribute__((packed))`** — sent as the raw in-memory struct (natural ARM EABI
alignment/padding), verified 28 bytes with the actual `arm-none-eabi-gcc`. Field order (no
offset table given — see §5's round-trip guidance):

`pcap04_scale_af_per_umpm` (`int32`), `scl3300_scale_cdeg_per_umpm` (`int32`),
`pcap04_zero_af` (`int32`), `scl3300_x_zero_cdeg`/`_y_zero_cdeg`/`_z_zero_cdeg` (`int16`
each), `calibration_timestamp` (`uint32`), `scale_valid`/`zero_valid` (`bool`, 1 byte each).
All fields describe REV-A sensors — see §8.

### 7.7 `DeviceSettings` — `GET_SETTINGS` request / `SET_SETTINGS` response (60 bytes)

**Not packed** — same round-trip caveat as §7.6. Exactly 60 bytes, i.e. exactly
`MAX_PAYLOAD` — **zero header margin**; this struct cannot grow without either shrinking a
field elsewhere or changing the packet framing. Field groups (`system_state.h`'s own comment
documents the EEPROM-page grouping; each group must stay field-contiguous):

- **Scheduler/timing**: `task_sensors_ms`, `task_processing_ms`, `task_display_ms`,
  `task_ble_ms`, `task_usb_ms`, `task_battery_ms`, `task_temperature_ms`,
  `stream_interval_ms` (all `uint16`), `settling_threshold_umpm` (`int32`),
  `settling_timeout_ms` (`uint32`), `filter_cutoff_hz_num`/`_den` (`uint16` each)
- **Battery**: `battery_critical_mv`, `battery_low_mv`, `vbat_scale_num`, `vbat_scale_den`
  (all `uint16`)
- **TMP236** (on-board temp sensor): `tmp236_seg1_voffs_mv`, `_seg1_num`, `_seg1_den`,
  `_seg_boundary_mv`, `_seg2_voffs_mv`, `_seg2_num`, `_seg2_den`, `_seg2_tinfl_cdeg` (all
  `uint16`)
- **LM35** (external temp sensor): `lm35_scale_mv_per_c` (`uint16`)
- **Encoder**: `encoder_counts_per_detent` (`uint16`)
- **BLE**: `ble_configured` (`bool`) — clearing this forces `drv_rn4871.c` to re-run full
  RN4871 configuration on next boot
- `_settings_end_marker` (`uint8`) — internal boundary marker, not a real setting; not
  meaningful to a host, harmless to round-trip verbatim

Note: `task_uart_ms` is **not** in this struct (no EEPROM headroom left) — the wired UART
poll period is a firmware-internal fixed constant, not host-configurable.

### 7.8 `ApiIdentityPayload` — see §7.4 (listed once; cross-referenced from §5's table)

### 7.9 `ApiDispRecord` / `ApiDispStreamPayload` — `DISP_STREAM_DATA` (variable, ≤56 bytes)

One `ApiDispRecord` (18 bytes, packed):

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 0 | `seq` | `uint16` | Rolling per-cycle sequence number — see §8 for the wraparound contract |
| 2 | `delta1_mm` | `float` | Sensor 1 displacement, mm |
| 6 | `residual1` | `float` | Sensor 1 complex-division residual (diagnostic, ~0 nominally) |
| 10 | `delta2_mm` | `float` | Sensor 2 displacement, mm |
| 14 | `residual2` | `float` | Sensor 2 residual |

`ApiDispStreamPayload` wraps 1–3 records:

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 0 | `count` | `uint8` | Number of valid records that follow, `1..3` |
| 1 | `reserved` | `uint8` | Unused, always `0` on the wire today |
| 2 | `records[count]` | `ApiDispRecord[]` | `count × 18` bytes; the packet's `LEN` field is `2 + count*18`, **not** always the maximum 56 — a report with fewer than 3 pending cycles is shorter, not zero-padded to 56 |

---

## 8. Accuracy notes / known caveats

- **PCAP04/SCL3300 fields are legacy.** REV B hardware does **not** carry a PCAP04 or
  SCL3300 (see `CLAUDE.md`'s WP3 notes) — every field referencing them (`ApiStatusPayload`'s
  `sensor_pcap04_*_ok`/`sensor_scl3300_ok`, `ApiStreamPayload`/`ApiRawStreamPayload`/
  `ApiSinglePayload`'s `tilt_pcap04_umpm`/`tilt_scl3300_*_umpm`, `ApiRawStreamPayload`'s raw
  PCAP04/SCL3300 readings, and all of `CalibrationData`) will read as stale/zero on current
  hardware. This is a REV-A-era protocol surface nobody has cleaned up yet, not a bug in any
  single command.
- **`DISP_STREAM`'s `seq` wraps every ~25 seconds** (`uint16`, ~2604.17 cycles/sec
  production). A host doing gap detection **must** use wraparound-safe (modular) comparison
  — a naive `seq != prev + 1` check produces a false "gap" at every rollover even when
  nothing was lost.
- **`DISP_STREAM`'s ~15% throughput margin assumes a 1 ms USB HID `bInterval`**, which is
  the common CubeMX default but has not been directly confirmed against this device's actual
  USB descriptor. If the real polling interval is longer, the achievable drain rate could
  fall below the ~2604.17 Hz production rate, and the output ring would evict continuously —
  visible only as `seq` gaps, since drop counters (`svc_displacement_get_output_drop_count()`
  etc.) aren't currently exposed through this API.
- **No calibration write path for the WP10 displacement sensors.** `DisplacementSensorCal`/
  `DisplacementSharedCal` (S1/S2 gain, d0, zero-offset, shared attenuation) are only
  loadable/saveable from within `Services/svc_storage.c` at boot — there is no
  `SET_DISPLACEMENT_CAL`-style command yet.
- **Nothing here has been validated against real hardware.** Same standing caveat as every
  work package in this project — protocol behavior is build-verified against the firmware
  source, not observed over an actual USB/BLE/UART link with a real host implementation.
