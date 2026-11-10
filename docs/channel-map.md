# Channel map (REV B) -- one place for the signal names and the ADC channels

Confirmed against the schematic on 2026-11-10. If this table and a comment elsewhere disagree, this table wins; fix the comment.

## The four signals and their ADC channels

| Signal | What it is | ADS131M04 channel | PGA setting (API Calibrations) | Default PGA |
|---|---|---|---|---|
| **A** | excitation A, the positive drive | **ch2** | `A_PGA` 0x4A | 1 |
| **B** | excitation B, the negative drive (opposite phase to A, about the same amplitude) | **ch1** | `B_PGA` 0x4B | 1 |
| **S1** | sensor 1 | **ch3** | `S1_PGA` 0x48 | 1 |
| **S2** | sensor 2 | **ch0** | `S2_PGA` 0x49 | 1 |

The drive that reaches the sensor is **D = A - B** (about 2 x A). Every tilt reading is the in-phase part of **S / D** (after the
gains are divided out), so A and B are reference signals and S1 / S2 are the measurements.

Physical ADC channels, in order: **ch0 = S2, ch1 = B, ch2 = A, ch3 = S1.** In code: `ADS131M04_CH_S2/B/A/S1`
(`Drivers_App/drv_ads131m04.h`). The REV A branch used a different mapping (ch0 = B, ch1 = A, ch2 = S1, ch3 = S2); it does not apply.

## Two different orders are in use -- check which one a field uses

| Order | Used by |
|---|---|
| **ADC channel order: S2, B, A, S1** (= ch0, ch1, ch2, ch3) | Bulk raw-ADC capture (the four 24-bit values of a sample, `ch0..ch3`); Commands `ADC_MUX` (`channel_mask`, bit n = ADC channel n); the ADS131M04 `GAIN1` register (CH0..CH3 fields); the `Raw data` register read-back |
| **Signal order: B, A, S1, S2** | the phasor stream (`iB qB iA qA iS1 qS1 iS2 qS2`); the **signal-diagnostics** frame (Topics `SIGNAL_DIAG`: the four `rms_mv_*`, `p2p_mv_*`, `phase_deg_*` values are B, A, S1, S2); the LCD diagnostics screen; the internal `BatchSums` |

So **index 0 of a diagnostics frame is B, not ADC channel 0.** A client must not label diagnostics values by ADC channel number. The
diagnostics field names are `*_b`, `*_a`, `*_s1`, `*_s2` for exactly this reason (they were `*_ch0..ch3` before fw 0.11.11,
which suggested the ADC channels; the wire layout did not change).

## Phase reference in the diagnostics

`phase_deg_*` is referenced so that **D = A - B sits at 90 deg**: A reads about +90 deg, B about 270 deg (opposite), and a sensor reads
its own angle against that. (Earlier documents said "channel 2 = 0 deg by definition"; that was the REV A convention.)

## Gains

Each of the four channels has its own PGA (1, 2, 4 ... 128), readable and settable over the API (`Calibrations` 0x48 .. 0x4B). The ADC
input range is +-1.2 V / PGA. The demod refers every phasor to the ADC pin before forming S / D, so the gains need no recalibration
of `k` (stated for PGA 1). The raw phasor stream and bulk capture are **not** divided by the gains: divide each channel by its own PGA.
