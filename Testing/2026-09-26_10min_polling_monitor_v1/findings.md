# Findings — SUPERSEDED, see 2026-09-26_10min_streaming_monitor

**At face value**, this run showed: steady-state step noise std ~0.0022mm on
both S1/S2 (matching the user's own "±0.001-0.002mm" description), a handful
of jumps >0.005mm but almost none >0.010mm, and — the headline conclusion at
the time — **S1 and S2 jumps appeared to occur at completely different times
(zero overlap)**, which looked like it ruled out any shared cause (temperature,
EMI, shared rail/clock, ADC clipping) and pointed at independent per-channel
analog noise.

**That "zero overlap" conclusion was wrong** — a measurement artifact of
polling delta1 and delta2 via two separate, non-simultaneous UART round trips
at ~1.6s spacing. It could easily misalign a genuinely simultaneous fast event
between the two channels. Repeating the exact same test with a real
simultaneous-sample stream (`../2026-09-26_10min_streaming_monitor/`) showed
the opposite: 100% of S2's big jumps coincided exactly with an S1 jump at the
same sample, i.e. there IS a shared component.

`clip_count`/`amplitude_fault_count`/`degenerate` stayed flat throughout (real,
still holds) — not ADC clipping or a computation fault either way.

**Lesson kept for the record:** don't trust cross-channel timing correlation
(or its absence) from independently-polled GETs: only a genuinely simultaneous
per-batch stream can answer "did these two channels move together."
