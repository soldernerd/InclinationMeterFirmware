# Device API v2 Reference

Full, standalone contract for writing host software against this device's binary
command/response protocol (USB HID, BLE, or wired UART). No firmware source-reading required.
Supersedes `docs/api.md` (the v1 reference).

> **Status: in progress.** This document is built out incrementally, one category at a time,
> alongside firmware implementation (WP11, per `docs/api-v2-spec.md` §8) — it is not written
> once at the end. **Currently covered: System status (§4), Commands (§5), Calibrations (§6),
> Settings (§7), Measurements (§8).** Topic groups, Debug messages, Raw data, and Bulk
> transfers are designed (see `docs/api-v2-spec.md`) but not yet implemented in firmware, and
> have no resources documented here yet — sending any opcode in those categories today gets
> `UNKNOWN_CATEGORY` (§3). Per a 2026-08-19 project rule: every individual measurement result,
> device setting, and calibration constant is exposed as its own resource "out of the box";
> bigger constructs (Topic groups, Bulk transfers, and WP10's high-rate displacement-cycle
> stream, which would live under Raw data) are deliberately deferred until an actual need for
> them exists, rather than built speculatively now.

---

## 1. Packet format

```
[OPCODE 2B][LEN 2B][PAYLOAD 0..LEN B][CRC16 2B]
```

- **OPCODE** — 16 bits, little-endian. Structure in §2.
- **LEN** — payload length in bytes, little-endian. No fixed maximum in the protocol itself;
  see §9's transport notes for the current practical ceiling.
- **PAYLOAD** — opcode-specific, little-endian. For every response, byte 0 of PAYLOAD is
  always a status code (§3); resource-specific data, if any, follows starting at byte 1.
- **CRC16** — CRC-16/CCITT-FALSE (poly `0x1021`, init `0xFFFF`, no input/output reflection,
  no XOR-out), computed over `OPCODE + LEN + PAYLOAD` (not including the CRC bytes
  themselves), little-endian on the wire.
- No padding. Total packet size on the wire is always `6 + LEN`.
- A response always echoes the request's OPCODE unchanged.

On any transport, a request that fails CRC gets a `BAD_CRC` response (§3) — the device still
echoes back the opcode it parsed (untrustworthy if the opcode bytes themselves were
corrupted, but harmless to echo). A request too short or truncated to even parse a length
field gets no response at all (nothing to safely echo).

---

## 2. Opcode structure

16 bits: `[VERB: 4 bits][CATEGORY: 4 bits][RESOURCE INDEX: 8 bits]`, VERB in the
most-significant nibble.

```
opcode = (verb << 12) | (category << 8) | resource_index
```

### Verbs

| Verb | Value | Meaning |
|---|---|---|
| `GET` | `0x0` | One-shot read |
| `SET` | `0x1` | Persist a value |
| `EXECUTE` | `0x2` | Fire a one-shot action, no stored value |
| `SUBSCRIBE` | `0x3` | Begin periodic/event-driven push delivery |
| `UNSUBSCRIBE` | `0x4` | End a subscription |
| `START_BULK` | `0x5` | Begin a bulk transfer |
| `CANCEL_BULK` | `0x6` | Abort an in-progress bulk transfer |

`0x7`–`0xF` reserved.

### Categories

| Category | Value | Status in this firmware |
|---|---|---|
| System status | `0x0` | **Implemented** (§4) |
| Commands | `0x1` | **Implemented** (§5) |
| Calibrations | `0x2` | **Implemented** (§6) |
| Settings | `0x3` | **Implemented** (§7) |
| Measurements | `0x4` | **Implemented** (§8) |
| Topic groups | `0x5` | Not yet implemented |
| Debug messages | `0x6` | Not yet implemented |
| Raw data | `0x7` | Not yet implemented |
| Bulk transfers | `0x8` | Not yet implemented |

`0x9`–`0xF` reserved.

---

## 3. Status codes

Every response's payload byte 0 is one of these. `OK` means the rest of the payload (if any)
is valid resource data; any other code means the payload is exactly 1 byte (the status code
alone, no resource data).

