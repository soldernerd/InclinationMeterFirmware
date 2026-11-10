# Channel map (REV B) -- one place for the signal names and the ADC channels

Confirmed against the schematic on 2026-11-10. If this table and a comment elsewhere disagree, this table wins; fix the comment.

## The four signals and their ADC channels

| Signal | What it is | ADS131M04 channel | PGA setting (API Calibrations) | Default PGA |
|---|---|---|---|---|
| **S2** | sensor 2 | **ch0** | `S2_PGA` 0x49 | 1 |
| **B** | excitation B, the negative drive (opposite phase to A, about the same amplitude) | **ch1** | `B_PGA` 0x4B | 1 |
| **A** | excitation A, the positive drive | **ch2** | `A_PGA` 0x4A | 1 |
| **S1** | sensor 1 | **ch3** | `S1_PGA` 0x48 | 1 |

The drive that reaches the sensor is **D = A - B** (about 2 x A). Every tilt reading is the in-phase part of **S / D** (after the
gains are divided out), so A and B are reference signals and S1 / S2 are the measurements.

Physical ADC channels, in order: **ch0 = S2, ch1 = B, ch2 = A, ch3 = S1.** In code: `ADS131M04_CH_S2/B/A/S1`
(`Drivers_App/drv_ads131m04.h`). The REV A branch used a different mapping (ch0 = B, ch1 = A, ch2 = S1, ch3 = S2); it does not apply.

## One order everywhere: ADC channel order, S2, B, A, S1

Since **fw 0.13.0 every per-channel payload follows the ADC channels: S2 (ch0), B (ch1), A (ch2), S1 (ch3).** That covers the bulk raw-ADC
capture (the four 24-bit values of a sample), the phasor stream (`iS2 qS2 iB qB iA qA iS1 qS1`), the signal-diagnostics frame (every group
of four values: `*_s2, *_b, *_a, *_s1`), Commands `ADC_MUX` (`channel_mask`, bit n = ADC channel n), the ADS131M04 `GAIN1` register fields,
the `Raw data` register read-back and the LCD diagnostics screen. A client can therefore index every one of them by the ADC channel
number. In code: `ADS131M04_CH_S2/B/A/S1` (`Drivers_App/drv_ads131m04.h`).

Payloads that carry only the two **sensors** as named results (the tilt readings, the precision and zero-calibration results, the theoretical
tilt) list S1 then S2; they are not ADC channel readings and are not affected.

Before 0.13.0 the phasor stream and the diagnostics were in B, A, S1, S2 (the order of the demod's internal `BatchSums`, which is
unchanged and not on the wire). **That was a wire-format change: a client that decodes these two payloads by position must be updated;
one that decodes by field name (the generated `apiv3_defs.py`) is unaffected** because the names did not change.

## For app implementers: labels, order and exact payload layouts

**Use these labels and this order, everywhere, in every app.** Do not invent another order and do not number the signals yourself.

| Index (= ADC channel) | Label to show | Long name | Role |
|---|---|---|---|
| **0** | **S2** | Sensor 2 | measurement |
| **1** | **B** | Excitation B | reference, opposite phase to A |
| **2** | **A** | Excitation A | reference |
| **3** | **S1** | Sensor 1 | measurement |

The index in the first column is the **ADC channel number** (`ch0` .. `ch3` on the ADS131M04) **and** the position of the signal in every
per-channel payload below. If you show a channel number next to a label, show this one: `S2` = channel 0, `B` = channel 1, `A` = channel 2,
`S1` = channel 3. Never show "channel 0" next to A, and never put A before B.

### Exact layouts (all numbers little-endian)

**Signal diagnostics**, Topics `SIGNAL_DIAG` (0x05), GET or SUBSCRIBE; 56 bytes = 14 x float32:

