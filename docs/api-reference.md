# Device API v2 — Host Reference

The complete contract for host software talking to this firmware build
(fw 0.10.43). Design rationale is in `api-v2-spec.md`; this document is
what a host developer (mobile app, bench scripts) needs and nothing more.
`Services/svc_api.h` is the implementation-side source of truth this is
kept in sync with — if the two ever disagree, trust the header and treat
this file as needing an update.

All nine categories from the design spec are implemented: System status,
Commands, Calibrations, Settings, Measurements, Topic groups, Debug
messages, Raw data, Bulk transfers. Two verbs from the original design are
GET-only in practice on Raw data (no resource in that category is
currently subscribable, despite the design spec's category table saying
"yes" — see that section below).

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

16 bits: `[VERB:4 (top)] [CATEGORY:4] [RESOURCE:8]`. `OPCODE = (verb << 12) | (cat << 8) | res`.

| Verb | val | | Category | val |
|---|---|---|---|---|
| GET | 0x0 | | System status | 0x0 |
| SET | 0x1 | | Commands | 0x1 |
| EXECUTE | 0x2 | | Calibrations | 0x2 |
| SUBSCRIBE | 0x3 | | Settings | 0x3 |
| UNSUBSCRIBE | 0x4 | | Measurements | 0x4 |
| START_BULK | 0x5 | | Topic groups | 0x5 |
| CANCEL_BULK | 0x6 | | Debug messages | 0x6 |
| | | | Raw data | 0x7 |
| | | | Bulk transfers | 0x8 |

## Status codes (first response payload byte)

| | | | | |
|---|---|---|---|---|
| 0x00 OK | 0x01 UNKNOWN_CATEGORY | 0x02 VERB_NOT_VALID | 0x03 UNKNOWN_RESOURCE | 0x04 BAD_CRC |
| 0x05 BAD_LENGTH | 0x06 BUSY_RESOURCE | 0x07 BUSY_EXCLUSIVE | 0x08 INVALID_PARAMETER | 0x09 NOT_SUBSCRIBED |
| 0x0A NOTHING_TO_CANCEL | | | | |

`BUSY_RESOURCE` = a resource-specific busy condition (EEPROM write in
flight, the ADC never initialized, the wrong zero-cal/precision-measurement
step for the current phase). `BUSY_EXCLUSIVE` = a device-wide exclusivity
conflict — specifically: **Bulk transfers (0x8) and the real-time
displacement demod (Commands 0x01) are mutually exclusive**, device-wide,
across all transports (both want the ADS131M04's one sample-callback
slot). Starting one while the other is active gets `BUSY_EXCLUSIVE`.
There is no other exclusivity-group membership in this build — Raw data
(0x7) is GET-only, so there's no subscription state for it to conflict
with.

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
| 6 | u8 | reserved (always 0 — was `calibration_valid` for a REV A tilt store that no longer exists) |

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

### `EXECUTE 0x1/0x01` — Displacement (start/stop the demod)  → opcode `0x2101`
Request payload: 1 byte, `0` = stop the ADS131M04 sample stream + WP10
per-cycle demodulation, `1` = start it. Off at boot until
`Core/Src/main.c` auto-starts it at the very end of setup (so it's already
running for a normal boot; this EXECUTE is for stopping it, or restarting
after a stop). Response: status only, always `OK` regardless of whether
acquisition is actually healthy — check `GET Raw data 0x7/0x00`'s
`ads_ok` for that. **Mutually exclusive with Bulk transfers (0x8)** — see
the status-codes section above.

### `EXECUTE 0x1/0x02` — Force charge  → opcode `0x2102`
Request payload: none. Enables the charger regardless of SoC while USB is
present (a one-shot overnight top-off); self-clears on full or USB removal.
No-op with no USB.

### `EXECUTE 0x1/0x03` — Power test  → opcode `0x2103`
Request payload: `u32 mask` LE. Diagnostic — each bit keeps one
subsystem/rail/clock on, cleared bits cut it immediately. Response data
(4 B): the resulting `u32 mask` (also readable via `GET 0x7/0x01`). Bit map:

