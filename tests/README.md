# Host unit tests

Fast, hardware-free tests for the firmware's **pure-logic** pieces — the
parts that are only otherwise exercised by flashing a board:

| suite | covers |
|-------|--------|
| `test_math_crc.c`  | `Math/math_crc.c` — CRC-16/CCITT-FALSE (spec check value, edge cases, the real API- and ADS131M04-frame uses) |
| `test_txframe.c`   | `Services/svc_txframe.c` — the SPSC frame FIFO: FIFO order, wrap, the 64-byte urgent reserve, oversized-frame refusal, reset |
| `test_transfer.c`  | the extracted fixed-point transfer / decode functions: `drv_tmp236_mv_to_cdeg` (two-segment fit, boundary continuity, negative °C), `drv_lm35_mv_to_cdeg`, `drv_encoder_quad_step` (all 16 Gray-code transitions) |

Each suite is one self-contained `.c` that `#include`s the source under
test directly. `test.h` is a ~60-line assert harness — no Unity, no
ceedling, no dependency.

## Running

Needs a **native** C compiler (`gcc` / `clang`), *not* `arm-none-eabi-gcc`.

```sh
cd tests
make                       # build + run all three
CC=clang make              # or pick the compiler
```

Exit code is non-zero if any check fails — drop this into CI as-is.

> The current dev box has no host compiler installed, so these can't be
> run here yet. `oracle_crc.py` (below) is the piece that runs today.

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