| Offset | Field | Signal | | Offset | Field | Signal | | Offset | Field | Signal |
|---|---|---|---|---|---|---|---|---|---|---|
| 0 | `rms_mv_s2` | S2 | | 16 | `p2p_mv_s2` | S2 | | 32 | `phase_deg_s2` | S2 |
| 4 | `rms_mv_b` | B | | 20 | `p2p_mv_b` | B | | 36 | `phase_deg_b` | B |
| 8 | `rms_mv_a` | A | | 24 | `p2p_mv_a` | A | | 40 | `phase_deg_a` | A |
| 12 | `rms_mv_s1` | S1 | | 28 | `p2p_mv_s1` | S1 | | 44 | `phase_deg_s1` | S1 |

then 48 `theoretical_tilt_s1` (mm/m, from S1) and 52 `theoretical_tilt_s2` (mm/m, from S2). Amplitudes are at the ADC pin (already divided by
the channel's gain), in millivolts. Phase is in degrees, referenced so that A - B sits at 90 deg: **A reads about 90, B about 270**.

**Phasor stream**, Topics `PHASOR_STREAM` (0x04), push packets `[status][issue_seq][page 0][34 data bytes]`; the 34 data bytes are:

| Offset | 0 | 4 | 8 | 12 | 16 | 20 | 24 | 28 | 32 |
|---|---|---|---|---|---|---|---|---|---|
| Field | `iS2` | `qS2` | `iB` | `qB` | `iA` | `qA` | `iS1` | `qS1` | `seq` (u16) |
| Signal | S2 | S2 | B | B | A | A | S1 | S1 | batch counter |

Each `i`/`q` pair is one signal's in-phase and quadrature sum over a 64-cycle batch, as float32, **raw: not divided by the channel gains**.

**Bulk raw-ADC capture**, Bulk `RAW_ADC` (0x00): chunks `[status][page][10 samples]`, each sample 12 bytes = four signed 24-bit values:
bytes 0..2 = **S2** (ch0), 3..5 = **B** (ch1), 6..8 = **A** (ch2), 9..11 = **S1** (ch3). Raw ADC codes, not divided by the gains.

**Commands `ADC_MUX`** (0x75), `channel_mask`: bit 0 = S2, bit 1 = B, bit 2 = A, bit 3 = S1.

**Gains** are named resources, not a list: `S1_PGA` 0x48, `S2_PGA` 0x49, `A_PGA` 0x4A, `B_PGA` 0x4B (their ids are fixed, not in channel order).

**Payloads about the two sensors only** (the tilt readings, the live/precision/zero-calibration results, the `theoretical_tilt_*` pair) are
named results and list **S1 then S2**. They are not per-channel readings; do not mix them up with the four-channel payloads above.

### Checklist before you ship a display

1. The four labels appear exactly as `S2, B, A, S1`, in that order, with the channel numbers 0, 1, 2, 3 (or none).
2. A and B are drawn as the **references** (about equal amplitude, **opposite phase**: about 90 and 270 deg); S1 and S2 as the **measurements**.
3. If a diagnostics view shows two large, equal-amplitude, opposite-phase signals labelled S1 / S2 (or A / S1), the labels are wrong.
4. Amplitudes are used as delivered (at the ADC pin). If you scale raw phasors or bulk codes yourself, divide each channel by **its own** gain.
5. Decode by **position within the layout above** or by the field names; never by "the first two are references".
6. Firmware older than **0.13.0** sent the phasor stream and the diagnostics in B, A, S1, S2. If you must support it, read the firmware version
   from System `IDENTITY` and use the old order only below 0.13.0; the bulk capture, `ADC_MUX` and the gains were never affected.

## Phase reference in the diagnostics

`phase_deg_*` is referenced so that **D = A - B sits at 90 deg**: A reads about +90 deg, B about 270 deg (opposite), and a sensor reads
its own angle against that. (Earlier documents said "channel 2 = 0 deg by definition"; that was the REV A convention.)

## Gains

Each of the four channels has its own PGA (1, 2, 4 ... 128), readable and settable over the API (`Calibrations` 0x48 .. 0x4B). The ADC
input range is +-1.2 V / PGA. The demod refers every phasor to the ADC pin before forming S / D, so the gains need no recalibration
of `k` (stated for PGA 1). The raw phasor stream and bulk capture are **not** divided by the gains: divide each channel by its own PGA.
