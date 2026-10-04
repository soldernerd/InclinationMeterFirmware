# Findings — periodic raw-ADC bulk capture, 24 h

**Run:** 2026-09-30 ~21:36 -> 2026-10-01 21:36, fw 0.10.62, board 6DA08073.
**Data:** 2880 captures, 6144 samples x 4 ch each (0.295 s), every 30 s. Zero gaps, zero
truncated captures. Raw file `data/bulk_adc_log.bin` (203 MB, not committed).
**Analysis code:** `analysis/` (see "Reproducing" at the end). Graphs: `analysis/graphs/`.

Channel map (ADS131M04, from `docs/signal_processing.tex` Section 10 / `svc_displacement.c`):
**ch0 = S2, ch1 = B, ch2 = A, ch3 = S1.** A and B are the excitation measurements
(PGA 1, ~4.4 M counts, ~53 % of full scale); S1/S2 are the Wyler sensors (PGA 16).
Wall clock is approximate (run start taken from file times); "night" below =
23:00-07:00 (nobody in the shop), "day" = the rest of the run.

Phasors here are `sum x[n] e^{-j 2 pi n/8}` over one 8-sample cycle (the firmware uses
`I + jQ`, i.e. the complex conjugate). Magnitudes and `Re(S/(A-B))` are identical.

## 1. The first cycle of every capture can be corrupt (bulk path only)

In 284 of 2880 captures (9.9 %) the **first sample** is stale (it repeats an earlier
conversion). It only ever affects cycle 0, on all four channels, and the amplitude error
takes four discrete levels (+62 000, -18 000, -38 000, -125 000 ppm of the A/B amplitude).
It inflated the within-capture amplitude std of A/B from ~213 to ~1155 counts and the
excess kurtosis from ~0 to ~17 000 until cycle 0 was dropped. **All results below drop
cycle 0.** Likely cause: the first frame read after `drv_ads131m04_start()` is a
conversion latched while the ADC was idle. Impact on the live demod is small (once per
start), but any start/stop-based measurement should discard the first cycle.

## 2. A / B (excitation) channels: clean, Gaussian, 1/f-limited

After dropping cycle 0 the per-cycle amplitude and phase of A and B are Gaussian
(excess kurtosis ~0.05, skew ~ -0.14), flat in day vs night, white phase noise
(0.043 mdeg/sqrt(Hz)), 1/f amplitude noise. Median, trimmed mean, Huber and sigma-clipped
mean are all within ~1 % of the plain mean: **robust location estimators buy nothing on
A/B.** Averaging more cycles within a capture does not help either: the 30 s scatter of the
capture mean (~35 ppm) is ~20x the white-noise standard error (1.7 ppm) because the noise
is 1/f.

Common-mode rejection does help: A and B are 0.995 correlated; `ln(|A|/|B|)` has 7.8 ppm
30 s scatter (4.5x better) and the A-B phase difference 0.17 mdeg vs ~0.6 mdeg for one
channel.

There is a large, real event at **~04:33**: A and B phase drop ~200 mdeg within ~20
minutes (then recover over ~15 h), the A/B ratio swings ~150 ppm, S1/S2 shift too. Not
noise (within-capture std unchanged); looks like a power/thermal event. Not investigated.

## 3. Sensor channels (S1, S2): a ~20 Hz resonance dominates, and it is day/night dependent

S1/S2 have 5-30x larger relative noise than A/B and it is *not* white. The spectrum of the
sensor reading (`Re S/(k(A-B))`) shows one dominant peak:

| | night | day |
|---|---|---|
| peak frequency | 20.5 Hz | 19.9 Hz (per-capture median; 18-21.5 Hz p10-p90, stable over 24 h) |
| peak / HF floor (S1) | 26 dB | 37 dB |
| peak / HF floor (S2) | 31 dB | 42 dB |
| S1-S2 coherence at 20 Hz | 0.99, phase ~9 deg | 1.00, phase ~7 deg |
| FWHM | 5.4-5.9 Hz | 5.7-5.9 Hz (**window-limited**: Hann on 0.295 s resolves ~4.9 Hz; true Q >~ 4) |