| Code | Name | Meaning |
|---|---|---|
| `0x00` | `OK` | Success |
| `0x01` | `UNKNOWN_CATEGORY` | Opcode's category field doesn't match a category this firmware build implements |
| `0x02` | `VERB_NOT_VALID` | Verb not valid for this category |
| `0x03` | `UNKNOWN_RESOURCE` | Category valid, resource index not defined within it |
| `0x04` | `BAD_CRC` | Request failed CRC check |
| `0x05` | `BAD_LENGTH` | Payload length doesn't match what the resource expects |
| `0x06` | `BUSY_RESOURCE` | Resource-specific busy condition |
| `0x07` | `BUSY_EXCLUSIVE` | Conflicts with another exclusive-group operation (not yet reachable — no category implements exclusivity-group resources yet) |
| `0x08` | `INVALID_PARAMETER` | Payload parses, but a field's value is out of range |
| `0x09` | `NOT_SUBSCRIBED` | `UNSUBSCRIBE` with no active subscription (not yet reachable — no category implements `SUBSCRIBE` yet) |
| `0x0A` | `NOTHING_TO_CANCEL` | Cancel-type operation with nothing to cancel (not yet reachable — no category implements a cancel verb yet) |

A device firmware build only ever reports the codes each implemented resource can actually
produce — see each resource's own status-code list below for the specific subset relevant to
it, rather than assuming any code in this table applies everywhere.

---

## 4. System status (category `0x0`)

Read-only device/firmware information. `GET` only — not `SET`-able, not subscribable.
Never blocks, no concurrency/exclusivity rules apply (unlimited concurrent requests, any
transport, any time).

### 4.1 `GET` Identity — opcode `0x0000`

`verb=GET(0x0), category=SYSTEM_STATUS(0x0), resource=0x00` → **`0x0000`**

**Request:** no payload (`LEN=0`).

**Response payload** (27 bytes after the status byte, 28 bytes total):

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 0 | `fw_major` | `uint8` | Firmware major version |
| 1 | `fw_minor` | `uint8` | Firmware minor version |
| 2 | `fw_patch` | `uint8` | Firmware patch version |
| 3 | `product_str` | `char[16]` | Zero-padded, **not NUL-terminated** if it exactly fills 16 bytes — parse by fixed length, not `strlen` |
| 19 | `serial_str` | `char[8]` | Same fixed-width convention as `product_str` |

**Status codes:** `OK` (always, this resource cannot fail once dispatched) · `BAD_CRC` ·
`BAD_LENGTH` (any nonzero `LEN` in the request).

**Worked example** (firmware v1.0.0, product string `"InclinoMeter"`, serial `"SN00001"`):

Request:
```
00 00 00 00 C0 84
```
`OPCODE=0x0000, LEN=0, CRC=0x84C0`

Response:
```
00 00 1C 00 00 01 00 00 49 6E 63 6C 69 6E 6F 4D 65 74 65 72
00 00 00 00 53 4E 30 30 30 30 31 00 59 09
```
`OPCODE=0x0000 (echoed), LEN=0x001C=28, status=0x00 (OK), fw=1.0.0, product_str="InclinoMeter\0\0\0\0", serial_str="SN00001\0", CRC=0x0959`

### 4.2 `GET` Device state — opcode `0x0001`

`verb=GET(0x0), category=SYSTEM_STATUS(0x0), resource=0x01` → **`0x0001`**

**Request:** no payload (`LEN=0`).

**Response payload** (7 bytes after the status byte, 8 bytes total):

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 0 | `battery_state` | `uint8` | `0`=normal, `1`=low, `2`=critical, `3`=charging, `4`=full |
| 1 | `battery_soc_pct` | `uint8` | State of charge, 0–100% |
| 2 | `battery_mv` | `uint16` | Battery voltage, mV |
| 4 | `usb_connected` | `uint8` | 0/1 |
| 5 | `ble_connected` | `uint8` | 0/1 |
| 6 | `calibration_valid` | `uint8` | 0/1 |

**Status codes:** `OK` (always) · `BAD_CRC` · `BAD_LENGTH` (any nonzero `LEN` in the request).

**Worked example** (battery normal, 85% SoC, 4000 mV, USB connected, BLE not connected,
calibration valid):

Request:
```
01 00 00 00 74 F2
```
`OPCODE=0x0001, LEN=0, CRC=0xF274`

Response:
```
01 00 08 00 00 00 55 A0 0F 01 00 01 C9 27
```
`OPCODE=0x0001 (echoed), LEN=8, status=0x00 (OK), battery_state=0 (normal), soc=0x55=85%, battery_mv=0x0FA0=4000, usb_connected=1, ble_connected=0, calibration_valid=1, CRC=0x27C9`

---

