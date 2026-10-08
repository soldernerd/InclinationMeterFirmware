# Operator and service manual

For the person using the instrument and the person setting it up. Firmware 0.11.x. **Draft: written from the firmware, not yet
checked against the hardware** -- every procedure that says *(unverified)* has not been run end to end on a board. The
protocol for host software is in [api-v3-spec.md](api-v3-spec.md) and [api-reference.md](api-reference.md); the measurement
theory is in [signal_processing.pdf](signal_processing.pdf).

## 1. What it measures

Two tilt sensor heads, S1 and S2, read through an AC bridge. The display shows each head's tilt in **mm per metre** (mm/m,
i.e. the height difference in mm over a 1 m baseline) and their difference **Diff = S1 - S2**. The difference cancels what
both heads see alike (most of the electronics' drift), which is why it is the more repeatable number. Readings update about
four times a second and are smoothed over about 0.6 s.

## 2. Controls

There are two rotary knobs with push buttons.

| Knob | Turn | Press |
|---|---|---|
| **Left** | next / previous screen (not while editing a setting) | back to the previous screen; cancels an edit |
| **Right** | on SETTINGS: move the cursor, or change the value being edited | LIVE: start a precision measurement. SETTINGS: edit the row, press again to confirm |

Any knob press wakes the instrument from sleep. Turning does not.

## 3. Screens

**LIVE** -- the readings. `S1`, `S2` and `Diff` in mm/m with four decimals. A trailing ` !` means the last 0.6 s window looked
disturbed (a bump, footsteps, a machine starting): the value is still shown but do not trust it. The bottom line shows
temperature, battery state of charge and voltage, or the precision measurement while one runs.

**STATUS** -- firmware version and build id (git commit; `-dirty` = built from uncommitted changes), serial number, whether the
settings loaded (`SAVE FAILED` means the last write to the EEPROM did not stick), BLE and USB state, uptime, date and time,
and the environment sensor (temperature, pressure, humidity).

**SETTINGS** -- rows, top to bottom:

| Row | What it does |
|---|---|
| Battery critical | voltage below which the instrument powers itself off (mV) |
| Auto power-off | idle seconds before sleeping; 0 = never |
| Force charge | charge now regardless of the battery level (needs USB power) |
| Zero cal both / S1 / S2 | the flip calibration, section 6 |
| Service mode | switches service mode on or off, section 8 |
| Reboot to DFU | puts the instrument in the ROM bootloader for flashing. **It stays there until reflashed**, section 9 |
| Power off | sleep now |

**DIAGNOSTICS** -- amplitude and phase of the four raw channels (B, A, S1, S2) and the tilt each sensor amplitude would
correspond to by the Wyler handbook figure. For setting up and troubleshooting, not for measuring.

## 4. Measuring

1. Switch on (press a knob). The tilt measurement starts by itself a moment after boot. `-- not running --` on LIVE means it is
   stopped (a host stopped it); it can be started from the API.
2. Put the instrument on the surface, wait for the first reading (about a second) and for the temperature to settle. The
   electronics drift with temperature by a few tens of micrometres per degree, so let a cold instrument warm up and keep it
   out of draughts and sunlight. Avoid measuring while it charges (more noise, more jumps).
3. Read `Diff` for the best repeatability; `S1` and `S2` individually carry the common drift.

**Precision measurement** (press the right knob on LIVE). The instrument looks for the first quiet 2 s window and reports the
value of S1, S2 and Diff from it. `Precision n/81...` shows the progress, `disturbed` that the table is not quiet yet. If no
quiet window appears within 5 s it reports an error and no value -- repeat when the surface is calm.

## 5. Power and charging

* Charge by USB. Charging starts when the battery is below 3.70 V; `Force charge` overrides that once.
* The instrument warns at 3.60 V and powers off at 3.40 V (the setting `Battery critical`). It does not power off while USB
  power is present.
* It sleeps after `Auto power-off` seconds without knob activity. It does not sleep while USB or BLE is connected, while
  charging, or while a measurement is running.
* Sleeping is a full restart on wake: the clock keeps running, everything else starts fresh.

## 6. Calibration

All calibration values are per instrument, kept in its own EEPROM, and can only be **written in service mode** (section 8);
reading is always possible. Order matters: **invert, phase, k, then zero**, because each step assumes the earlier ones.

### Zero (flip calibration) -- from the menu or the API

Corrects the level point of each head. A real surface tilt reverses its sign when the instrument is turned 180 degrees, the
instrument's own zero error does not, so the average of the two readings is the zero error.

1. Put the instrument on a stable surface, a mark on the side facing you. Keep the table undisturbed.
2. SETTINGS > *Zero cal both* (or *S1* / *S2* for one head). Press, press again to confirm. `step 1: n/64` counts up (about 1.6 s).
3. Turn the instrument **180 degrees in place** (same spot, same surface).
4. Press the same row again: `step 2`. When it finishes the new zero is stored. Repeat once and check that two runs agree.

### Sign (invert)

If "front up" should read positive and does not, set `S1_INVERT` / `S2_INVERT` (API Calibrations 0x43 / 0x47). It flips only
the reported reading.

### Phase *(unverified)*

Each head's signal is rotated by its calibrated phase so that tilt falls entirely in the in-phase part. `S1_PHASE`,
`S2_PHASE`, in 0.01 degree. A wrong phase shows as a reading that changes sign or size with temperature and as a large
quadrature residual. The shipped defaults come from the principal axis of 19 hours of recorded phasors
(`docs/signal_processing.pdf`); the procedure to redo it on a given unit is to record the phasor stream
(API Topics PHASOR_STREAM) while introducing a known tilt and take the phase of the signal relative to `A - B`.

### Sensitivity k *(unverified)*

`S1_K`, `S2_K` convert the measured ratio into mm/m: tilt = (ratio - zero) / k. The default is derived from the sensor's
nominal figure (20 uV RMS per um/m). To calibrate on a granite plate or with a shim: put a known shim of height h under one end
of a baseline of length L (tilt = h / L in mm/m), note the change in the ratio, and set k = change in ratio / tilt. A single
sheet of 80 g/m2 paper (about 0.1 mm) is a usable first check. Use the precision measurement or the 4-decimal LIVE value, and
repeat both ways round.

### Battery voltage

`VBAT_SCALE_NUM`, `VBAT_SCALE_DEN`, `VBAT_OFFSET` correct the divider; calibrate against a meter at two voltages
(`PythonTestCode/set_vbat_scale.py`).

### Clock *(unverified)*

Set the time (API System RTC, or the companion app). To trim: note the PC time and the instrument's time including the
sub-second counter, wait 12 hours or more, compare. A clock that lost x ppm needs `RTC_TRIM = +x * 10` (unit 0.1 ppm, positive
runs faster). The step is about 0.95 ppm. Without a backup battery the time is lost when the battery is removed.

## 7. Connections

* **USB**: custom HID device, no driver needed; also charges. **BLE**: advertises as `Leveltronic_<MAC>`; no pairing is
  required. **UART**: the debug header, 115200 8N1, the connection a script reaches with `pyserial`.
* Host side: `PythonTestCode/apiv3.py` (client) and the scripts around it.

## 8. Service mode

Gates everything that can damage a calibration or leave the instrument in a state a user cannot get out of: all calibration
writes, the flip calibration command, factory defaults, reboot to DFU, rail and pin tests, the fault test.

* Enter: SETTINGS > *Service mode*, press, press again. It cannot be switched on from the API.
* The row then shows `[ON m:ss]`, the time left until it ends by itself after 10 minutes without a request.
* It ends by the same menu row, by the API command `SERVICE_END`, after 10 minutes of silence, and whenever the instrument
  goes to sleep.
* Without it, a gated call answers `SERVICE_MODE_REQUIRED`; reading values still works.

## 9. Recovery

| Situation | What to do |
|---|---|
| Screen frozen, no reaction | The watchdog restarts a stuck instrument within seconds. If it does not, pull the reset line (NRST, pin 8 of the debug header) low once. |
| Reboots in a loop | The last fault is in System HEALTH (`last_fault_kind`, `last_fault_pc`, `last_fault_lr`). Look the address up with `arm-none-eabi-addr2line -e <elf> <pc>`; reflash a known good firmware. |
| *Reboot to DFU* was used, or the app will not start | The instrument is in the ROM bootloader (USB DFU, 0483:DF11) and stays there. Flash with `dfu_flash.ps1`, which also restores `nBOOT0`. Some hosts need an NRST tap or a USB replug to see the device. |
| `Settings: SAVE FAILED` | The EEPROM did not accept the last write. Power-cycle and try again; if it persists the EEPROM or the I2C bus needs a look. |
| Wrong calibration after experiments | Service mode > API `FACTORY_DEFAULTS` resets **everything** (also the tilt calibration) to the compiled defaults. |
| Readings jump or drift | Is it charging? Warm-up not finished? Draught or sun on one head? Swap the heads' cables to see whether the noise follows the sensor unit or the channel. |

## 10. Technical data

| | |
|---|---|
| Processor | STM32G0B1, 64 MHz |
| Excitation | AD9833, 2604 Hz sine |
| ADC | ADS131M04, 4 channels simultaneous, 24 bit, 20.83 kSps; gain 16 on S1 and S2 |
| Display | Sharp LS027B7DH01 memory LCD, 400 x 240 |
| Update rate | 40.7 batches/s internal, 4.07/s displayed |
| Environment sensor | BME280 (temperature, pressure, humidity) |
| Storage | 24LC256 EEPROM, per-page CRC and version |
| Supervision | window watchdog (3 s stall limit), HardFault capture |