| bit | on ⇒ |
|---|---|
| 0 | 5V rail (display, temp sensors, buzzer buffer, −5V inverter, analog AFE) |
| 1 | switched 3V3 rail (EEPROM, BME280, battery-sense divider) |
| 2 | AD9833 DDS (chip awake + MCLK running) |
| 3 | ADS131M04 (out of reset + MCLK running) |
| 4 | RN4871 BLE (out of reset) |
| 5 | display (`DISP_ON` + VCOM toggle) |
| 6 | LEDs |
| 7 | CPU busy-spin (clear ⇒ `WFI` idle between ticks) |

Default at boot is `0xFF`. UART/USB stay up on any mask. Restoring bits is
best-effort (BLE/display want a reboot for a clean state). **Not a mobile-
app feature** — bench/diagnostic only; cutting the wrong rail can kill the
transport you're talking over.

### `EXECUTE 0x1/0x04` — Pin test  → opcode `0x2104`
Request payload: 1 byte. `bits[5:0]` drive 6 MCU→level-converter signals
(0 SCK / 1 MOSI / 2 CS / 3 DISP_ON / 4 VCOM / 5 BUZZER) as static push-pull
outputs; `bit6` = allow `DISP_ON` high (panel **must** be unplugged first —
this can damage the display otherwise); `bit7` = reboot back to normal
firmware. Response: status only, always `OK`; if `bit7` was set, the
device reboots ~immediately after acking. Arming (any pattern with
`bit7` clear) is irreversible without triggering the reboot bit or a
power cycle. **Bench/diagnostic only, not a mobile-app feature.**

### `EXECUTE 0x1/0x05` — Reboot to DFU  → opcode `0x2105`
Request payload: none. Response: status only, then the device reboots
into the ROM USB-DFU bootloader (VID `0x0483` / PID `0xDF11`) and **stays
there on every subsequent boot** — a power cycle does not recover the
application firmware; it must be reflashed with the `nBOOT0` option byte
restored. **Bench/diagnostic only — do not expose in the mobile app**
without a very deliberate, hard-to-reach confirmation flow; triggering
this by accident bricks the app-boot path until someone reflashes it over
SWD or DFU.

### `EXECUTE 0x1/0x06` — Zero calibration  → opcode `0x2106`
The classic 180-degree reversal test. 1-byte payload:

| value | action |
|---|---|
| `0x00` | cancel — abort an in-progress run. `OK` even if already idle. |
| `0x01` | step 1 — place the instrument in its starting orientation, then EXECUTE this. Averages ~128 batches (~1.6 s) at the current orientation. `BUSY_RESOURCE` if the demod (Commands 0x01) isn't running, or a precision measurement (Commands 0x07) is in progress. |
| `0x02` | step 2 — physically rotate the instrument 180°, then EXECUTE this. Same averaging at the new orientation, then computes and **persists** new `disp_s1/s2_zero_offset_um` to this instrument's own EEPROM (Calibrations 0x2, resources 0x03/0x06). `BUSY_RESOURCE` if step 1 hasn't finished yet. |

Each EXECUTE acks (`OK` or `BUSY_RESOURCE`) immediately; the averaging
itself takes time in the background. **Poll progress via `GET Raw data
0x7/0x03`.** A `GET` on Calibrations `0x2/0x03` or `0x2/0x06` after step 2
completes reads back the newly persisted offset. Entirely local to this
physical instrument — never touches anything shared across units.

### `EXECUTE 0x1/0x07` — Triggered precision measurement  → opcode `0x2107`
The "trigger it, device takes the time it needs, reports one reliable
value" mode, for the actual measurement use case (as opposed to watching
the continuous live/streaming readout). 1-byte payload:

| value | action |
|---|---|
| `0x00` | start — begin averaging up to 64 quality-good batches **per sensor**, stopping once BOTH sensors reach 64 or 4000 ms elapses, whichever comes first. Restarts a fresh run if one was already in progress. `BUSY_RESOURCE` if the demod isn't running or a zero-cal is in progress. |
| `0x01` | cancel — abort an in-progress run. `OK` even if already idle/done. |

Acks immediately; **poll progress and read the result via `GET Raw data
0x7/0x04`.** Typical (clean-signal) completion is ~3.2 s — note this is
*not* ~2 s even in the best case, since 64 samples at the ~20.3 Hz source
rate takes ~3.15 s minimum with zero discards; the 4 s figure is a worst-
case ceiling, not the expected time. Unlike zero-cal, nothing is written
to EEPROM — the result is a plain, repeatable `GET`, valid until the next
`start`. See the worked example at the end of this document for the full
start→poll→read flow.

