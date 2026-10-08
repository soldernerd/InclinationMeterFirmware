# Documentation index

## Current -- describes the product as it is
| File | What |
|---|---|
| [api-v3-spec.md](api-v3-spec.md) | The device API protocol: framing, verbs, status codes, validation order, subscriptions, service mode, transports, how to change it |
| [api-reference.md](api-reference.md) | Every resource with opcode, request and response layout (**generated** from `tools/api_spec.py`) |
| [signal_processing.pdf](signal_processing.pdf) / `.tex` | The measurement chain: excitation, sampling, phasor demodulation, display and precision filters, calibrations, the 19 h analysis |
| [watchdog-and-faults.md](watchdog-and-faults.md) | Supervised watchdog, fault capture and service mode, with the bench plan |
| [release-checklist.md](release-checklist.md) | What must be true before 1.0.0 |
| [decisions.md](decisions.md) | Tuning rationale that used to live in `Config/config.h` comments (kept verbatim) |
| [cubemx_configuration_checklist.md](cubemx_configuration_checklist.md) | What to check after regenerating from CubeMX |
| [bench_instruments.md](bench_instruments.md) | The bench setup (PSU / DMM control) |

## Subsystem notes -- design and bring-up of one part, still accurate
| File | What |
|---|---|
| [wp7_ad9833_dac.md](wp7_ad9833_dac.md) | The AD9833 excitation source |
| [wp8_ads131m04_adc.md](wp8_ads131m04_adc.md) | The ADS131M04 and its raw-DMA read |
| [adc_acquisition_redesign.md](adc_acquisition_redesign.md) | Why the acquisition runs from the SysTick drain |
| [wp9_bme280_env_sensor.md](wp9_bme280_env_sensor.md) | The BME280 |
| [display_page_render.md](display_page_render.md) | Banded rendering of the display |
| [wp4_reboot_to_dfu.md](wp4_reboot_to_dfu.md) | How "Reboot to DFU" works and how to recover |

## History -- records of how we got here, not a description of the product
| File | What |
|---|---|
| [wp10_displacement.md](wp10_displacement.md) | The displacement / tilt work log, newest at the bottom (parts describe removed features) |
| [wp2-5_rebase_status.md](wp2-5_rebase_status.md) | The August branch-rebase effort |
| [pinout_migration_wp2-5.md](pinout_migration_wp2-5.md) | The REV A to REV B pin migration survey |
| [api-v2-spec.md](api-v2-spec.md) | Pointer: v2 is superseded by v3 |
| `archive-ioc/` | Superseded CubeMX project files, do not regenerate from them |

Pin assignments: `Config/pin_config.h` and `WylerLeveltronic.ioc` are authoritative. Bench-test records: `Testing/README.md`.
