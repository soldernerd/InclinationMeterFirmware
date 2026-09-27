# Sensor swap test

**Date:** 2026-09-26. **Firmware:** 0.10.42 (unchanged from the streaming
monitor test — no firmware change needed). **Board:** #2.

## Purpose

`../2026-09-26_10min_streaming_monitor/` found ch3 ("S1") far noisier than ch0
("S2"), with a large population of unshared, heavy-tailed jumps. The
then-current channel-mapping doc claimed S1 used an external cable and S2
didn't, making cable microphonics the leading suspect. User corrected this:
both sensors actually use identical, interchangeable 2m cables (the doc
comment in `Services/svc_displacement.h` was simply wrong, and was fixed as
part of this investigation). To actually test cable-vs-sensor-head, the user
physically swapped the two sensor heads between connectors and asked for the
exact same 10-minute streaming monitor to be repeated.

## Method

Identical to `../2026-09-26_10min_streaming_monitor/` — same
`monitor_10min_stream_swapped.py` script (copy of `monitor_10min_stream.py`,
same Topic 0x03 SUBSCRIBE method), same board, same firmware, only the
physical sensor heads swapped between the two connectors beforehand. No
firmware change was needed since the swap is purely physical/electrical — the
API already reports both channels symmetrically.

Two output files, same format as the streaming-monitor test:
- `monitor_10min_stream_swapped.csv` — fast stream
- `monitor_10min_stream_swapped_slow.csv` — slow side-channel diagnostics