---

## Calibrations (0x2) — GET, SET

Sensor-correction constants for the WP10 displacement path — distinct
from Settings (0x3), which is operational/behavioral config. All fields
are **4-byte payload** (`i32` on the wire, LE), regardless of sign.
**None of these values are independently meaningful physical quantities
yet** — `d0_um` in particular is a coarse software scale factor standing
in for a still-missing real gain/d0 calibration against a certified
reference (it does not represent a literal sensor air gap). Treat these
as tuning knobs, not calibrated physical constants, until a proper bench
calibration is done.

| res | opcode (GET) | field | range |
|---|---|---|---|
| 0x00 | `0x0200` | `disp_atten_milli` — shared A/B attenuator, x1000 (nominal 3.000) | 100…100000 |
| 0x01 | `0x0201` | `disp_s1_gain_milli` — S1 amplifier gain, x1000 (nominal 160.000 as of the PGA=16 bump) | 100…1000000 |
| 0x02 | `0x0202` | `disp_s1_d0_um` — S1 scale factor, micrometers (NOT a literal air gap — see above) | 1…2000000 |
| 0x03 | `0x0203` | `disp_s1_zero_offset_um` — S1 zero calibration, micrometers, signed | −5000…5000 |
| 0x04 | `0x0204` | `disp_s2_gain_milli` — S2 amplifier gain, x1000 | 100…1000000 |
| 0x05 | `0x0205` | `disp_s2_d0_um` — S2 scale factor, micrometers | 1…2000000 |
| 0x06 | `0x0206` | `disp_s2_zero_offset_um` — S2 zero calibration, micrometers, signed | −5000…5000 |

- **GET** (`0x02xx`): payload none → `[OK][i32 LE value]`.
- **SET** (`0x12xx`): payload = 4-byte `i32` LE value. `BAD_LENGTH` if not
  4 bytes, `INVALID_PARAMETER` if out of range, `BUSY_RESOURCE` if an
  EEPROM write is already in flight or the save fails. On success, the
  value is persisted immediately (no separate commit step).
- `zero_offset_um` (0x03/0x06) is normally set by the zero-calibration
  procedure (Commands 0x06), not written directly — but a direct `SET` is
  allowed (e.g. to restore a previously-read-back value, or clear to 0).

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
| 0x07 external temp | `0x0407` | i16, centi-°C (LM35) |
| 0x08 external temp valid | `0x0408` | u8, 0/1 |
| 0x09 displacement S1 delta | `0x0409` | f32 LE, mm — **post**-moving-average (smoothed over the last 4 batches) |
| 0x0A displacement S1 residual | `0x040A` | f32 LE, Im(x1) — should sit near 0; a consistently nonzero value usually means a Calibrations constant is off |
| 0x0B displacement S2 delta | `0x040B` | f32 LE, mm — post-moving-average |
| 0x0C displacement S2 residual | `0x040C` | f32 LE, Im(x2) |
| 0x0D displacement ok | `0x040D` | u8, 0/1 — 0x09–0x0C are meaningless while this is 0 |

- **GET**: request payload none → response `[OK][value]`.
- **SUBSCRIBE** (`0x34xx`): request payload = `u32 interval_ms` LE, range
  50 … 3 600 000. Ack is status-only. Then periodic pushes follow **under
  the same opcode**, payload `[OK][issue_seq u8][page u8=0][value]`.
  Re-SUBSCRIBE to the same resource updates the interval in place.
- **UNSUBSCRIBE** (`0x44xx`): payload none. `NOT_SUBSCRIBED` if there was
  no active subscription on this transport.
- All subscriptions are per-transport and cleared on connect/disconnect.
- **0x09/0x0B are smoothed** (a 4-sample boxcar on top of the underlying
  ~20.3 Hz batch rate). If you want the raw, pre-smoothing value at the
  full batch rate for tighter-loop analysis, use Topic groups `0x5/0x03`
  instead (below) — it carries the same two deltas/residuals unsmoothed,
  plus a per-batch quality flag.

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
| 11 | i16 | external temperature, centi-°C (LM35, TEMP_SENSE_EXT) |
| 13 | u8 | external temperature valid (0 if out of range / no sensor) |

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

### `0x5/0x02` — Displacement phasors  → GET `0x0502`, SUBSCRIBE `0x3502` (32 B payload)

