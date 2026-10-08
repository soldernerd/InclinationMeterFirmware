# Drift investigation: why does S1 ramp in phase? (started 2026-10-08)

**Question.** After the phase / zero / k calibration (see `../2026-10-07_phase_calibration/setup.md`) the 24 h stream
(`../2026-10-07_24h_phasor_stream_calibrated/`) shows S1 ramping about +2.3 um/m per hour (+18 um/m in 9.5 h), nearly linear,
purely in phase (the quadrature part moves < 1 um/m); S2 moves much less (net +1 um/m). The first 24 h run (2026-09-27,
fw 0.10.53, other mounting) shows the same: S1 ramp ~3.4 um/m per h in the same sense, S2 without a ramp. Temperature, SoC,
charging and the A/B balance term explain at most 2-3 um/m of it (`common_mode_analysis.py` in the 24 h folder).

**Hypotheses.** (a) additive interference at the carrier frequency or an ADC / PGA effect specific to channel 3 (S1);
(b) a term proportional to the excitation that the ratio S/(A-B) does not cancel: coupling (leakage) from A or B into the S1
input, or the sensor / instrument 1 itself (creep, thermal); (c) environment (humidity of the board).
Physical swaps are not possible until the weekend.

## Tests (all remote; fw 0.10.76 adds the two commands they need)

| test | what it separates | firmware / script |
|---|---|---|
| phase reversal: excitation phase 0 / 90 / 180 / 270 deg (AD9833 PHASE0, Commands 0x0A) | an excitation-proportional signal gives the same S/D at 0 and 180 deg, an additive one flips sign. 10 min. | `drift_tests.py --plan phase` |
| ADC input short on ch3 (S1) and ch0 (S2) (ADS131M04 CHn_CFG.MUX, Commands 0x0B), excitation on | S measured with the inputs shorted = ADC / PGA / digital additive terms alone. If S1 still ramps -> the channel; if flat -> upstream of the pins | `--plan short` (2 h) |
| excitation off for 3 h (AD9833 RESET), sensors connected | any in-phase drift of S1 with no excitation = additive interference; then restart and see if the ramp resumes / resets | `--plan off` |
| chopping: phase 0 / 180 every 30 s for 6 h | splits the ramp into the proportional and the additive part continuously; a chopped reading cancels an additive drift | `--plan chop` |
| env logging (BME280 humidity / pressure / temperature every 30 s) in every run | humidity-driven leakage would look exactly like this | built into the runner |
| raw ADC bulk captures with excitation on / off | spectrum near 2.6 kHz, DC, harmonics | later |

`drift_tests.py` runs a plan unattended, logs every 64-cycle batch with the applied state and always restores excitation
ON / phase 0 / all channels normal on exit; `analyze_drift_tests.py` summarises a run; `settings_backup.py` saves and restores
the board settings around a flash (the battery-page layout change of fw 0.10.75 reseeds that page).

## Log

* 2026-10-08: fw 0.10.76 builds (excitation and ADC-mux commands, charge-full timeout 0.10.75). 24 h capture still running
  on 0.10.73 until ~21:40; tests start after it ends.