## 5. Commands (category `0x1`)

Fire-once actions with no stored value. `EXECUTE` only — not `GET`-able, not subscribable.
Never blocks, no concurrency/exclusivity rules apply.

### 5.1 `EXECUTE` Test beep — opcode `0x2100`

`verb=EXECUTE(0x2), category=COMMANDS(0x1), resource=0x00` → **`0x2100`**

Sounds the onboard buzzer once (~100 ms, 2 kHz), for verifying host↔device connectivity and
command dispatch without touching sensor/calibration/settings state. No other side effect.

**Request:** no payload (`LEN=0`).

**Response payload:** 1 byte (status only) — no resource data on success.

**Status codes:** `OK` (buzzer sounded) · `BAD_CRC` · `BAD_LENGTH` (any nonzero `LEN` in the
request).

**Worked example:**

Request:
```
00 21 00 00 36 35
```
`OPCODE=0x2100, LEN=0, CRC=0x3536`

Response:
```
00 21 01 00 00 C6 67
```
`OPCODE=0x2100 (echoed), LEN=1, status=0x00 (OK), CRC=0x67C6`

---

## 6. Calibrations (category `0x2`)

WP10 displacement-sensor calibration constants. `GET`/`SET`, not subscribable. Never blocks
beyond the normal EEPROM-write busy window, no exclusivity rules apply. **Legacy REV-A
calibration fields (PCAP04/SCL3300 scale/zero constants) are deliberately not exposed** — REV
B hardware doesn't carry those sensors; see `docs/api.md` §8 (the superseded v1 reference) for
where this was flagged, and this document's status note for the "add back if/when the sensors
come back" decision.

Every resource shares the same shape:

- **`GET`** — request: no payload (`LEN=0`). Response: status byte + a 4-byte little-endian
  `float`, IEEE 754. Status codes: `OK` · `BAD_CRC` · `BAD_LENGTH` (nonzero `LEN`).
