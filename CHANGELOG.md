# Changelog

Firmware versions are `FW_VERSION_*` in `Config/app_version.h`. Until 1.0.0 the minor number moves with breaking changes
of the wire protocol or the stored settings; the patch number with every build handed over for flashing. Entries are
newest first. **Nothing from 0.10.64 on has been bench-tested yet** unless a line says so; the acceptance list for 1.0.0 is
`docs/release-checklist.md`.

## 0.13.1 - 2026-11-10
Fixes 0.13.0: the **phasor stream** was still sent in the old B, A, S1, S2 byte order while the spec and the generated field names (`iS2 qS2 iB qB iA qA iS1 qS1 seq`) already said ADC order, so a client decoding by name got the signals under the wrong names. Cause: the stream copies the hand-written `DisplacementPhasorEntry` onto the wire, and only its size was checked against the generated struct. The struct now has the ADC order and `svc_api_res_stream.c` asserts every field offset against `Api2TopicsPhasorStreamPush`, so the two cannot drift again. Found by cross-checking the new frames against a raw bulk capture channel by channel; the signal-diagnostics frame was already correct (all four channels matched the capture exactly). Use 0.13.1, not 0.13.0.

## 0.13.0 - 2026-11-10
**Wire-format change.** Every payload that carries the four ADC channels now follows the ADC channel order **S2 (ch0), B (ch1), A (ch2), S1 (ch3)**, like the bulk capture already did. Before, the phasor stream and the signal-diagnostics frame used B, A, S1, S2 (the internal batch order), which matched nothing else. Phasor stream (Topics 0x04): `iS2 qS2 iB qB iA qA iS1 qS1 seq` (was `iB qB iA qA iS1 qS1 iS2 qS2 seq`). Signal diagnostics (Topics 0x05): each group of four is `s2, b, a, s1` (was `b, a, s1, s2`) for `rms_mv_*`, `p2p_mv_*`, `phase_deg_*`. The frame sizes and the field names are unchanged, so a client that decodes by name (the generated `apiv3_defs.py`) keeps working; **a client that decodes these two payloads by position (the mobile app) must be updated.** `DisplacementSignalDiag` is indexed by `ADS131M04_CH_*` and the LCD diagnostics screen shows S2, B, A, S1. Payloads that carry only the two sensors as named results (tilt, precision, zero calibration, theoretical tilt) keep S1 then S2. Docs: `docs/channel-map.md` (now one order everywhere), the generated `docs/api-reference.md`, the operator manual and the wp10 notes. Not bench-compared against the app.

## 0.12.0 - 2026-11-10
The ADC PGA gains of the **excitation channels A (ADC ch2) and B (ADC ch1)** are now settings too: Calibrations `A_PGA` (0x4A) and `B_PGA` (0x4B), `GET` + `SET`, u8, a power of two 1..128, default 1, applied at once like `S1_PGA` / `S2_PGA` (0x48 / 0x49); together all four channel gains are readable and settable over the API. A and B may have different gains: the demod brings both to the larger one before forming D = A - B and each sensor's inverse gain absorbs that factor, so the reading is the same whatever the four gains are (host test with five gain combinations). Stored in the two bytes appended to the displacement page (EEPROM layout 0x000D); a 0x000C page is migrated with its calibrations (see 0.11.10; bench: all ten calibration values identical before and after the flash). `drv_ads131m04_set_pga()` takes the four gains; named constants `ADS131M04_CH_S2/B/A/S1` replace the bare channel indices in the displacement code and the GAIN1 register packing. The raw phasor stream and bulk capture stay undivided: a client divides each channel by its own gain (documented in `docs/channel-map.md`). Docs: `docs/wp8_ads131m04_adc.md`, generated `docs/api-reference.md`.

