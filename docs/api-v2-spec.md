# Device API v2 — Design Specification

Status: **design agreed, not yet implemented**. This document replaces the protocol currently
implemented in `Services/svc_api.c`/`svc_api.h` (see `api.md` for the v1 reference this
supersedes). It is a specification to implement against, not a description of existing code.

---

## 1. Goals / philosophy

Same transport-agnostic philosophy as v1 — one packet format, one opcode set, one dispatcher
in `svc_api.c`, reused verbatim by every transport (`svc_usb.c`, `svc_ble.c`, `svc_uart.c`
remain thin adapters that move bytes in/out).

What changes from v1, and why:

- **No fixed 64-byte packet, no zero-padding.** The 64-byte ceiling and padding were an
  artifact of USB HID report size, not a real requirement of BLE or UART — both already
  reassemble a variable-length stream via a length field. Packets are now exactly as long as
  they need to be. USB gains a new obligation it didn't have before: a payload larger than one
  HID report now needs multi-report reassembly on the USB side too (BLE/UART already do this).
- **2-byte opcode with internal structure**, replacing v1's flat sequential 1-byte opcode
  space. Opcode = verb + category + resource index (§3). This gives the protocol room to grow
  by category rather than by appending to one flat list, and lets the dispatcher validate a
  request in stages (§6).
- **A single full-byte status/reason code** on every response, replacing v1's bare ACK/NACK.
  (§5)