- **`SET`** — request: a 4-byte little-endian `float`. Response: status byte only. Status
  codes: `OK` (queued for EEPROM write — see the note below) · `BAD_CRC` ·
  `BAD_LENGTH` (`LEN != 4`) · `BUSY_RESOURCE` (another EEPROM write already in progress, or —
  rarely — this one failed to queue; retry shortly either way) · `INVALID_PARAMETER` (value
  outside the resource's valid range, §6.1–§6.7 below).

**`SET`'s `OK` means "accepted and queued", not "durably written."** EEPROM writes in this
firmware are non-blocking and drained over subsequent scheduler ticks; a write that fails
*after* being queued (rare — exhausted retries) is only observable via a later `GET` still
returning the old value, or the device's local UI showing a save-failed indicator. There is no
push notification for this today (Calibrations isn't subscribable) — a host that needs
certainty should `GET` back the value it just `SET` after a short delay.

### 6.1 Sensor 1 gain — opcode `0x0200` / `0x1200`

`resource=0x00`. `k = atten × gain` (§6.7's `atten` is shared) is a divisor in the
displacement math (`Services/svc_displacement.c`); must be nonzero.

| Field | Type | Valid range | Default |
|---|---|---|---|
| `gain` | `float` | `0.1` – `1000.0` | `10.0` |

### 6.2 Sensor 1 neutral gap (d0) — opcode `0x0201` / `0x1201`

`resource=0x01`.

| Field | Type | Valid range | Default |
|---|---|---|---|
| `d0_mm` | `float` | `0.001` – `10.0` | `0.1` |

### 6.3 Sensor 1 zero offset — opcode `0x0202` / `0x1202`

`resource=0x02`.

| Field | Type | Valid range | Default |
|---|---|---|---|
| `zero_offset_mm` | `float` | `-10.0` – `10.0` | `0.0` |

### 6.4 Sensor 2 gain — opcode `0x0203` / `0x1203`

`resource=0x03`. Same shape as §6.1, sensor 2.

### 6.5 Sensor 2 neutral gap (d0) — opcode `0x0204` / `0x1204`

`resource=0x04`. Same shape as §6.2, sensor 2.

### 6.6 Sensor 2 zero offset — opcode `0x0205` / `0x1205`

`resource=0x05`. Same shape as §6.3, sensor 2.

### 6.7 Shared attenuation — opcode `0x0206` / `0x1206`

`resource=0x06`. Shared across both sensors (not per-sensor) — see `Services/
svc_displacement.c` for how it combines with each sensor's own `gain`.

| Field | Type | Valid range | Default |
|---|---|---|---|
| `atten` | `float` | `0.1` – `100.0` | `3.0` |

### 6.8 Worked example — get, set, and a rejected value

Get sensor 1's gain:
```
Request:  00 02 00 00 A0 EA                    (OPCODE=0x0200, LEN=0, CRC=0xEAA0)
Response: 00 02 04 00 00 00 00 20 41 11 08      (status=0x00 OK, gain=10.0, CRC=0x0811)
```

Attempt to set it to exactly `0.0` (below the `0.1` minimum — would make `k = atten × gain`
zero, a divide-by-zero in the displacement math):
```
Request:  00 12 04 00 00 00 00 00 07 60         (OPCODE=0x1200, LEN=4, value=0.0, CRC=0x6007)
Response: 00 12 01 00 08 FB 51                  (status=0x08 INVALID_PARAMETER, CRC=0x51FB)
```

---

## 7. Settings (category `0x3`)

Every `DeviceSettings` field (scheduler periods, battery thresholds, and the calibration
constants each on-board sensor's own driver uses to convert its raw reading — TMP236, LM35,
the battery-voltage divider). `GET`/`SET`, not subscribable, same non-blocking/busy/`OK`-means-
queued behavior as Calibrations (§6) — see that section's note, it applies here identically.

Shared behavior:

- **`GET`** — request: no payload. Response: status byte + the field's raw value, little-endian,
  width per the table below (1, 2, or 4 bytes). Status codes: `OK` · `BAD_CRC` ·
  `BAD_LENGTH`.
- **`SET`** — request: the field's value, little-endian, same width as its `GET` response.
  Response: status byte only. Status codes: `OK` · `BAD_CRC` · `BAD_LENGTH` (wrong width) ·
  `BUSY_RESOURCE` · `INVALID_PARAMETER` (outside the field's valid range — every field below
  has one; none accept an arbitrary value of the right width).

**Two fields currently have no effect on device behavior**: `task_processing_ms` and the
`filter_cutoff_hz_num`/`filter_cutoff_hz_den` pair are real, persisted, individually
`GET`/`SET`-able settings with no firmware consumer yet (a complementary filter that would use
the cutoff pair is still future work). `SET` on them succeeds normally; it just doesn't change
any observable behavior today.

**`battery_critical_mv` and `battery_low_mv` have a cross-field constraint `SET` enforces**:
`critical` must stay strictly less than `low` (`Services/svc_battery.c` relies on this
ordering to classify battery state correctly). A `SET` that would invert the pair — even
though the new value is within that field's own `[min,max]` — is rejected with
`INVALID_PARAMETER`, checked against whichever value is *currently stored* for the other
field. No other field in this category has a cross-field constraint.

| # | Resource | Field | Type | Valid range | Default | Notes |
|---|---|---|---|---|---|---|
| `0x00` | Sensor task period | `task_sensors_ms` | `uint16` (ms) | 1–60000 | 100 | |
| `0x01` | Processing task period | `task_processing_ms` | `uint16` (ms) | 1–60000 | — | No consumer yet |
| `0x02` | Display task period | `task_display_ms` | `uint16` (ms) | 1–60000 | — | |
| `0x03` | BLE task period | `task_ble_ms` | `uint16` (ms) | 1–60000 | — | |
| `0x04` | USB task period | `task_usb_ms` | `uint16` (ms) | 1–60000 | — | |
| `0x05` | Battery task period | `task_battery_ms` | `uint16` (ms) | 1–60000 | — | |
| `0x06` | Temperature task period | `task_temperature_ms` | `uint16` (ms) | 1–60000 | — | |
| `0x07` | Stream interval (local UI) | `stream_interval_ms` | `uint16` (ms) | 1–60000 | 200 | No API-layer consumer (v1's STREAM_DATA is gone); still read/written by the device's own SETTINGS screen |
| `0x08` | Settling threshold | `settling_threshold_umpm` | `int32` (µm/m) | 1–100000 | 10 | |
| `0x09` | Settling timeout | `settling_timeout_ms` | `uint32` (ms) | 1–60000 | — | |
| `0x0A` | Filter cutoff numerator | `filter_cutoff_hz_num` | `uint16` | 1–10000 | — | No consumer yet |
| `0x0B` | Filter cutoff denominator | `filter_cutoff_hz_den` | `uint16` | 1–10000 | — | No consumer yet |
| `0x0C` | Battery critical threshold | `battery_critical_mv` | `uint16` (mV) | 2500–4200, and `< battery_low_mv` | 3650 | Cross-field constraint, see above |
| `0x0D` | Battery low threshold | `battery_low_mv` | `uint16` (mV) | 2500–4200, and `> battery_critical_mv` | 3800 | Cross-field constraint, see above |
| `0x0E` | VBAT scale numerator | `vbat_scale_num` | `uint16` | 1–10000 | — | |
| `0x0F` | VBAT scale denominator | `vbat_scale_den` | `uint16` | 1–10000 | — | |
| `0x10` | TMP236 seg1 voltage offset | `tmp236_seg1_voffs_mv` | `uint16` (mV) | 0–3300 | 400 | |
| `0x11` | TMP236 seg1 slope numerator | `tmp236_seg1_num` | `uint16` | 1–10000 | 200 | |
| `0x12` | TMP236 seg1 slope denominator | `tmp236_seg1_den` | `uint16` | 1–10000 | 39 | |
| `0x13` | TMP236 segment boundary | `tmp236_seg_boundary_mv` | `uint16` (mV) | 0–3300 | 2350 | |
| `0x14` | TMP236 seg2 voltage offset | `tmp236_seg2_voffs_mv` | `uint16` (mV) | 0–3300 | 2350 | |
| `0x15` | TMP236 seg2 slope numerator | `tmp236_seg2_num` | `uint16` | 1–10000 | 1000 | |
| `0x16` | TMP236 seg2 slope denominator | `tmp236_seg2_den` | `uint16` | 1–10000 | 197 | |
| `0x17` | TMP236 seg2 inflection point | `tmp236_seg2_tinfl_cdeg` | `uint16` (0.01°C) | 0–20000 | 10000 | |
| `0x18` | LM35 scale factor | `lm35_scale_mv_per_c` | `uint16` (mV/°C) | 1–1000 | 10 | |
| `0x19` | Encoder counts per detent | `encoder_counts_per_detent` | `uint16` | 1–100 | 4 | |
| `0x1A` | BLE configured flag | `ble_configured` | `uint8` (bool) | 0–1 | 0 | |

Opcode = `0x0300 | resource` for `GET`, `0x1300 | resource` for `SET` (e.g. `battery_critical_mv`,
resource `0x0C`: `GET`=`0x030C`, `SET`=`0x130C`).

### 7.1 Worked example — get, set, and the cross-field rejection

Get the sensor task period:
```
Request:  00 03 00 00 90 DD                    (OPCODE=0x0300, LEN=0, CRC=0xDD90)
Response: 00 03 03 00 00 64 00 12 16            (status=0x00 OK, value=100 (0x0064), CRC=0x1612)
```

Set the battery critical threshold to 3650 mV (its own default — within range, and below the
current `battery_low_mv` default of 3800):
```
Request:  0C 13 02 00 42 0E 73 88               (OPCODE=0x130C, LEN=2, value=3650, CRC=0x8873)
Response: 0C 13 01 00 00 6C 2D                  (status=0x00 OK, CRC=0x2D6C)
```

Attempt to set it to 4000 mV instead — individually within the field's own 2500–4200 range,
but at or above the currently-stored `battery_low_mv` (3800), inverting the pair:
```
Request:  0C 13 02 00 A0 0F 82 EE               (OPCODE=0x130C, LEN=2, value=4000, CRC=0xEE82)
Response: 0C 13 01 00 08 64 AC                  (status=0x08 INVALID_PARAMETER, CRC=0xAC64)
```

---

## 8. Measurements (category `0x4`)

Individual live sensor readings. `GET` (one-shot) and `SUBSCRIBE`/`UNSUBSCRIBE` (periodic
push) all valid; not `SET`-able. Never blocks, no exclusivity rules apply (unlimited
concurrent requests/subscriptions, any transport, any time — none of these resources are in
the exclusivity group described in `docs/api-v2-spec.md` §4.4).

Every resource in this category shares the same request/response shape apart from the value
payload itself:

- **`GET`** — request: no payload (`LEN=0`). Response: status byte + the resource's current
  value (format per-resource, §8.1–§8.9 below). Status codes: `OK` · `BAD_CRC` · `BAD_LENGTH`
  (nonzero `LEN`).
- **`SUBSCRIBE`** — request: 4-byte little-endian `uint32 interval_ms`, valid range
  `50`–`3600000` (1 hour) inclusive. Response: status byte only (no value — the value arrives
  as the first push, a separate later packet). Status codes: `OK` · `BAD_CRC` · `BAD_LENGTH`
  (`LEN != 4`) · `INVALID_PARAMETER` (`interval_ms` outside the valid range). Re-subscribing
  to an already-subscribed resource on the same transport updates `interval_ms` in place
  (spec §4.3) — no error, no duplicate subscription, and the running `issue_seq` (below) is
  **not** reset.
- **`UNSUBSCRIBE`** — request: no payload (`LEN=0`). Response: status byte only. Status codes:
  `OK` · `BAD_CRC` · `BAD_LENGTH` (nonzero `LEN`) · `NOT_SUBSCRIBED` (no active subscription
  for this resource on this transport).
- **Subscription pushes** — sent under the **same opcode as the `SUBSCRIBE` request** (verb
  stays `SUBSCRIBE`, spec §3.1), once per `interval_ms`, until unsubscribed or the transport
  disconnects (disconnecting — or reconnecting — clears all of that transport's subscriptions;
  a host must re-`SUBSCRIBE` after every reconnect). Payload: `[status=OK][issue_seq][page][value]`.
  `issue_seq` (`uint8`) increments once per push and wraps at 256 — compare with wraparound-safe
  (modular) arithmetic, not `seq != prev+1`. `page` is always `0`: no resource in this category
  is large enough to ever need multi-packet delivery, so there is only ever one page per push.
  `value` is the same format as the `GET` response's value.

### 8.1 Onboard temperature — opcode `0x0400` / `0x3400` / `0x4400`

`resource=0x00`. Source: TMP236 (`Drivers_App/drv_tmp236.c`).

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 0 | `temp_cdeg` | `int16` | Centidegrees C (0.01°C/LSB) |

No validity companion field — firmware has no presence/fault signal for this sensor; a stale
last-known value and a live one are indistinguishable on the wire.

### 8.2 External temperature — opcode `0x0401` / `0x3401` / `0x4401`

`resource=0x01`. Source: LM35 (`Drivers_App/drv_lm35.c`).

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 0 | `temp_cdeg` | `int16` | Centidegrees C (0.01°C/LSB) |

Same no-validity-field caveat as §8.1.

### 8.3 BME280 temperature — opcode `0x0402` / `0x3402` / `0x4402`

`resource=0x02`. Source: BME280 environmental sensor (`Drivers_App/drv_bme280.c`), a
different physical sensor from §8.1/§8.2 — do not conflate the three.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 0 | `temp_cdeg` | `int16` | Centidegrees C (0.01°C/LSB) |
| 2 | `valid` | `uint8` | 0/1 — `false` until the first successful BME280 conversion after boot |

### 8.4 BME280 pressure — opcode `0x0403` / `0x3403` / `0x4403`

`resource=0x03`.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 0 | `pressure_pa` | `uint32` | Pascals |
| 4 | `valid` | `uint8` | 0/1, same meaning as §8.3 |

### 8.5 BME280 humidity — opcode `0x0404` / `0x3404` / `0x4404`

`resource=0x04`.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 0 | `humidity_centipct` | `uint16` | 0.01 %RH/LSB |
| 2 | `valid` | `uint8` | 0/1, same meaning as §8.3 |

### 8.6 Displacement sensor 1, delta — opcode `0x0405` / `0x3405` / `0x4405`

`resource=0x05`. Source: WP10 differential-capacitor displacement sensing
(`Services/svc_displacement.c`) — most-recent-cycle snapshot, not the full per-cycle stream
(the latter is deferred, see this document's status note).

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 0 | `delta_mm` | `float` | Displacement, mm |
| 4 | `valid` | `uint8` | 0/1 — `false` until at least one displacement cycle has been fully computed after boot |

### 8.7 Displacement sensor 1, residual — opcode `0x0406` / `0x3406` / `0x4406`

`resource=0x06`.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 0 | `residual` | `float` | Im(x) from the complex division — diagnostic, should sit near 0 if the physical model holds; not itself part of the displacement result |
| 4 | `valid` | `uint8` | 0/1, same meaning as §8.6 |

### 8.8 Displacement sensor 2, delta — opcode `0x0407` / `0x3407` / `0x4407`

`resource=0x07`. Same shape as §8.6, sensor 2.

### 8.9 Displacement sensor 2, residual — opcode `0x0408` / `0x3408` / `0x4408`

`resource=0x08`. Same shape as §8.7, sensor 2.

### 8.10 Worked example — subscribe, receive a push, unsubscribe

Subscribe to BME280 temperature (§8.3) at 1000 ms:

```
Request:  02 34 04 00 E8 03 00 00 0B 78
          (OPCODE=0x3402, LEN=4, interval_ms=1000, CRC=0x780B)
Response: 02 34 01 00 00 A7 84
          (status=0x00 OK, CRC=0x84A7)
```

First push, ~1000 ms later (temperature 26.50°C, sensor valid):

```
02 34 06 00 00 00 00 5A 0A 01 70 B1
(OPCODE=0x3402 -- same as the request, LEN=6, status=0x00 OK, issue_seq=0,
 page=0, temp_cdeg=0x0A5A=2650, valid=1, CRC=0xB170)
```

Unsubscribe:

```
Request:  02 44 00 00 C5 A8              (OPCODE=0x4402, LEN=0, CRC=0xA8C5)
Response: 02 44 01 00 00 D2 C6           (status=0x00 OK, CRC=0xC6D2)
```

Unsubscribing again with nothing active:

```
Request:  02 44 00 00 C5 A8              (identical request)
Response: 02 44 01 00 09 FB 57           (status=0x09 NOT_SUBSCRIBED, CRC=0x57FB)
```

Subscribing with an interval below the minimum (10 ms):

```
Request:  02 34 04 00 0A 00 00 00 D9 48  (OPCODE=0x3402, interval_ms=10)
Response: 02 34 01 00 08 AF 05           (status=0x08 INVALID_PARAMETER, CRC=0x05AF)
```

---

## 9. Transports

| Transport | Framing | Notes |
|---|---|---|
| USB | HID reports, up to 64 bytes/report (hardware limit) | A packet that doesn't fit in one 64-byte report cannot be sent or received yet — multi-report chaining for larger payloads is designed (`docs/api-v2-spec.md` §2.2) but not yet implemented. Not a concern for any resource documented above (largest response so far is 34 bytes total). |
| BLE | Raw byte stream (RN4871 transparent UART), reassembled by length | 115200 baud to the module. |
| UART | Raw byte stream (wired debug UART), reassembled by length | 115200 baud, 8N1. |

All three transports currently share a 128-byte single-packet reassembly ceiling
(`API2_PACKET_MAX_SIZE`) — again, not a concern for any resource documented above. This
ceiling, and USB's separate 64-byte physical-report limit, will both need revisiting once a
category with larger payloads (Debug messages, Raw data, or Bulk transfers, most likely) is
implemented.

---

## 10. Common error responses (worked examples)

These apply to any request, not one specific resource — shown against `GET` Identity
(§4.1) as a representative example.

**Bad CRC** — request with a deliberately wrong CRC:
```
Request:  00 00 00 00 FF FF        (OPCODE=0x0000, LEN=0, CRC intentionally wrong)
Response: 00 00 01 00 04 B8 66     (status=0x04 BAD_CRC, CRC=0x66B8)
```

**Unknown resource** — `GET` on System status resource `0x05` (only `0x00`/`0x01` exist):
```
Request:  05 00 00 00 85 38        (OPCODE=0x0005, LEN=0)
Response: 05 00 01 00 03 08 35     (status=0x03 UNKNOWN_RESOURCE, CRC=0x3508)
```

**Verb not valid** — `SET` (verb `0x1`) on System status resource `0x00` (GET-only category):
```
Request:  00 10 00 00 A3 C7        (OPCODE=0x1000, LEN=0)
Response: 00 10 01 00 02 D9 1D     (status=0x02 VERB_NOT_VALID, CRC=0x1DD9)
```

**Bad length** — `GET` Identity with one stray payload byte (expects `LEN=0`):
```
Request:  00 00 01 00 AB BD 22     (OPCODE=0x0000, LEN=1, payload=[0xAB])
Response: 00 00 01 00 05 99 76     (status=0x05 BAD_LENGTH, CRC=0x7699)
```

**Unknown category** — any opcode whose category field is `0x5`–`0xF` (not yet implemented, or
not defined) gets `UNKNOWN_CATEGORY` (`0x01`) the same way. (`0x2` Calibrations, `0x3`
Settings, and `0x4` Measurements are all implemented — see §6, §7, §8 respectively.)