## 0.11.11 - 2026-11-10
Channel map made explicit, new `docs/channel-map.md` (confirmed against the schematic: **A = ADC ch2, B = ch1, S1 = ch3, S2 = ch0**; the drive to the sensor is D = A - B). The API spec described the signal-diagnostics frame as "channel order S2, B, A, S1" with "channel 2 = 0 deg by definition", but the firmware has sent it in **signal order B, A, S1, S2** with phase referenced to A - B at 90 deg since 0.10.66 (the LCD screen and the phasor stream use the same order) -- a likely reason the phone app mixed the channels up. The frame is unchanged on the wire; its fields are renamed `rms_mv_ch0..3 / p2p_mv_ch0..3 / phase_deg_ch0..3` -> `*_b, *_a, *_s1, *_s2` so a name can no longer be mistaken for an ADC channel, and the spec text now says which order each resource uses (diagnostics and phasor stream: signal order; bulk capture, ADC_MUX mask, GAIN1: ADC channel order). **Clients that read the diagnostics by field name (generated `apiv3_defs.py`) need the new names.**

## 0.11.10 - 2026-11-10
Settings and calibration writes are never answered "busy" any more (BLE, USB, UART alike). Found on the bench: every SET rewrote **all seven** EEPROM pages one chunk per scheduler tick, and a SET that arrived while that was running got `BUSY_RESOURCE` -- a host writing k, zero, phase and invert in a row was refused 7 times out of 8 (even with 0.5 s between writes one was still refused). Now `svc_storage_save_settings()` only queues the save and `svc_storage_update()` writes, in the background, just the pages whose bytes differ from what the EEPROM holds, coalescing everything changed meanwhile. A SET is applied and answered at once; the API handler no longer checks for a running save. `settings_save_failed` still latches on a failed write, and a failing chip is not retried by itself (the next save request tries again). `svc_storage_is_busy()` now means "a requested change is not yet on the chip" (the power-off wait uses it). Bench (fw 0.12.0 build): 40 back-to-back calibration writes over USB and 30 over BLE all OK, and a gain and a phase change survived a reboot.
Also: the displacement page can now migrate. The page layout is 0x000D (the A and B gain bytes are appended); a layout-0x000C page that is intact over its old size is kept (its calibrations survive) and rewritten, instead of being reseeded to defaults; a corrupt one is still reseeded. Host tests: queued/coalesced saves, only changed pages written, a burst never refused, migration (kept values, new fields at default, page rewritten, corrupt page reseeded), no retry storm.

## 0.11.9 - 2026-11-10
System STATE `service_mode` now means "writes are unlocked": it reads 1 whenever service mode is on **or** the gate is compiled off (`SERVICE_MODE_REQUIRED 0`, the current setting), so a host can rely on it to decide whether to offer calibration writes. Before, it showed only whether service mode had been entered at the instrument, which made a client think writes were blocked while the firmware accepted them (the phone app refused the phase calibration). The on-device indicator and menu row still show whether service mode was actually entered. Field text in `tools/api_spec.py`, `docs/api-reference.md` and `docs/api-v3-spec.md` section 5a updated. No wire or EEPROM-layout change; bench-checked: the flag reads 1 with service mode not entered.

## 0.11.8 - 2026-11-09 (includes the interim 0.11.7 build)
The ADC PGA gain of the two sensor channels is now a setting that can be read and written over the API: Calibrations `S1_PGA` (0x48, ADC ch3) and `S2_PGA` (0x49, ADC ch0), `GET` + `SET`, u8, a power of two from 1 to 128 (anything else answers `INVALID_PARAMETER`), **default 1**. Until now it was a compile-time constant of 16 (0.11.7 only changed that constant to 1; the sensors of the high-sensitivity home-made prototype clipped at 16). A SET persists and applies immediately: the demod is stopped, `GAIN1` is written and read back, the demod restarts clean. A stored 0 counts as the default, so no EEPROM version bump and the stored calibrations are untouched; the two values live in the displacement page's former pad bytes. `k` is stated for PGA 1 and the demod divides by the gain, so no recalibration is needed. New `drv_ads131m04_set_pga()`, `svc_displacement_apply_pga()`; documented in `docs/wp8_ads131m04_adc.md` and the generated `docs/api-reference.md`. Host tests (test_api_tables, test_svc_displacement) extended with the new stubs and pass. Bench (2026-11-09): GET returns the value, SET 3 is refused, SET 2 doubles the signal (the clipping then shows), SET 1 restores it.

