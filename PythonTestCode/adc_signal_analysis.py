#!/usr/bin/env python3
"""
Repeated bulk-ADC captures + FFT-based signal characterization: frequency,
amplitude, THD, SNR/SINAD/ENOB, ch1-ch2 phase, and run-to-run repeatability.

Reuses bulk_adc_csv.py's capture() (same wire protocol, same
ADC_BULK_SAMPLE_COUNT), so results are directly comparable to plain
bulk_adc_csv.py runs -- this just adds spectral analysis and repeats it.

  python adc_signal_analysis.py --port COM6                # 5 captures (default)
  python adc_signal_analysis.py --port COM6 --n 10 --out adc_analysis.csv

Needs numpy (already on this box).
"""

import argparse
import sys
import time

import numpy as np
import serial

import apiv2 as a
from bulk_adc_csv import capture, find_port

BAUD = 115200
SAMPLE_RATE_HZ = 64_000_000 / 3072  # = 20833.333... Hz, config.h ADS131M04_FDATA_TIMER_TICKS
LSB_MV = 2.4 / (1 << 23) * 1000     # ADC_RAW_LSB_V (apiv2.py) in mV
LIVE_CHANNELS = (0, 1, 2, 3)        # check all 4 -- which channels are "live" changes as
                                     # sensors get connected/disconnected during bring-up;
                                     # analyze_channel() returns None for a flat one anyway
N_HARMONICS = 9                     # 2nd..9th evaluated for THD
HANN_ENBW_CORRECTION = 1.5          # (noise_gain/coherent_gain)^2 for a Hann window -- see analyze_channel()


def analyze_channel(raw_codes, fs=SAMPLE_RATE_HZ):
    """FFT-based characterization of one channel's capture.
    Returns a dict, or None if there's no discernible tone (flat channel)."""
    x = np.asarray(raw_codes, dtype=np.float64) * LSB_MV
    x = x - x.mean()
    n = len(x)

    # Time-domain reference numbers (window-free, so trustworthy on their own)
    p2p_td = x.max() - x.min()
    rms_td = float(np.sqrt(np.mean(x ** 2)))
    if p2p_td < 1.0:  # < 1 mV pp -- noise floor, nothing to fit a tone to
        return None

    win = np.hanning(n)
    cg = win.mean()  # coherent gain, corrects the window's amplitude scaling
    X = np.fft.rfft(x * win)
    freqs = np.fft.rfftfreq(n, d=1.0 / fs)
    amp = np.abs(X) / (n * cg)
    amp[1:-1] *= 2.0  # single-sided spectrum (everything except DC/Nyquist)

    dc_guard = 3  # ignore bins 0..2 (DC + its window skirt) when hunting for the tone
    k0 = int(np.argmax(amp[dc_guard:])) + dc_guard

    # Parabolic (quadratic) interpolation across the 3 bins around the peak
    # for sub-bin frequency resolution -- log-magnitude gives a cleaner fit.
    def logmag(k):
        return np.log(amp[k] + 1e-30)
    a1, a2, a3 = logmag(k0 - 1), logmag(k0), logmag(k0 + 1)
    denom = (a1 - 2 * a2 + a3)
    delta = 0.5 * (a1 - a3) / denom if abs(denom) > 1e-12 else 0.0
    freq_hz = (k0 + delta) * fs / n
    fund_amp = amp[k0]  # peak, mV -- accurate to ~<1% here since cycles/record is near-integer

    def local_peak_amp(target_freq):
        k = int(round(target_freq * n / fs))
        if k < 1 or k + 1 >= len(amp):
            return 0.0
        lo, hi = max(1, k - 2), min(len(amp) - 1, k + 2)
        return float(amp[lo:hi + 1].max())

    harmonic_amps = [local_peak_amp(freq_hz * h) for h in range(2, N_HARMONICS + 1)]
    p_fund = fund_amp ** 2 / 2.0
    p_harm = sum(h ** 2 for h in harmonic_amps) / 2.0

    # Noise floor: sum the windowed spectrum's bins directly, excluding a
    # guard band around the fundamental and each harmonic (a Hann window's
    # main lobe is ~4 bins wide, plus sidelobes -- 5 bins each side covers
    # it) so leaked-but-real tone energy isn't miscounted as noise. This
    # stays entirely in the windowed-spectrum domain rather than
    # subtracting a time-domain total (rms_td**2) from the spectral
    # fundamental+harmonics: those are two independent estimates of
    # nearly the same large number once SNR is good, and subtracting them
    # is catastrophic cancellation -- an early version of this script did
    # exactly that and the reported SNR swung between ~48 dB and a
    # meaningless ~149 dB run to run on what time-domain stats show is a
    # visually-identical clean sine each time.
    #
    # The bin-sum below systematically over-reads noise power by
    # (noise-bandwidth-gain / coherent-gain)^2 -- for a Hann window,
    # (sqrt(3/8)/0.5)^2 = 1.5 -- because coherent-gain normalization is
    # only exact for a concentrated tone, not spread-spectrum noise;
    # HANN_ENBW_CORRECTION divides that back out.
    guard = 5
    noise_mask = np.ones(len(amp), dtype=bool)
    noise_mask[:dc_guard] = False
    for center_freq in [freq_hz] + [freq_hz * h for h in range(2, N_HARMONICS + 1)]:
        c = int(round(center_freq * n / fs))
        lo, hi = max(0, c - guard), min(len(noise_mask), c + guard + 1)
        noise_mask[lo:hi] = False
    p_noise = float(np.sum(amp[noise_mask] ** 2)) / 2.0 / HANN_ENBW_CORRECTION
    p_noise = max(p_noise, 1e-9)

    thd_pct = (np.sqrt(sum(h ** 2 for h in harmonic_amps)) / fund_amp * 100.0) if fund_amp > 0 else float("nan")
    snr_db = 10 * np.log10(p_fund / p_noise)
    sinad_db = 10 * np.log10(p_fund / (p_noise + p_harm))
    enob = (sinad_db - 1.76) / 6.02

    return dict(freq_hz=freq_hz, peak_mv=fund_amp, rms_td_mv=rms_td, p2p_td_mv=p2p_td,
                thd_pct=thd_pct, snr_db=snr_db, sinad_db=sinad_db, enob=enob,
                fft_bin=k0, fft_phase_rad=float(np.angle(X[k0])))