- **Explicit concurrency model**: one-shot requests, subscriptions, and bulk transfers have
  different blocking/coexistence rules, defined explicitly rather than falling out of a shared
  per-transport mode flag that could be silently overwritten (v1's `ApiMode` problem). (§4)
- **CRC kept at the API layer**, not delegated to the transport — wired UART has no
  link-layer error detection at all, so this is the only thing protecting it. (§7)

---

## 2. Packet format

```
[OPCODE 2B][LEN 2B][PAYLOAD 0..LEN B][CRC16 2B]
```

- `OPCODE` — see §3. Little-endian on the wire.
- `LEN` — payload length in bytes. 2 bytes (not 1) since payloads are no longer capped at
  ~60 bytes — a topic group or a bulk chunk may need more. Little-endian.
- `PAYLOAD` — opcode-specific, little-endian (native STM32G0 byte order).
- `CRC16` — same algorithm as v1 (`math_crc16()`, CRC-16/CCITT-FALSE: poly `0x1021`, init
  `0xFFFF`, no reflection, no XOR-out), computed over `OPCODE + LEN + PAYLOAD`. Little-endian
  on the wire.
- No padding. Total packet size on the wire is always `6 + LEN`.

A response always echoes the request's `OPCODE`. This holds for every kind of traffic,
including subscription pushes and bulk chunks — see §4.4, there is no separate
"unsolicited"/push opcode or verb.

### 2.1 Byte-stream transports (BLE, UART)

Same reassembly approach as v1: `svc_ble.c`/`svc_uart.c` feed bytes into a reassembler; once
the header (4 bytes: opcode + len) is in hand the declared length is known, and once the full
`6 + LEN` bytes have arrived the packet dispatches and the reassembler resets. A partial packet
that stalls past a timeout is abandoned, as in v1.

### 2.2 USB

USB HID reports remain capped at ~64 bytes per transfer (this is a hardware/USB-spec limit,
not a protocol choice). A payload larger than one report's worth is delivered as multiple
reports; the host and device reassemble across reports the same way BLE/UART reassemble across
a byte stream. This is new relative to v1, where every packet fit in exactly one report by
construction.

### 2.3 Sequencing (page / issue counters)

Two independent 1-byte wrapping counters, used where relevant:

- **Page counter** — position within one atomic multi-packet delivery. Applies to `GET`
  responses (single value, topic group) and bulk chunks alike: "which piece of this one
  payload am I looking at." Resets at the start of each new atomic delivery.
- **Issue counter** — which periodic delivery this is, for subscriptions only. Independent of
  the page counter, so a host can distinguish "I lost page 2 of this delivery" from "I missed
  an entire periodic delivery."

Both wrap (1 byte); gap detection must be done with wraparound-safe (modular) comparison, not
naive `!= prev + 1`.

---

## 3. Opcode structure

16-bit opcode, three fields:

```
[VERB: 4 bits][CATEGORY: 4 bits][RESOURCE INDEX: 8 bits]
```

### 3.1 Verbs (7 defined, 9 reserved)

| Verb | Meaning |
|---|---|
| `GET` | One-shot read |
| `SET` | Persist a value (calibration, settings) |
| `EXECUTE` | Fire a one-shot action with no stored value (start charging, enter sleep, etc.) |
| `SUBSCRIBE` | Begin periodic/event-driven push delivery for a resource |
| `UNSUBSCRIBE` | End a previously started subscription |
| `START_BULK` | Begin a bulk transfer |
| `CANCEL_BULK` | Abort an in-progress bulk transfer |

There is no separate verb for device-initiated/push traffic. `SUBSCRIBE`/`START_BULK` get an
explicit status-code response first, like every other request (§4.2, §5); the subscription's
pushed data or the bulk transfer's chunks then follow as their own separate packets, still
under the **same opcode** the host used to request them — the request/response opcode-echo
rule covers this without a special case. A host distinguishes "this is the SUBSCRIBE ack" from
"this is pushed data" by packet order: the ack comes first, exactly once; pushes follow,
zero or more times, until unsubscribed/cancelled/complete.

### 3.2 Categories (8 defined, 8 reserved)

| Cat. | Name | Allowed verbs | Subscribable | Notes |
|---|---|---|---|---|
| `0x0` | System status | `GET` | No | Firmware version, serial number, connection/charging state, etc. |
| `0x1` | Commands | `EXECUTE` | No | Fire-once actions: start/stop charging, start measurement, sleep, etc. |
| `0x2` | Calibrations | `GET`, `SET` | No | Sensor-correction constants: offsets, gains |
| `0x3` | Settings | `GET`, `SET` | No | Operational/behavioral config: battery thresholds, scheduler periods, etc. |
| `0x4` | Measurements | `GET`, `SUBSCRIBE`, `UNSUBSCRIBE` | Yes | Individual values: onboard temperature, sensor inclination, humidity, etc. |
| `0x5` | Topic groups | `GET`, `SUBSCRIBE`, `UNSUBSCRIBE` | Yes | Fixed-at-compile-time sets of related measurements, to avoid subscribing to many individual streams |
| `0x6` | Debug messages | `SUBSCRIBE`, `UNSUBSCRIBE` | Yes (only) | Log stream, severity levels (info/warning/error at minimum). No `GET` — a stream has no "current value." Max message length capped at a fixed `MAX_DEBUG_MSG_LEN` (TBD) since the stream itself is unbounded. |
| `0x7` | Raw data | `GET`, `SUBSCRIBE`, `UNSUBSCRIBE` | Yes | Development/debug intermediate values — raw ADC readings, phasors, etc. |
| `0x8` | Bulk transfers | `START_BULK`, `CANCEL_BULK` | N/A | Large RAM-buffered transfers: display dump, raw ADC capture, etc. |
| `0x9`–`0xF` | *(reserved)* | — | — | Available for future work packages |

**System time** is a special case within Calibrations or Settings (TBD which) — unlike other
entries in those categories, it's not a constant; both `GET` and `SET` are meaningful and the
value changes on its own between reads.

### 3.3 Resource index

8 bits (0–255) per category, assigned per-resource as the API is fleshed out. Not exhausted by
any category today — plenty of headroom for new measurements/topic groups/etc. without
reshuffling existing IDs.

### 3.4 Dispatch validation order

The dispatcher should validate in this order, matching the status-code priority in §5:

1. Category exists (top 4 bits recognized)
2. Verb is valid for that category
3. Resource index exists within the category
4. CRC valid
5. Length matches what the resource expects
6. Resource/exclusivity busy checks
7. Payload parameter values in range

---

## 4. Concurrency model

### 4.1 Three traffic classes

| Class | Blocking? | Concurrent instances | Delivery |
|---|---|---|---|
| One-shot (`GET`, `SET`, `EXECUTE`) | No | Unlimited, any transport | Atomic — start to finish uninterrupted by other traffic, even if it spans multiple packets |
| Subscription (`SUBSCRIBE`/`UNSUBSCRIBE`) | No | Many, per-transport, one per topic (see §4.3) | Each individual push is atomic (like a `GET` response); pushes over time are not required to be contiguous with each other |
| Bulk (`START_BULK`/`CANCEL_BULK`) | Yes, w.r.t. other bulk only | At most one, **device-wide** (RAM-buffer constraint, not per-transport) | Only bulk chunks may be interleaved with other traffic mid-transfer — this is the *only* interruptible delivery in the protocol |

Only bulk-chunk delivery yields between chunks so other atomic responses and subscription
pushes can get a turn. There is no general packet-priority scheduler — one-shot and
subscription responses are never split by anything, by design.

### 4.2 Subscribe/unsubscribe confirmation

`SUBSCRIBE` and `UNSUBSCRIBE` are acknowledged like every other request — a status-code
response (§5) is returned immediately, before any data push. No exception: this matches §5's
"no request goes unanswered" rule, which applies uniformly across all verbs. The first actual
pushed value follows afterward, as its own separate packet under the same opcode.

### 4.3 One subscription per topic

Re-subscribing to an already-subscribed resource (same transport) updates that subscription's
parameters (e.g. interval) rather than creating a second, independent subscription. No
subscription-ID field needed — the opcode (verb+category+resource) is the subscription's
identity, scoped per transport.

### 4.4 Exclusivity group

Heavy/data-intensive subscriptions (e.g. raw ADC in real time) and bulk transfers are mutually
exclusive, **device-wide**, across all transports simultaneously — this is a RAM-contention
constraint, not a per-transport limit. Only one member of this group may be active at any
time; starting a second is NACKed (`BUSY_EXCLUSIVE`, §5).

Which specific resources fall into the exclusivity group is a per-resource flag, not implied
by category alone — most `0x7` (Raw data) subscriptions and all of `0x8` (Bulk) fall in it;
light subscriptions (e.g. periodic temperature) do not.

Multiple simultaneous transports are expected to be rare in practice but must be handled
correctly regardless.

### 4.5 Bulk transfer specifics

- `START_BULK` is acknowledged with a status-code response (§4.2, §5) before chunk 0, like
  every other request. Transfer size is **known in advance from the API description**
  (documented per bulk resource) rather than communicated in that response — so the ack itself
  carries no payload beyond the status code, and no separate "here's the total size" handshake
  is needed.
- Delivery is reliable: the device blocks/retries at the transport layer until each chunk is
  actually sent — no silent drop (this differs from v1's USB behavior, where a busy IN
  endpoint just dropped the packet).
- Each chunk carries the page counter (§2.3) for gap detection.
- **No chunk-level retransmission.** If the host detects a CRC failure or a gap on a bulk
  transfer, it cancels (`CANCEL_BULK`) and restarts the whole transfer. Deliberate
  simplicity/RAM tradeoff — chunk-level resend would require the device to buffer or
  regenerate already-sent chunks, which competes with the same RAM constraint that limits
  bulk transfers to one at a time. Corruption is expected to be rare enough that this is
  acceptable.
- `CANCEL_BULK` releases the device's RAM buffer (and the exclusivity slot, §4.4) immediately,
  not on a timeout.

---

## 5. Response status codes

Every response carries a full status byte (not a bare ACK/NACK). No request goes unanswered —
`SUBSCRIBE`, `SET`, `EXECUTE`, `START_BULK`/`CANCEL_BULK` all get at least this byte.

| Code | Name | Meaning |
|---|---|---|
| `0x00` | `OK` | Success |
| `0x01` | `UNKNOWN_CATEGORY` | Top 4 bits of the opcode don't match a defined category |
| `0x02` | `VERB_NOT_VALID` | Verb not valid for this category (e.g. `SUBSCRIBE` on Commands) |
| `0x03` | `UNKNOWN_RESOURCE` | Category valid, resource index not defined within it |
| `0x04` | `BAD_CRC` | Request failed CRC check |
| `0x05` | `BAD_LENGTH` | Payload length doesn't match what the resource expects |
| `0x06` | `BUSY_RESOURCE` | Resource-specific busy condition (e.g. EEPROM write in progress) |
| `0x07` | `BUSY_EXCLUSIVE` | Exclusivity group conflict (§4.4) — another heavy subscription or bulk transfer is active |
| `0x08` | `INVALID_PARAMETER` | Payload parses, but a field's value is out of range |
| `0x09` | `NOT_SUBSCRIBED` | `UNSUBSCRIBE` on a resource with no active subscription on this transport |
| `0x0A` | `NOTHING_TO_CANCEL` | `CANCEL_BULK`, or cancelling a single-shot action, with nothing active |

**No exceptions**: unlike v1 (where `CANCEL_SINGLE` ACKs unconditionally even with nothing in
progress), every cancel-type operation NACKs with `NOTHING_TO_CANCEL` if there's nothing to
cancel. Same for `UNSUBSCRIBE` with no active subscription.

`BUSY_RESOURCE` vs `BUSY_EXCLUSIVE` are deliberately distinct — they call for different host
behavior (retry shortly vs. wait for the other exclusive-group operation to finish).

---

## 6. Data integrity

CRC is enforced at the API layer, uniformly across all transports — not delegated to
transport-level error detection, since wired UART has none at all (USB/BLE both have
link-layer CRC already; relying on that would leave UART with zero protection).

- **Scope**: per on-wire packet, not per reassembled atomic message. Corruption is caught
  before reassembly effort is wasted stitching together a value that will have to be
  discarded anyway.
- **Host → device**: device detects bad CRC, responds `BAD_CRC` (§5). Host resends the request.
- **Device → host**: the device cannot detect its own corrupted transmissions; the host must
  check CRC on receipt. For one-shot responses and subscription pushes, a bad packet is simply
  discarded — the host re-requests (one-shot) or waits for the next push (subscription). For
  bulk transfers, see §4.5 — no targeted resend, restart the whole transfer.

---

## 7. Capability / version discovery

No separate capability-negotiation mechanism. A host queries firmware version via the System
status category (`GET`) and is expected to know, from documentation per firmware version,
which resources/verbs that build supports. This is a deliberate simplification — revisit only
if version-specific behavior differences become hard to manage in practice.

---

## 8. Host-facing API reference (deliverable, separate from this document)

This design document is for whoever implements the protocol. It is **not** what a host-side
developer should have to read. A separate reference document must be produced — the complete,
standalone contract for anyone writing host software against the device, with no need to know
or care about firmware internals (scheduler, RAM buffering, EEPROM, transport adapter code,
etc.).

That reference must be a **full and complete listing**, not a summary — every verb, every
category, every defined resource/opcode, and every status code, each fully specified. At
minimum, for each individual command (verb + resource):

- Full opcode (verb, category, resource index — and the resulting 16-bit value)
- Exact request payload format: field names, types, byte offsets, units, valid ranges
- Exact response payload format, same level of detail
- Whether it's blocking or non-blocking, and which exclusivity/concurrency rules apply to it
  (§4) — e.g. "conflicts with any other exclusivity-group member," "always non-blocking"
- Whether it's subscribable, and if so, the payload format of each pushed update (which may
  differ from the `GET` response) and how the issue/page counters apply to it
- All status codes it can plausibly return, with the specific meaning in that command's context
  (not just a link to the generic table in §5)
- Worked example: request bytes → response bytes, for at least the non-trivial commands

This reference is a required deliverable of implementing this spec, produced and kept in sync
as each category/resource is built out — not written once at the end from memory. It supersedes
`api.md` (the v1 reference) once complete.

---

## 9. Open items / explicitly deferred

- Exact resource index assignments within each category (System status, Measurements, Topic
  groups, etc.) — to be defined as each is implemented.
- `MAX_DEBUG_MSG_LEN` value.
- Whether System time lives under Calibrations or Settings.
- Per-resource exclusivity-group membership list (§4.4) — needs to be enumerated explicitly
  once the Raw data and Bulk resource lists are finalized.
- Migration/compatibility story for existing v1 clients (Flutter app, LevelApp) — not
  addressed in this document; this is a breaking protocol change.
