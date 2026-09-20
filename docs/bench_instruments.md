# Bench instruments — SCPI reference

Two SCPI-controllable instruments live on this bench. Both are driven the
same way: PyVISA + plain SCPI ASCII commands over a VISA resource string.
The shared Python wrapper is `PythonTestCode/bench_instruments.py`
(`Psu` + `Scope` classes); `PythonTestCode/scope_label_channels.py` is the
CLI for the scope's channel-naming problem specifically.

```sh
pip install pyvisa
```

VISA runtime: this bench already has one installed (Keysight IO Libraries
Suite — `KeysightInstrumentIoServi...` / `ConnectionExpert.Server.H`
services), confirmed by `pyvisa.ResourceManager().list_resources()`
enumerating both instruments below. No extra setup needed on this PC.

---

## Keysight E36104A — bench power supply

| | |
|---|---|
| Link | USB (USBTMC) |
| VISA resource | `USB0::0x2A8D::0x0802::MY55506105::0::INSTR` |
| Wrapper | `bench_instruments.Psu` |

**Safety limit (agreed, do not raise without asking the user first):**
voltage ≤ **4.3 V**, current limit ≤ **100 mA**. `Psu.set_voltage()` /
`Psu.set_current_limit()` enforce this in code (raise `ValueError` above
the cap) — belt-and-braces alongside just not doing it.

| SCPI | wrapper method | notes |
|---|---|---|
| `*IDN?` | `idn()` | |
| `MEAS:VOLT?` | `measure_voltage()` | read-only |
| `MEAS:CURR?` | `measure_current()` | read-only |
| `VOLT <v>` | `set_voltage(v)` | capped at 4.3 V |
| `CURR <a>` | `set_current_limit(a)` | capped at 0.100 A |

`PythonTestCode/bench_power_sweep.py` (the current-consumption bisection
script) predates this module and has its own minimal read-only `Psu` —
it never writes voltage/current, only queries, so there's nothing to
migrate there; both can coexist.

---

## Keysight/Agilent MSO-X 3024A — mixed-signal oscilloscope

| | |
|---|---|
| Link | LAN |
| VISA resource | `TCPIP0::192.168.1.107::inst0::INSTR` |
| Wrapper | `bench_instruments.Scope` |
| Channels | 4 analog (`CHANnel1..4`) + 16 digital (`DIGital0..15`, 2 pods of 8) |

Command reference confirmed against Keysight's InfiniiVision X-Series
Programmer's Guide (3000/2000 X-Series share this command set):

| SCPI | wrapper method | notes |
|---|---|---|
| `:CHANnel<n>:LABel "<s>"` | `set_analog_label(ch, s)` | n = 1..4 |
| `:CHANnel<n>:LABel?` | `get_analog_label(ch)` | |
| `:DIGital<d>:LABel "<s>"` | `set_digital_label(ch, s)` | d = 0..15 |
| `:DIGital<d>:LABel?` | `get_digital_label(ch)` | |
| `:DISPlay:LABel {ON\|OFF}` | `show_labels(on)` | global label-visibility toggle |
| `:CHANnel<n>:DISPlay {ON\|OFF}` | `enable_analog(ch, on)` | trace on/off |
| `:DIGital<d>:DISPlay {ON\|OFF}` | `enable_digital(ch, on)` | trace on/off, MSO only |
| `:POD<n>:DISPlay {ON\|OFF}` | `enable_pod(pod, on)` | pod 1 = D0-D7, pod 2 = D8-D15 |
| `:MEASure:VAMPlitude? CHANnel<n>` | `measure_vamplitude(ch)` | |
| `:MEASure:VPP? CHANnel<n>` | `measure_vpp(ch)` | |
| `:MEASure:FREQuency? CHANnel<n>` | `measure_frequency(ch)` | |
| `:MEASure:PHASe? CHAN<a>,CHAN<b>` | `measure_phase(a, b)` | |
| `:DISPlay:DATA? PNG,COLor` | `screenshot_png(path)` | IEEE 488.2 binary block |

**Gotchas:**
- Labels are capped at **10 characters** and the scope **uppercases**
  them — `set_analog_label(1, "sig_1")` ends up reading `SIG_1` on
  screen. `LABEL_MAX_LEN` in the module documents the cap.
- A label alone doesn't make a channel visible — a digital channel needs
  both its **pod** (`:POD<n>:DISPlay ON`) and its own
  (`:DIGital<d>:DISPlay ON`) enabled, or the new label sits on a trace
  that isn't drawn. `Scope.label_all(..., enable=True)` (the default)
  handles this; pass `enable=False` to only rename channels already on
  screen without touching visibility.

### Naming all 20 channels in one call

```sh
cd PythonTestCode
python scope_label_channels.py --analog 1:S1,2:S2,3:S3,4:S4 \
                                --digital 0:SCK,1:MOSI,2:CS,3:DRDY
```

or from a JSON file for a fixed per-project probe layout:

```json
{"analog": {"1": "S1", "2": "S2"}, "digital": {"0": "SCK", "1": "MOSI"}}
```

```sh
python scope_label_channels.py --json my_layout.json --screenshot scope.png
```

`--list` prints the 20 current labels; `--hide` turns the on-screen
labels off without erasing them.

---

## Why a shared module instead of one script per task

Both instruments' SCPI dialects are centralized once, here, the same way
`PythonTestCode/apiv2.py` centralizes the firmware's own wire protocol —
new bench scripts import `bench_instruments` rather than re-deriving
command syntax from memory each time.
