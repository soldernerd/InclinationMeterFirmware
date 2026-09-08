# Display: banded (u8g2 page-buffer) rendering

**Branch:** `Display_PageMode` · **fw 0.9.39**

## Problem

`task_display` did a full 400×240 u8g2 render + framebuffer parse in one
blocking call. On the single-threaded cooperative scheduler that call is
the longest thing in a pass — ~65 ms idle at `-O2`, ~110 ms while the
ADS131M04 acquisition ISR load steals cycles (fw 0.9.38 bench). It fires
on every visible change (the STATUS clock ticks once a second), so
`task_api` and every other task ate a ~110 ms tail latency that often.

The `-O2` pass (fw 0.9.38, `1aaf18f`) took the full render from ~120 ms to
~65 ms but the *shape* of the problem was unchanged: one unbounded
blocking call.

## Change

u8g2 switched from the full framebuffer (`_f`, 12 KB) to page-buffer mode
(`_2`, 800 B — two 8-px tile rows). `app_display_update()` is now a small
state machine:

- **DISP_IDLE** — change-detect early-out (unchanged `snapshot_changed()`).
  On a real change: freeze the time base (`s_render_ms`) and capture the
  value snapshot, `u8g2_FirstPage()`, go to DISP_RENDER.
- **DISP_RENDER** — each call renders `DISPLAY_PAGES_PER_TICK` bands
  (`config.h`, default 3): run the full compositor (`draw_active_screen()`,
  u8g2 clips each draw op to the current band), `u8g2_NextPage()`. When
  `NextPage` returns 0 the last band has been written into
  `drv_sharp_lcd`'s framebuffer — one DMA blit pushes the whole image to
  the panel, back to DISP_IDLE.

`task_display` now runs **every tick** (was `task_display_ms` = 100 ms) so
a redraw completes promptly — 15 bands / 3 per tick ≈ 5 ticks. Idle ticks
are just the change-detect check (~1 µs at `-O2`). The 100 ms
`task_display_ms` setting still paces `task_ui`.

The `u8g2_hal_callback.c` byte-sink parser is unchanged: every DRAW_TILE
transfer carries an absolute line address, so it places rows correctly
regardless of which band they arrived in — it needs no page awareness.

### Tearing

`s_render_ms` freezes everything derived purely from elapsed time (the
STATUS uptime clock) for the whole multi-tick render. The value snapshot
is captured at render start. The *structural* selectors
(`battery_low` / measurement state / `current_screen`) are still read live
each band — a change mid-render tears one frame, then `snapshot_changed()`
forces a clean redraw the next pass. A future fast-updating screen (live
angle readout) would need its inputs frozen into the snapshot the same way
`s_render_ms` is.

## Bench (fw 0.9.39, wired-UART API, ADS131M04 running)

API round-trip latency, IDENTITY, tight harness:

| | idle median | idle p99/max | busy median | busy p95 | busy p99/max |
|---|---|---|---|---|---|
| **0.9.38** full render | 4.5 ms | 65 / 65 ms | 4.6 ms | 5.3 ms | 103 / 110 ms |
| **0.9.39** banded | 4.7 ms | 30 / 30 ms | 4.7 ms | 34 ms | **52 / 54 ms** |

Worst-case tail roughly halved; the scheduler is never blocked for a full
render. p95 rises (5 → 34 ms) — the display cost is now *spread* across
more passes instead of concentrated in a few. Lower `DISPLAY_PAGES_PER_TICK`
to trade tail for spread.

ADC acquisition integrity unchanged: `frame_deficit` 0 (range −2..0),
`ring_overflow` 0, 0 drops, CRC match, no fault over 58 s / 1.2 M frames.

RAM: **−11.2 KB** (78.6 % → 71.1 %) — the 12 KB `_f` framebuffer is gone,
`drv_sharp_lcd`'s 12 KB blit buffer stays. FLASH: +128 B.

## Not done

- **Visual confirmation** on the physical panel — the band math and the
  parser are page-mode-safe by construction, but a human needs to eyeball
  LIVE / STATUS / SETTINGS / measuring overlay / low-battery.
- Freezing structural selectors / a proper render snapshot for
  zero-tear fast screens — a WP5 compositor concern.