## 0.11.6 - 2026-11-09
USB bulk captures work. `Services/svc_usb.c` now registers a TX-ready gate (`usb_tx_ready`, the same rule as BLE and UART), so the bulk sender no longer floods the 1 KiB TX ring: with no gate the drop-newest policy discarded about 96 % of the chunks (a 6144-sample capture ran 30 s and delivered ~270 samples). Bench: the full 6144 samples in 5.6 s over USB.
**Known limit, not changed:** the USB TX pump runs once per `task_usb_ms` (default 100 ms) and a report still in flight blocks the next one, so the stream is capped at ~10..16 reports/s (the phasor stream lost two thirds of its batches). With Settings `TASK_USB` (0x42) = 1 ms the phasor stream runs gapless at 40.9 batches/s. Pumping from the IN-complete callback would remove the dependence on that setting. No API, wire or EEPROM-layout change.

## 0.11.5 - 2026-10-26
Service mode is now a compile-time option, **off for now**: `SERVICE_MODE_REQUIRED` in `Config/config.h` (0 = the resources flagged `service=True` -- calibration writes, zero calibration, factory defaults, DFU, power / pin tests, the investigation commands -- are always open; 1 = they answer `SERVICE_MODE_REQUIRED` unless service mode was switched on at the instrument, as in 0.11.1..0.11.4). The menu row, the state flag, the idle timeout and the host tests of the mode itself are unchanged. Set it back to 1 before an instrument leaves the bench. No API, wire or EEPROM-layout change; build- and host-test-verified, not bench-tested.

## 0.11.4 - 2026-10-26
Charging now stops after `charge_full_timeout_min` minutes (default 60) at a SoC reading of **97 % or more** (new `CHARGE_FULL_SOC_PCT` in `Config/config.h`; was 100 %). With the instrument running from the battery the voltage-based SoC often tops out at 97..99 %, so the 100 % condition never fired. 97 % is about 4.13 V; the TP4056 STANDBY rule and the restart below `BATTERY_CHARGE_START` are unchanged. Settings row renamed "Charge stop @97%". No API opcode, payload or EEPROM-layout change. Host test added; build-verified, not bench-tested.

## 0.11.3 - 2026-10-25
Incremental robustness fixes (no API, wire, EEPROM-layout or measurement change). Host-test- and build-verified; not yet bench-tested.
- I2C1 bus clear at boot (`HAL_App/hal_i2c.c`): if SCL or SDA is low when `hal_i2c_init` runs (a reset in the middle of an EEPROM /
  BME280 transaction leaves the slave holding SDA), the bus is released with up to nine SCL clocks and a STOP before the peripheral is
  re-initialised. A healthy bus is not touched.
- `svc_storage_init` no longer reseeds a settings page it could not READ (`DRV_ERR_COMM`). Before, an unreadable chip was treated like
  a corrupt page: defaults were generated and written back, which overwrote the real calibrations whenever the write side still
  worked. Now the boot runs on the in-RAM defaults, the chip is left untouched, `settings_save_failed` is latched (SETTINGS screen
  marker, System HEALTH), an ERROR line goes to the debug log, and `svc_storage_save_settings` / `restore_defaults` refuse until the
  next reset. New host test `an_unreadable_chip_is_never_reseeded_overwritten_or_saved_to` (fails on the old code).
  Found on 2026-10-25 when an ST-Link reset left board 6DA98059 with a stuck I2C bus: it booted on defaults (see COORDINATION.md).

