# Host unit tests

Fast, hardware-free tests for the firmware's **pure-logic** pieces — the
parts that are only otherwise exercised by flashing a board:

| suite | covers |
|-------|--------|
| `test_math_crc.c`  | `Math/math_crc.c` — CRC-16/CCITT-FALSE (spec check value, edge cases, the real API- and ADS131M04-frame uses) |
| `test_math_phasor.c` | `Math/math_phasor.c` — per-position batch accumulation + `math_phasor_combine()` vs the per-sample reference (exact equality on random and full-scale data, int32 bound) |
| `test_math_window.c` | `Math/math_window.c` — the LIVE display / precision-measurement window logic: Hann weights, ring wrap, window-level quality flag (burst, recovery, floor creep, break) |
| `test_math_displacement.c` | `Math/math_displacement.c` — the per-batch demodulation: known tilt/phase/zero/PGA in, tilt out; degenerate excitation; the flip-calibration formula |
| `test_math_quality.c` | `Math/math_quality.c` — the display stream (cadence, doubtful flag, gaps, wrap) and the sliding-window precision measurement (waits for the floor, rejects disturbed windows, timeout, break) |
| `test_golden_displacement.c` | replays a recorded 10.6-minute excerpt of the real 19 h phasor stream (`data/`, committed — a clone can run it) through the same pipeline and must reproduce the values the firmware produced before the maths was extracted |
| `test_api_core.c` | `Services/svc_api.c` — the API dispatcher on a synthetic table: the staged validation order (category, verb, resource, resource verb, CRC, length), status propagation, response framing, after-reply actions, interval and event subscriptions, per-transport isolation, the byte reassembler |
| `test_api_tables.c` | the REAL generated tables (`Services/svc_api_tables.c`) with the field handler: every resource reachable with its verbs and request length, every settings/calibration field bounds-checked and persisted, cross-field rules, the RTC trim hook, state-changing IDs safe against a stale v2 client |
| `test_apiv3.py` | the Python client library (`PythonTestCode/apiv3.py`): CRC, framing, byte-stream and USB HID reassembly, every generated payload round-tripped, generated files up to date |
| `test_math_fault.c` / `test_math_supervisor.c` | the fault record kept across a reset (pack / unpack / corruption check) and the decision logic of the supervised watchdog (when the SysTick interrupt may refresh it) |
| `test_svc_service.c` | service mode: entered only locally, left explicitly, ended by 10 minutes without API activity, restarted by each request |
| `test_txframe.c`   | `Services/svc_txframe.c` — the SPSC frame FIFO: FIFO order, wrap, the 64-byte urgent reserve, oversized-frame refusal, reset |
| `test_transfer.c`  | the extracted fixed-point transfer / decode functions: `drv_tmp236_mv_to_cdeg` (two-segment fit, boundary continuity, negative °C), `drv_lm35_mv_to_cdeg`, `drv_encoder_quad_step` (all 16 Gray-code transitions) |

Each suite is one self-contained `.c` that `#include`s the source under
test directly. `test.h` is a ~60-line assert harness — no Unity, no
ceedling, no dependency.

## Running

Needs a **native** C compiler (`gcc` / `clang`), *not* `arm-none-eabi-gcc`.

```sh
cd tests
make                       # build + run all suites
CC=clang make              # or pick the compiler
CC="python -m ziglang cc" make   # no compiler installed? pip install ziglang (zig cc)
```

Exit code is non-zero if any check fails — drop this into CI as-is.

The golden test reads `data/` relative to the working directory (run from `tests/`, or from the repo root).
See `data/README.md` for what the recorded data is and how to change the golden deliberately.

## `oracle_crc.py`

A pure-CPython CRC-16/CCITT-FALSE reference. Runs with no compiler.
Confirms the algorithm against the catalogue check value and against
`PythonTestCode/apiv2.py`, and regenerates the pinned expected values in
`test_math_crc.c`:

```sh
python tests/oracle_crc.py --verify
```

## Adding a suite

1. Extract the logic under test into a pure function (no HAL, no globals —
   pass config/calibration in). Put trivial leaf functions `static inline`
   in the driver header; anything with branches in a `_calc.c`.
2. New `tests/test_<thing>.c`, `#include "test.h"` + the source.
3. Add it to `SUITES` in the `Makefile`.