Bench/diagnostic — the demod's raw intermediate I/Q values, one step
upstream of the delta/residual math. Not needed for normal app operation,
but useful for diagnosing a wiring/calibration problem that delta/residual
alone can't distinguish from "device working, instrument tilted." All
`f32` LE, valid only while Measurements `0x0D` (disp_ok) is true:

| off | type | field |
|---|---|---|
| 0 | f32 | iB (Exciter B, CH1) |
| 4 | f32 | qB |
| 8 | f32 | iA (Exciter A, CH2) |
| 12 | f32 | qA |
| 16 | f32 | iS1 (Sensor 1, CH3) |
| 20 | f32 | qS1 |
| 24 | f32 | iS2 (Sensor 2, CH0) |
| 28 | f32 | qS2 |

A healthy A/B pair should read ~180° apart in phase; if it doesn't,
suspect wiring, not calibration.

### `0x5/0x03` — Raw (pre-smoothing) displacement  → GET `0x0503`, SUBSCRIBE `0x3503` (18 B payload)

The **recommended resource for anything that needs the sensor value at
full rate or wants to reason about data quality** — e.g. the precision-
measurement feature's own internal averaging is built on exactly this
data. Same ~20.3 Hz source rate as the phasors topic above (both come
from the same underlying batch); subscribe at the 50 ms floor to track it
essentially 1:1 (batch period is ~49.2 ms). Valid only while Measurements
`0x0D` (disp_ok) is true:

| off | type | field |
|---|---|---|
| 0 | f32 | delta1_mm_raw — Sensor 1, **pre**-moving-average |
| 4 | f32 | residual1 — Im(x1), identical to Measurements 0x0A |
| 8 | f32 | delta2_mm_raw — Sensor 2, pre-moving-average |
| 12 | f32 | residual2 — Im(x2), identical to Measurements 0x0C |
| 16 | u8 | quality1_ok — 0/1, see below |
| 17 | u8 | quality2_ok — 0/1, see below |

**Quality flag (bench-validated 2026-09-26):** a real jump/glitch in a
sensor's underlying signal makes its residual step by ~4-4.6x its normal
size in the *same* batch. `quality1/2_ok` tracks a rolling baseline of the
residual's typical step size and flags a batch `0` (bad) when the current
step exceeds 3x that baseline. `0` means: treat this specific batch's
`delta*_mm_raw` with suspicion — it is *not* a hard guarantee of error,
just a statistically-motivated warning. A host doing its own averaging
(rather than using the triggered precision measurement, which already
does this) should discard or downweight batches where the relevant
`quality*_ok` is 0.

---

## Settings (0x3) — GET, SET

Every surviving `DeviceSettings` field. Resource IDs are **stable wire
values, not a dense sequence** — `0x01` and `0x07`–`0x0B` are retired
(they were REV A task-scheduling / settling / complementary-filter fields
removed when the REV A SCL3300/PCAP04 sensor stack was pulled). The gaps
are kept deliberately so every surviving ID keeps its number across
firmware versions — **do not treat the resource list as contiguous, and
do not reuse the retired numbers for anything new.**

| res | field | width | range |
|---|---|---|---|
| 0x00 | task_sensors_ms | u16 | 1…60000 |
| ~~0x01~~ | *(retired — was task_processing_ms)* | — | — |
| 0x02 | task_display_ms | u16 | 1…60000 |
| 0x03 | task_ble_ms | u16 | 1…60000 |
| 0x04 | task_usb_ms | u16 | 1…60000 |
| 0x05 | task_battery_ms | u16 | 1…60000 |
| 0x06 | task_temperature_ms | u16 | 1…60000 |
| ~~0x07~~–~~0x0B~~ | *(retired — were stream_interval_ms, settling_threshold, settling_timeout_ms, filter_cutoff_hz_num/den, all REV A)* | — | — |
| 0x0C | battery_critical_mv | u16 | 2500…4200, must stay `<` battery_low_mv |
| 0x0D | battery_low_mv | u16 | 2500…4200, must stay `>` battery_critical_mv |
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
| 0x1C | vbat_offset_mv | i32 | −500…500 (additive Vbat correction, bench-calibrated) |

> `0x1B`/`0x1C` are appended out of struct order (their `DeviceSettings`
> fields sit mid-struct in the battery EEPROM page) — this doesn't affect
> the wire protocol, `0x00`–`0x1A` simply keep the indices they already had.

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