def run(port, n_captures, timeout):
    ser = serial.Serial(port, BAUD, timeout=0.2)
    results = {ch: [] for ch in LIVE_CHANNELS}
    print(f"{'#':>2}  {'ch':>3}  {'freq (Hz)':>10}  {'peak (mV)':>10}  {'RMS (mV)':>9}  "
          f"{'THD %':>7}  {'SNR (dB)':>9}  {'SINAD (dB)':>10}  {'ENOB':>5}")
    completed = 0
    for i in range(n_captures):
        try:
            rows, gaps = capture(ser, timeout=timeout)
        except TimeoutError as e:
            # Handling the board mid-session (moving it, touching cables)
            # has repeatedly caused a hang partway through a capture this
            # session -- don't lose the captures already collected over
            # one timeout; report what we have and let the caller reset
            # and retry for the rest.
            print(f"  (capture {i}: {e} -- stopping here, keeping the "
                  f"{completed} capture(s) already collected)")
            break
        completed += 1
        if gaps:
            print(f"  (capture {i}: {gaps} gap/CRC event(s))")
        for ch in LIVE_CHANNELS:
            codes = [r[ch] for r in rows]
            res = analyze_channel(codes)
            if res is None:
                print(f"{i:>2}  ch{ch:<2} -- flat, no tone")
                continue
            results[ch].append(res)
            print(f"{i:>2}  ch{ch:<2}  {res['freq_hz']:>10.3f}  {res['peak_mv']:>10.2f}  "
                  f"{res['rms_td_mv']:>9.2f}  {res['thd_pct']:>7.3f}  {res['snr_db']:>9.2f}  "
                  f"{res['sinad_db']:>10.2f}  {res['enob']:>5.2f}")

    # Pairwise phase difference for every pair of channels that actually
    # showed a tone in every capture (order/count of "live" channels isn't
    # assumed -- which ones have a real signal can change as sensors get
    # connected/disconnected during bring-up).
    live = [ch for ch in LIVE_CHANNELS if completed and len(results[ch]) == completed]
    for a_ch, b_ch in [(x, y) for i, x in enumerate(live) for y in live[i + 1:]]:
        print(f"\nch{a_ch}-ch{b_ch} phase difference per capture:")
        for i, (r1, r2) in enumerate(zip(results[a_ch], results[b_ch])):
            dphi = np.degrees(r2['fft_phase_rad'] - r1['fft_phase_rad'])
            dphi = (dphi + 180) % 360 - 180
            print(f"  {i}: {dphi:+7.2f} deg")

    print("\nrun-to-run repeatability (mean +/- std over the captures above):")
    for ch in LIVE_CHANNELS:
        rs = results[ch]
        if not rs:
            continue
        freqs = np.array([r['freq_hz'] for r in rs])
        peaks = np.array([r['peak_mv'] for r in rs])
        thds = np.array([r['thd_pct'] for r in rs])
        print(f"  ch{ch}: freq {freqs.mean():.3f} +/- {freqs.std():.4f} Hz "
              f"({freqs.std() / freqs.mean() * 1e6:.1f} ppm)   "
              f"peak {peaks.mean():.2f} +/- {peaks.std():.3f} mV "
              f"({peaks.std() / peaks.mean() * 100:.3f} %)   "
              f"THD {thds.mean():.3f} +/- {thds.std():.3f} %")

    ser.close()
    return results


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--n", type=int, default=5, help="number of back-to-back captures")
    ap.add_argument("--timeout", type=float, default=20.0)
    args = ap.parse_args()
    port = args.port or find_port()
    print(f"Opening {port} @ {BAUD} -- {args.n} captures of "
          f"{a.ADC_BULK_SAMPLE_COUNT} samples ({a.ADC_BULK_SAMPLE_COUNT / SAMPLE_RATE_HZ * 1000:.1f} ms) each\n")
    run(port, args.n, args.timeout)


if __name__ == "__main__":
    main()
