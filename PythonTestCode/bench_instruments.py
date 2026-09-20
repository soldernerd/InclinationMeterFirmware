"""
Shared PyVISA helpers for the two SCPI bench instruments used on this
project. See docs/bench_instruments.md for connection details, safety
limits, and the command references these wrap.

  - Psu    Keysight E36104A bench power supply   (USB, VISA)
  - Scope  Keysight/Agilent MSO-X 3024A mixed-signal scope (LAN, VISA)

Install:  pip install pyvisa
Needs a VISA runtime (Keysight IO Libraries Suite or NI-VISA) for the
TCPIP/USB backends -- already present on this bench (confirmed via
pyvisa.ResourceManager().list_resources()).
"""

import pyvisa

PSU_RESOURCE = "USB0::0x2A8D::0x0802::MY55506105::0::INSTR"
SCOPE_RESOURCE = "TCPIP0::192.168.1.107::inst0::INSTR"

# Bench safety limits agreed for this project -- see docs/bench_instruments.md
# and memory/keysight-psu-limits (do not raise without the user's say-so).
PSU_MAX_VOLTAGE_V = 4.3
PSU_MAX_CURRENT_A = 0.100


class Psu:
    """Keysight E36104A, single channel used on this bench.

    Voltage/current setters are hard-capped at the bench's agreed limits --
    they raise ValueError rather than silently clamping, so a bad value
    fails loud instead of quietly doing something else. Measurement calls
    are always safe (read-only queries).
    """

    def __init__(self, resource=PSU_RESOURCE, rm=None):
        self.rm = rm or pyvisa.ResourceManager()
        self.h = self.rm.open_resource(resource)
        self.h.timeout = 3000

    def idn(self):
        return self.h.query("*IDN?").strip()

    def measure_voltage(self):
        return float(self.h.query("MEAS:VOLT?"))

    def measure_current(self):
        return float(self.h.query("MEAS:CURR?"))

    def set_voltage(self, volts):
        if volts > PSU_MAX_VOLTAGE_V:
            raise ValueError(f"{volts} V exceeds the agreed {PSU_MAX_VOLTAGE_V} V bench limit")
        self.h.write(f"VOLT {volts}")

    def set_current_limit(self, amps):
        if amps > PSU_MAX_CURRENT_A:
            raise ValueError(f"{amps} A exceeds the agreed {PSU_MAX_CURRENT_A} A bench limit")
        self.h.write(f"CURR {amps}")

    def close(self):
        self.h.close()


ANALOG_CHANNELS = (1, 2, 3, 4)
DIGITAL_CHANNELS = tuple(range(16))  # D0..D15, POD1 = D0-D7, POD2 = D8-D15
LABEL_MAX_LEN = 10  # scope truncates (and uppercases) anything longer


class Scope:
    """Keysight/Agilent MSO-X 3024A -- 4 analog + 16 digital channels.

    Channel labels are the main pain point this exists to solve (tedious
    via the front panel): label_all() sets any subset of the 20 channels
    in one call and turns label display on. The scope itself uppercases
    labels and truncates them to 10 characters -- see LABEL_MAX_LEN.
    """

    def __init__(self, resource=SCOPE_RESOURCE, rm=None):
        self.rm = rm or pyvisa.ResourceManager()
        self.h = self.rm.open_resource(resource)
        self.h.timeout = 5000

    def idn(self):
        return self.h.query("*IDN?").strip()

    # ---- channel labels ----

    def set_analog_label(self, ch, label):
        if ch not in ANALOG_CHANNELS:
            raise ValueError(f"analog channel must be one of {ANALOG_CHANNELS}, got {ch}")
        self.h.write(f':CHANnel{ch}:LABel "{label[:LABEL_MAX_LEN]}"')

    def set_digital_label(self, ch, label):
        if ch not in DIGITAL_CHANNELS:
            raise ValueError(f"digital channel must be 0..15, got {ch}")
        self.h.write(f':DIGital{ch}:LABel "{label[:LABEL_MAX_LEN]}"')

    def get_analog_label(self, ch):
        return self.h.query(f":CHANnel{ch}:LABel?").strip().strip('"')

    def get_digital_label(self, ch):
        return self.h.query(f":DIGital{ch}:LABel?").strip().strip('"')

    def show_labels(self, on=True):
        self.h.write(f":DISPlay:LABel {'ON' if on else 'OFF'}")

    def enable_analog(self, ch, on=True):
        if ch not in ANALOG_CHANNELS:
            raise ValueError(f"analog channel must be one of {ANALOG_CHANNELS}, got {ch}")
        self.h.write(f":CHANnel{ch}:DISPlay {'ON' if on else 'OFF'}")

    def enable_digital(self, ch, on=True):
        """Turning a digital channel's *trace* on/off (independent of its
        label, and independent of the pod-level enable below -- a channel
        needs both its pod and itself on to actually show on screen)."""
        if ch not in DIGITAL_CHANNELS:
            raise ValueError(f"digital channel must be 0..15, got {ch}")
        self.h.write(f":DIGital{ch}:DISPlay {'ON' if on else 'OFF'}")

    def enable_pod(self, pod, on=True):
        """pod 1 = D0-D7, pod 2 = D8-D15."""
        if pod not in (1, 2):
            raise ValueError(f"pod must be 1 or 2, got {pod}")
        self.h.write(f":POD{pod}:DISPlay {'ON' if on else 'OFF'}")

    def label_all(self, analog=None, digital=None, show=True, enable=True):
        """analog: {1: 'S1', 2: 'S2', ...}   digital: {0: 'SCK', 1: 'MOSI', ...}

        enable=True also turns on the trace for every channel named here
        (and both pods, if any digital channel is named) -- a labeled but
        disabled channel is invisible on screen, which defeats the point.
        """
        for ch, name in (analog or {}).items():
            self.set_analog_label(ch, name)
            if enable:
                self.enable_analog(ch, True)
        if digital:
            if enable:
                self.enable_pod(1, True)
                self.enable_pod(2, True)
            for ch, name in digital.items():
                self.set_digital_label(ch, name)
                if enable:
                    self.enable_digital(ch, True)
        self.show_labels(show)

    # ---- measurement ----

    def measure_vamplitude(self, ch):
        return float(self.h.query(f":MEASure:VAMPlitude? CHANnel{ch}"))

    def measure_vpp(self, ch):
        return float(self.h.query(f":MEASure:VPP? CHANnel{ch}"))

    def measure_frequency(self, ch):
        return float(self.h.query(f":MEASure:FREQuency? CHANnel{ch}"))

    def measure_phase(self, ch_a, ch_b):
        return float(self.h.query(f":MEASure:PHASe? CHANnel{ch_a},CHANnel{ch_b}"))

    # ---- capture ----

    def screenshot_png(self, path):
        """Save the current screen (incl. channel labels) as a PNG."""
        data = self.h.query_binary_values(
            ":DISPlay:DATA? PNG, COLor", datatype="B", container=bytes
        )
        with open(path, "wb") as f:
            f.write(data)

    def close(self):
        self.h.close()


if __name__ == "__main__":
    # Quick reachability check for both instruments -- read-only.
    rm = pyvisa.ResourceManager()
    print("VISA resources:", rm.list_resources())
    try:
        psu = Psu(rm=rm)
        print("PSU :", psu.idn())
        psu.close()
    except Exception as e:
        print("PSU : error --", e)
    try:
        scope = Scope(rm=rm)
        print("Scope:", scope.idn())
        scope.close()
    except Exception as e:
        print("Scope: error --", e)