---

## Raw data (0x7) — GET only

Development/debug intermediate values. **Not subscribable in this
build** despite the design spec listing Raw data as subscribable in
principle — every resource here is GET-only; `SUBSCRIBE`/`UNSUBSCRIBE`
return `VERB_NOT_VALID`.

### `GET 0x7/0x00` — ADS131M04 diagnostics  → opcode `0x0700`
Request payload: none. Response (81 B, LE) — register read-back plus
acquisition-integrity counters:

| off | type | field |
|---|---|---|
| 0 | u16 | reg_id |
| 2 | u16 | reg_status |
| 4 | u16 | reg_mode |
| 6 | u16 | reg_clock |
| 8 | u16 | reg_gain1 |
| 10 | u16 | reg_cfg |
| 12 | u16 | clock_expected (what the driver wrote to CLOCK) |
| 14 | u8 | regs_read_ok (all RREG transfers succeeded) |
| 15 | u8 | ads_ok |
| 16 | u16 | last_capture_samples (most recent Bulk 0x8/0x00 capture) |
| 18 | u16 | last_capture_drops |
| 20 | u32 | last_capture_elapsed_ms |
| 24+ | — | acquisition-integrity tail (frame counters, fault code — bench/debug use only, see `Drivers_App/drv_ads131m04.h`'s `Ads131m04Integrity` for the exact layout) |

`reg_clock`'s OSR field is bits `[4:2]`: `0`=128, `1`=256, `2`=512,
`3`=1024, `4`=2048, `5`=4096, `6`=8192, `7`=16256; `fDATA = fCLKIN / (2 *
OSR)`, `fCLKIN ≈ 5.3333 MHz`.

### `GET 0x7/0x01` — Power-test state  → opcode `0x0701`
Request payload: none. Response (5 B):

| off | type | field |
|---|---|---|
| 0 | u32 | mask — current Commands 0x03 bitmask |
| 4 | u8 | flags — bit0 3V3 rail on, bit1 5V rail on (read back from the pins) |

### `GET 0x7/0x02` — Displacement diagnostics  → opcode `0x0702`
Request payload: none. Response (21 B, LE):

| off | type | field |
|---|---|---|
| 0 | u16 | input_drop_count — a completed carrier cycle was dropped, consumer not keeping up |
| 2 | u16 | output_drop_count — a computed batch was dropped from the (currently unread) internal result ring; expected to climb, not a fault |
| 4 | u16 | degenerate_count — a batch's A/B phasors were exactly identical (should not occur in practice) |
| 6 | u8 | disp_ok |
| 7 | u16 | phasor_log_progress — entries stored so far in the current/most recent Bulk 0x8/0x01 capture |
| 9 | u16 | clip_count — a raw ADC code rode near a rail (real clipping) |
| 11 | u16 | amplitude_fault_count — a batch's phasor exceeded what a full-scale, undistorted signal could produce (a data-integrity/gain-mismatch assertion, **not** a clipping indicator — real clipping can only reduce this value, never increase it) |
| 13 | u16 | max_update_gap_ms — longest gap seen between consecutive internal scheduler passes of the displacement task since the last start; a scheduler-latency diagnostic, not something to build app logic on |
| 15 | u32 | max_gap_at_uptime_ms — device uptime (ms) when that max was recorded |
| 19 | u16 | gap_over_threshold_count — count of scheduler passes since start that came in unusually late (bench/diagnostic use) |

`clip_count`/`amplitude_fault_count`/`gap_over_threshold_count` are all
saturating (stick at 65535) and reset only on a fresh `EXECUTE 0x1/0x01`
start. The last three fields (`max_update_gap_ms` onward) are bench/
diagnostic instrumentation for investigating scheduler timing — not
meant for the mobile app to act on, included here only for completeness.

### `GET 0x7/0x03` — Zero-calibration status  → opcode `0x0703`
Request payload: none. Response (5 B, LE):

| off | type | field |
|---|---|---|
| 0 | u8 | phase (0 idle, 1 step1 running, 2 step1 done/ready for step2, 3 step2 running, 4 result ready — transient, applied within one tick, so polling slower than that will normally see phase go straight from 3 back to 0) |
| 1 | u16 | progress — batches averaged so far in the current step, 0 while idle/between steps |
| 3 | u16 | target — always 32 (the sample count for a zero-cal step, ~1.6s at the current batch rate), no need to hardcode |

### `GET 0x7/0x04` — Precision-measurement status  → opcode `0x0704`
Request payload: none. Response (20 B, LE):

| off | type | field |
|---|---|---|
| 0 | u8 | phase (0 idle, 1 running, 2 done) |
| 1 | u16 | target — always 64, no need to hardcode |
| 3 | u16 | count1 — quality-good batches averaged so far, Sensor 1 (0..target) |
| 5 | u16 | count2 — same, Sensor 2 |
| 7 | u32 | elapsed_ms — wall-clock time since the triggering EXECUTE, 0 while idle |
| 11 | u8 | timed_out — 1 if the 4000ms ceiling was hit before both sensors reached target (only meaningful once phase==done) |
| 12 | f32 | delta1_mm — mean of the count1 batches actually collected; valid once phase==done, 0 before that |
| 16 | f32 | delta2_mm — same, Sensor 2 |

See the worked example below for the full flow.

---

## Bulk transfers (0x8) — START_BULK, CANCEL_BULK

Large, RAM-buffered, one-shot transfers. **Bench/diagnostic — not needed
for normal mobile-app operation.** Device-wide exclusive with each other
and with the real-time displacement demod (Commands 0x01) — see the
status-codes section above.

### `0x8/0x00` — Raw ADC capture  → START `0x5800`, CANCEL `0x6800`
`START_BULK`: no request payload (transfer size is fixed: 6144 samples ×
4 channels × 3 bytes). Response is the usual `[status]` ack; the capture
then runs in the background (~295 ms) and the samples stream out
asynchronously **under the same opcode**, chunked as
`[status=OK][page:1][sample:12]×N` (N ≤ 10, 12 bytes/sample = ch0..ch3 as
3-byte LE signed codes each), page wrapping 0–255. `CANCEL_BULK` aborts
an active capture/transfer and frees the exclusivity slot immediately.
`BUSY_RESOURCE` if the ADC never initialized (`ads_ok` false);
`BUSY_EXCLUSIVE` if the demod is running or another bulk transfer is
active.

### `0x8/0x01` — Phasor log capture  → START `0x5801`, CANCEL `0x6801`
A longer-duration, decimated companion to `0x00`: 512 entries of the
demod's batch-level phasors at every 2nd completed batch (~40.7 Hz
effective), spanning ~12.6 s — for diagnosing drift or slow mechanical
behavior the ~0.3 s raw-ADC capture can't reach. Same START/CANCEL shape
and exclusivity as `0x00`. Chunk payload is `[page:1][entry:34]×N` (N ≤ 3);
one entry is 8× `f32` LE (`iB,qB,iA,qA,iS1,qS1,iS2,qS2` — same layout as
Topic groups `0x5/0x02`) plus a `u16` `seq` (the last raw cycle folded
into that stored batch, for gap detection).