Consistent with the pendulum's natural frequency: it is coherent between the two sensors,
fixed in frequency, ringing in bursts (the burst waveforms in `graphs/8_bursts_ch0_ch3.png`
are decaying ~20 Hz oscillations), and its amplitude follows shop activity (~10 dB higher
by day, onset at ~07:00). A weak second feature near 40 Hz (-25 dB relative) exists.
Quiet-window statistics: S1 30 s scatter of the capture mean 0.068 um vs 0.143 um by day,
S2 0.118 vs 0.276 um (nominal firmware um, default d0, cal_mult = 1).

Per-cycle statistics of S1/S2 (what the user's original question asked for): within-capture
the amplitude std is 2.3-3.4 k counts at night and 4.5-5 k by day (ch3/ch0), skew ~0 at
night and up to +2 in vibration bursts, excess kurtosis median ~0 with sporadic bursts up to
~14. These bursts are slow (50-150 cycles) ring-down envelopes, **not** single-cycle
outliers, so per-cycle clipping would remove real signal. All per-capture moments are in
`analysis/capture_stats.csv` (126 columns).

## 4. How the current processing handles this (read from `svc_displacement.c` / signal_processing.tex)

Chain: 8-sample cycle phasors -> 64-cycle coherent batches (24.6 ms, 40.7 Hz) -> complex
ratio `x' = S/(k(A-B))` -> `2 d0 Re x'` -> boxcar MA of 8 batches (= one 512-cycle boxcar, 197 ms).

* The batch stream is sampled at 40.7 Hz, so its **Nyquist frequency is 20.35 Hz =
  f_DATA/1024 - exactly where the pendulum is**. The 64-cycle boxcar passes 19.9 Hz at
  only -3.7 dB, so the pendulum arrives almost unattenuated and folded to ~Nyquist, which
  the MA8 then cancels (it has an exact null at 20.35 Hz, -72 dB).
* That null is narrow. At 17-19 Hz and 21-24 Hz the MA8 only attenuates 21-30 dB (first
  sidelobe -21 dB), and the pendulum spans 18-21.5 Hz with Q~4. The leakage of the
  pendulum through the boxcar sidelobes is what sets the scatter by day
  (`graphs/12_filter_responses.png`).
* The residual-step quality flag fires 3.6-5 % of batches at night and 7-8 % by day
  (a Gaussian would give ~1.6 %); its rate per tercile of 20 Hz-band power is
  1.6 / 2.2 / 13.8 %, so it *does* track the pendulum. Flagged batches have rms deviation
  3.2 um vs 0.64 um for kept batches.

## 5. Strategy: tested on the real captures

30 s capture-to-capture scatter of the estimate (um, nominal units, lower is better),
columns night S1 / S2 / S1-S2, then day S1 / S2 / S1-S2. 767 cycles per capture, so these
are *capture-level* numbers (0.3 s integration), not the 1.6 s precision measurement.

| estimator | night | day |
|---|---|---|
| rect mean, 704 cycles (11 batches, as now) | 0.071 0.124 0.066 | 0.141 0.265 0.179 |
| rect mean, 5 pendulum periods (654 cyc) | 0.070 0.126 0.071 | 0.142 0.275 0.188 |
| **Hann window, 767 cyc** | 0.067 0.101 0.048 | **0.125 0.128 0.075** |
| **triangular (= 2 cascaded boxcars)** | 0.066 0.100 0.047 | **0.124 0.129 0.077** |
| regress DC + sines 12-28 Hz | 0.065 0.098 0.045 | 0.124 0.131 0.075 |
| flag-filtered mean (precision-style) | 0.191 0.510 0.346 | 0.626 1.879 1.331 |
| median of 11 batches | 0.182 0.509 0.343 | 0.415 1.268 0.909 |

* **An apodised (non-rectangular) window is the single clear win**: daytime S2 and S1-S2
  scatter drop 2.1x and 2.4x, night ~10-30 %. A regression that explicitly fits and
  removes 12-28 Hz sines gives the same result, confirming it is the pendulum leakage.
  Hann, Tukey, Blackman, triangular all perform alike.
* **Choosing an integer number of pendulum periods does not help** (the pendulum is
  broadband, 18-21.5 Hz, so any single null misses it).
* **Dropping individual flagged batches makes the estimate 3-10x worse.** A pendulum
  sampled at the batch Nyquist alternates sign batch to batch; the plain MA cancels it, but
  removing the high-amplitude batches unbalances the alternation (error vs a Hann
  "truth": 0.073 um with all batches, 0.527 um with the flagged ones removed). Caveat:
  emulated on 11-batch (0.27 s) captures; the real precision measurement averages 64
  batches, where the penalty should be smaller. Not yet measured, but worth testing
  before trusting `precision measurement` in vibrating conditions.
* A segment-wise inverse-variance weighting and a segment median did not help.
* S2 sees **3.1x the pendulum amplitude of S1** in the nominal-d0 units (both day and
  night), so the S1-S2 differential does *not* cancel the pendulum with unit gain
  (it adds 6.6 dB); with a fitted gain of 0.32 it cancels it by 15-17 dB. That gain is
  not the tilt scale ratio, so it can only be used as a vibration regressor, not as the
  measurement. (S2 is also generally the noisier sensor; consistent with earlier unit-swap
  findings. The cause of the 3x is unexplained: sensitivity calibration only differs 1.17x.)
* Ratiometric division by (A-B) does not change the reading noise at any timescale
  tested, because the reading is a tiny deviation from null (|x'| ~ 5e-5), so a 35 ppm
  excitation wander is negligible against sensor noise. It matters for scale only.

## 6. Recommendations (ordered by value / effort)

1. **Replace the 8-batch boxcar by a smoother with real stopband** (two cascaded MA8, i.e.
   triangular, 15 batches, ~0.37 s; or Hann/3-stage cascade). Triangular puts 15-26 Hz at
   <= -41 dB (vs -21 dB) at the cost of latency (~0.37 s vs 0.1 s) and bandwidth
   (-3 dB at 1.7 Hz vs 2.25 Hz; -1.0 dB vs -0.6 dB at 1 Hz). Zero new sensors, two lines of firmware. Expected
   daytime scatter reduction ~2x (S2, S1-S2). Same change for the precision measurement
   (apply the window to the 64-batch average).
2. **Do not drop batches by the residual-step flag when averaging an oscillation.**
   Use the flag as a *window-level* verdict instead (accept / extend / warn the user "too
   much vibration"), or weight smoothly. The flag already correlates with vibration (rate
   1.6 -> 13.8 % across terciles), so as a vibration meter it is good.
3. **Add an explicit vibration indicator** (15-26 Hz band power, or just the alternating
   batch-to-batch component, which is nearly free at 40.7 Hz). Show "shaky" on the LIVE screen
   and let the precision measurement lengthen its window automatically when it is high
   (day vs night differ by ~10 dB, so a fixed averaging time is wrong for one of them).
4. **Always discard the first cycle after `drv_ads131m04_start()`** (and for bulk captures).
5. **Do not design a narrow notch.** Frequency spread 18-21.5 Hz and Q~4 make a notch
   either miss or ring; a wide lowpass at ~2-3 Hz (needed anyway for a quasi-static tilt
   reading) with high stopband attenuation does the job. A 40.7 Hz batch rate is fine for
   that, but note the 64-cycle boxcar plus 40.7 Hz output rate folds the pendulum to
   Nyquist; moving to 32-cycle batches (81 Hz) would put it at a quarter of the sample
   rate instead, at a CPU cost (config.h notes the margin issue).
6. Optional later: an adaptive/regression cancellation of S1's pendulum band using S2 as
   the reference (15-17 dB available), only if the window change is not enough.
7. Check on the next hardware spin whether the 3x S2/S1 pendulum ratio is mounting/
   sensor related; if both sensors saw the same acceleration, S1-S2 would cancel the
   vibration and the day/night difference would largely vanish.

## 7. Not verified / caveats

* Everything is emulated offline from raw captures; **no firmware change was made or
  tested.** Capture length (0.295 s) limits what can be said about >1 s averaging.
* um values use default d0 (439/495 mm), k = 480, cal_mult = 1 -- relative comparisons are
  solid, absolute scale is not.
* The "pendulum natural frequency" attribution is consistent with the data but not proven
  (a mount resonance would look the same); a tap test would settle it. The peak
  frequency differs slightly night (20.5) vs day (19.9), possibly amplitude-dependent.
* The 04:33 event and the 07:00 -> evening noise onset were not correlated with other
  logs.

## 8. Follow-up analysis (same data): where the per-cycle fluctuation comes from, 80->64 trimming, raw spectrum

Scripts: `analysis/varbands.py`, `trim80.py`, `rawspec.py`, `rawspec2.py`, `plots3.py`; graphs 14-16.

**Per-cycle fluctuation of the sensor phasors.** Rotating S into the A-B frame (Re = in-phase =
the tilt axis, Im = quadrature), per-cycle std in ADC counts:

| | white floor (>300 Hz) | night, Re / Im | day, Re / Im |
|---|---|---|---|
| S1 | 2.0 k | 3.4 k / 1.9 k | 9.4 k / 2.2 k |
| S2 | 3.3 k | 8.1 k / 3.4 k | 25.7 k / 5.2 k |

Im is essentially the white floor (isotropic noise); Re has the same floor **plus the 15-26 Hz
pendulum**, which is 64 % (S1) / 84 % (S2) of the Re variance at night and 92-99 % by day
(graph 15). Nothing else is large: <6 Hz drift is 0.2-0.3 %, 26-200 Hz is a few %. The A/B
channels are different and unrelated: 213 counts (50 ppm), 55 % white, the rest 1/f, no
pendulum, identical day and night. (The earlier `|S|` amplitude statistics mix Re and Im, and
because the sensor phasor is not in line with the tilt axis the 40 Hz line that appears in
`|S|` is a second-order mixing artifact; in Re it is -25 dB below the 20 Hz line. Weak
48/60/71 Hz features, -17 to -25 dB, look like further mechanical modes.)

**Take 80 cycles, drop the 8 highest and 8 lowest, average the other 64: no gain.** 30 s scatter of
the capture estimate / live batch jitter (um nominal; S2 and S1-S2, the pendulum-dominated ones):

| per batch | night jitter S2 | day jitter S2 | day estimate S2 | day estimate S1-S2 |
|---|---|---|---|---|
| plain 64 (now) | 0.368 | 1.550 | 0.265 | 0.179 |
| plain 80 | 0.303 | 1.303 | 0.262 | 0.176 |
| 80 -> trim 8+8 by amplitude -> 64 | 0.310 | 1.395 | 0.411 | 0.353 |
| 80 -> trim 8+8 by in-phase part -> 64 | 0.309 | 1.416 | 0.289 | 0.197 |
| plain 96 | 0.216 | 0.932 | 0.267 | 0.181 |

The apparent jitter gain of trimming over the current 64 is just the longer window (plain 80 is
as good or better). Trimming by amplitude makes the capture-level estimate ~1.5-2x worse by day
(selection by `|S|` correlates with the pendulum phase). Reason: there are essentially no
outliers to trim (A/B per-cycle |z|>4 rate 3e-4 vs Gaussian 6e-5, sensor deviations are smooth
ring-down), and the pendulum is a coherent oscillation, not an outlier -- a linear window
(section 5) is the right tool. The only genuine single-cycle outlier is the stale first cycle
after start (section 1), which needs a discard rule, not a trim.

**Raw ADC spectrum (all 4 channels, 0-10.4 kHz, graph 14).** Below the 2604 Hz carrier there is
*nothing discrete*: no 50/100/150 Hz mains, no display (5 Hz) or switching lines (anything
>9 dB above the local floor would have been listed; none exist), on any channel, day or night.
The floor is smooth and falls with frequency (S2: -54 dBc/bin at 300 Hz to -81 at 10 kHz), with
a high-pass roll-off below ~150 Hz from the AC coupling. Only structure near the carrier is the
+/-20 Hz pendulum AM sidebands on S1/S2 (S2: -32 dBc night, -21 dBc day; S1: -54 / -43; weaker
at +/-10 and +/-30 Hz; A/B: -100 dBc and identical day/night), and harmonics at 5208 and 7812 Hz
(2nd/3rd): S2 -21/-15 dBc, S1 -37/-32 dBc, A/B -50/-58 dBc, 4th (Nyquist) S2 -28 dBc. Noise floor
near the carrier relative to the carrier: A/B -114 dBc/bin, S1 -77, S2 -60. **Caveat:** the sensor
channels carry large 2nd-4th harmonics (S2 3rd = -15 dBc); the 7th and 9th harmonics fold exactly onto
the carrier in an 8-sample demodulator and cannot be seen in this data, so the "harmonics are
negligible" statement in signal_processing.tex Section 11.3 is unchecked for the sensor channels.

**Why not 128-cycle batches (one 20.35 Hz period)?** (`analysis/box128.py`.) A 128-cycle boxcar nulls
20.35 Hz exactly (-72 dB) but the pendulum is at 19.9 Hz (18-21.5): gain -14.5/-17.9/-23/-33/-72/-30/-25/-23 dB at
17/18/19/19.9/20.35/21/21.5/22 Hz. Batch-to-batch jitter does fall ~9x (S2 day 5.25 -> 0.58 um, S1 1.68 -> 0.24).
But (1) the current 8-batch MA is already one 512-cycle boxcar with the same null, so the *smoothed* reading
gains nothing (capture estimate, S2 day: 11x64 = 0.265 um, 5x128 = 0.295 um); (2) 128-cycle batches
decimate to 20.35 Hz, so what the boxcar leaves of the pendulum (-18..-25 dB over most of its band) folds to
a 0.1-2 Hz wander that no later low-pass can remove, whereas at 64 cycles (40.7 Hz) it folds to ~20 Hz
where a second averaging stage removes it (64 batch, MA8, MA8: -38.7 dB worst 17-23 Hz; measured ~2x better
by day). (2) is from sampling theory, not measured (captures are too short). 128 does halve the division
load (CPU margin). To get the 128-cycle null without the aliasing, use MA2 on the 64-cycle batches.

**What the 24 h / 20 Hz granite stream can and cannot say** (`analysis/precision_window.py`, `stream_psd.py`,
`decimation_test.py`). That stream is the latest batch value pushed every ~57 ms (17.45 Hz mean), i.e. the 40.7 Hz batch
series decimated by ~2.3 with irregular gaps. The pendulum passes the 64-cycle batch at only -3.7 dB, so it is
aliased into 0-8.7 Hz instead of being cancelled. Evidence: in the quiet 200-245 min window the stream PSD is nearly flat
(0.03-0.12 um^2/Hz over 0.01-8 Hz) and the repeatability of T-second means follows white-noise sqrt(N) within 1.6x up to 3 s.
Decimating our contiguous captures the same way makes the plain mean 2.5-9x worse (30 s scatter, night S1/S2/diff
0.07/0.12/0.07 -> 0.17/0.50/0.35; day 0.14/0.27/0.18 -> 0.73/2.27/1.57 um nominal). Consequences: (1) repeatability
numbers derived from the stream (incl. earlier "SE plateaus" conclusions below ~10 s) are pessimistic for the firmware's
contiguous averaging; (2) robust estimators (median/trim/flag-filter) help on the stream (-30..-50 % with operator
present) because aliased ring-down bursts look like outliers there, but not on contiguous batches (section 5);
(3) future long tests should push every batch (event-driven) or log the phasor stream contiguously.

## Reproducing

```
cd analysis
python load.py            # parse .bin -> phasors.npz (142 MB cache, not committed)
python run_stats.py       # capture_stats.csv, stats.npz
python plots.py           # graphs 1-8
python quiet.py; python plot_quiet.py        # night/day comparison, graphs 9-10
python pendulum.py; python plots2.py          # pendulum spectrum + filter plots, graphs 11-13
python emulate2.py; python strategies.py; python strategies2.py   # estimator tests (printed)
```