## 0.11.2 - 2026-10-24
Ported from the `drift-probe` branch (probe builds 0.10.90 / 0.10.91, API v2) to API v3. Build- and host-test-verified only.
- Settings `CHARGE_FULL_TIMEOUT` (0x53, minutes, default 60, 0 = off; SETTINGS screen row "Charge stop @100%"): charging stops
  after that long at a 100 % state-of-charge reading or at the charger's STANDBY signal, whichever comes first (the TP4056
  cannot end a charge while the instrument draws its own current). EEPROM battery page version 0x0006 reseeds that page
  (thresholds, ADC scale / offset, auto power-off): save the settings with `Testing/2026-10-08_drift_investigation/settings_backup.py`
  before flashing and restore them afterwards.
- Commands `EXCITATION` (0x74: AD9833 on/off and PHASE0), `ADC_MUX` (0x75: ADS131M04 input mux per channel), `EXC_FREQ`
  (0x77: excitation frequency fs/n, n = 4..16 samples per carrier cycle, demodulation follows). All three need service mode and
  are not persistent. The amplitude-fault check and the signal diagnostics scale with n; calibrations are valid at n = 8 only.
- Commands `TILT_DEMOD` start / stop is no longer refused while the phasor stream is active (the stream is only a tap).
- Drift-investigation tooling restored under `Testing/2026-10-08_drift_investigation/`, ported to `apiv3.py`.

## Unreleased
- `svc_displacement.c` (1200 lines) split into the acquisition / demodulation core, `svc_disp_capture.c` (bulk raw capture),
  `svc_disp_phasor_stream.c` and `svc_disp_procedures.c` (flip calibration, precision measurement); no behaviour change.
  Host tests for the two procedures added.
- API names only (opcodes unchanged): resource `DISPLACEMENT` -> `TILT_DEMOD` (Commands 0x50, Diagnostics 0x02), System
  state field `displacement_running` -> `demod_running`.

- LIVE screen: heading "Tilt (mm per m)" and the readings carry the unit `mm/m` (they said "Displacement" and `mm`). Visual
  check on the display pending.
- `docs/operator-manual.md` (draft); `docs/firmware-module-graph.html` (generated by `tools/depgraph.py`): architecture overview for newcomers.

## 0.11.1 - 2026-10-08
- Service mode: calibration writes and the maintenance commands (zero calibration, factory defaults, DFU, power / pin test,
  rails, fault test) need it. It can only be entered on the instrument; it ends by menu, `SERVICE_END`, 10 minutes without API
  requests, or sleep.
- Supervised window watchdog (WWDG refreshed from SysTick while the main loop makes progress, 3 s stall limit).
- HardFault capture: PC / LR / xPSR / SP kept across the reset, logged at the next boot, in System HEALTH.
- New `FAULT_TEST` command to exercise the above on the bench.
- The firmware reports a build id (git commit, `-dirty`) in IDENTITY and on the STATUS screen.
- Host tests added for the battery policy, EEPROM storage (against a fake chip) and the BME280 compensation.

## 0.11.0 - 2026-10-08
- **API v3**: the whole API regrouped and generated from `tools/api_spec.py` (`docs/api-v3-spec.md`, `docs/api-reference.md`).
  Breaking: new category layout, state-changing IDs >= 0x40, new Python client `apiv3.py`.
- New: System HEALTH, RTC sub-second and trim (`RTC_TRIM`), Topics LIVE, Procedures, event subscriptions, USB multi-report
  fragmentation, power off / reboot / factory defaults / rail commands.
- The "Display rate" setting (it paced nothing) was removed; the scheduler EEPROM page is reseeded once.

## 0.10.72 - 2026-10-07
- Review clean-up phases 0-5: bug fixes (idempotent start, exclusivity, topic buffer overflow, ADC frame ring off-by-one,
  quality floor readiness / re-seed, zero-cal range check), removal of the output ring, the 8-batch moving average, the
  per-batch quality flag and the bulk phasor log; the per-batch maths moved to `Math/math_displacement` and
  `Math/math_quality` with a replay test on recorded data.

## 0.10.71 and earlier
See `git log` and `docs/wp10_displacement.md` (displacement / tilt history), `docs/signal_processing.pdf` (theory and the
analysis of the 19 h recording).