---

## Worked example: triggered precision measurement

The recommended flow for the mobile app's "take a reading" action:

1. Confirm the demod is running and healthy:
   `GET 0x4/0x0D` (disp_ok) → expect `[OK][01]`. If `00`, `EXECUTE 0x1/0x01`
   with payload `01` first.
2. Start the measurement: `EXECUTE 0x1/0x07` payload `00` →
   `[OK]`. (`[BUSY_RESOURCE]` if the demod isn't running or a zero-cal is
   in progress — resolve that first, don't retry blindly.)
3. Poll every ~200–500 ms: `GET 0x7/0x04` → decode `phase`. While
   `phase==1` (running), `count1`/`count2` climb toward `target` (64) and
   `elapsed_ms` climbs toward the 4000 ms ceiling — a progress bar can use
   `min(count1,count2)/target`.
4. Once `phase==2` (done): read `delta1_mm`/`delta2_mm` as the final
   values. Check `timed_out` — if `1`, the result is still the mean of
   whatever was collected (`count1`/`count2`, which may be less than 64),
   not a hard failure, but worth surfacing as lower-confidence in the UI
   (e.g. show the achieved sample count next to the result).
5. The result stays readable (repeat `GET 0x7/0x04`) until the next
   `EXECUTE 0x1/0x07` payload `00` starts a fresh run.

Expect ~3.2 s typical wall-clock time for a clean signal, not ~2 s — see
that command's own section above for why.
