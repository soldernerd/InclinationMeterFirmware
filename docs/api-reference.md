# Device API v2 Reference

Full, standalone contract for writing host software against this device's binary
command/response protocol (USB HID, BLE, or wired UART). No firmware source-reading required.
Supersedes `docs/api.md` (the v1 reference).

> **Status: in progress.** This document is built out incrementally, one category at a time,
> alongside firmware implementation (WP11, per `docs/api-v2-spec.md` §8) — it is not written
> once at the end. **Currently covered: System status (§4), Commands (§5).** Calibrations,
> Settings, Measurements, Topic groups, Debug messages, Raw data, and Bulk transfers are
> designed (see `docs/api-v2-spec.md`) but not yet implemented in firmware, and have no
> resources documented here yet — sending any opcode in those categories today gets
> `UNKNOWN_CATEGORY` (§3).

---

## 1. Packet format

```
[OPCODE 2B][LEN 2B][PAYLOAD 0..LEN B][CRC16 2B]
```

- **OPCODE** — 16 bits, little-endian. Structure in §2.
- **LEN** — payload length in bytes, little-endian. No fixed maximum in the protocol itself;
  see §6's transport notes for the current practical ceiling.
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
| Calibrations | `0x2` | Not yet implemented |
| Settings | `0x3` | Not yet implemented |
| Measurements | `0x4` | Not yet implemented |
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

## 6. Transports

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

## 7. Common error responses (worked examples)

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

**Unknown category** — any opcode whose category field is `0x2`–`0xF` (not yet implemented or
not defined) gets `UNKNOWN_CATEGORY` (`0x01`) the same way.
