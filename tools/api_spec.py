"""Single source of truth for the device API (v3).

`python tools/gen_api.py` turns this into
  Services/svc_api_defs.h        IDs, opcodes, packed payload structs, handler prototypes
  Services/svc_api_tables.c      the resource tables the dispatcher walks (settings/calibration rows included)
  PythonTestCode/apiv3_defs.py   constants, encoders and decoders for the host side
  docs/api-reference.md          the reference tables
`python tools/gen_api.py --check` fails if the committed files are out of date (run by the host tests' `make`).

The framing, verbs, status codes, dispatch order and subscription rules are in docs/api-v3-spec.md.

Conventions
  * Wire byte order is little-endian, floats are IEEE-754 binary32.
  * Every response payload starts with a status byte (Api2Status); the layouts below describe what follows it when the
    status is OK.
  * Resource IDs of state-changing resources (SET / EXECUTE) are >= 0x40 so that a stale v2 client, whose IDs were all
    below 0x40, gets UNKNOWN_RESOURCE instead of triggering something else.
"""

U8, I8, U16, I16, U32, I32, F32 = "u8", "i8", "u16", "i16", "u32", "i32", "f32"


def S(n):
    """Fixed-width ASCII string field of n bytes (NUL padded)."""
    return "char[%d]" % n


class Field:
    def __init__(self, ctype, name, doc="", unit=""):
        self.ctype, self.name, self.doc, self.unit = ctype, name, doc, unit


F = Field


class Res:
    """A resource.

    verbs     subset of GET SET EXECUTE SUBSCRIBE UNSUBSCRIBE START_BULK CANCEL_BULK
    rsp       fields of the GET / EXECUTE response payload (after the status byte)
    req       fields of the SET / EXECUTE request payload
    req_len   (min, max) request length instead of the fields' size (variable-length requests)
    push      fields of a subscription push (after [issue_seq][page]); defaults to rsp
    sub       None | 'interval' | 'event'   how SUBSCRIBE delivers
    sub_req   fields of the SUBSCRIBE request (default: u32 interval_ms for 'interval' and 'event')
    ev        name of the event-ops table for sub == 'event' (api_ev_<ev>)
    service   True: SET / EXECUTE answer SERVICE_MODE_REQUIRED unless service mode is active (see api-v3-spec.md,
              section 5a); GET and the other verbs are never gated
    """

    def __init__(self, name, rid, verbs, doc, rsp=None, req=None, req_len=None, push=None,
                 sub=None, sub_req=None, ev=None, service=False):
        self.service = service
        self.name, self.id, self.verbs, self.doc = name, rid, tuple(verbs), doc
        self.rsp, self.req, self.req_len, self.push = rsp, req, req_len, push
        self.sub, self.sub_req, self.ev = sub, sub_req, ev
        self.kind = "custom"
        self.cat = None


class FieldRes(Res):
    """A GET/SET resource backed by one DeviceSettings member (generic handler, bounds checked)."""

    def __init__(self, name, rid, ctype, field, lo, hi, doc, unit="", check=None, after=None, service=False):
        super().__init__(name, rid, ("GET", "SET"), doc, rsp=[F(ctype, "value", doc, unit)],
                         req=[F(ctype, "value", doc, unit)], service=service)
        self.kind = "field"
        self.ctype, self.field, self.lo, self.hi = ctype, field, lo, hi
        self.check, self.after, self.unit = check, after, unit


class Cat:
    def __init__(self, cid, name, title, verbs, doc, resources):
        self.id, self.name, self.title, self.verbs, self.doc = cid, name, title, tuple(verbs), doc
        self.resources = resources
        for r in resources:
            r.cat = self


GET, SET, EXECUTE, SUBSCRIBE, UNSUBSCRIBE, START_BULK, CANCEL_BULK = (
    "GET", "SET", "EXECUTE", "SUBSCRIBE", "UNSUBSCRIBE", "START_BULK", "CANCEL_BULK")
VERB_VALUE = {GET: 0, SET: 1, EXECUTE: 2, SUBSCRIBE: 3, UNSUBSCRIBE: 4, START_BULK: 5, CANCEL_BULK: 6}

# ---------------------------------------------------------------------------------------------------------------------
SYSTEM = Cat(0x0, "SYSTEM", "System", (GET, SET),
  "Identity, live device state, health and the real-time clock. Read-only except the RTC.", [
  Res("IDENTITY", 0x00, (GET,), "Who and what is this: firmware, protocol version, serial number.",
      rsp=[F(U8, "fw_major"), F(U8, "fw_minor"), F(U8, "fw_patch"),
           F(U8, "api_version", "protocol version, 3"),
           F(U16, "max_payload", "largest payload any packet may carry (122)", "B"),
           F(S(16), "product"), F(S(8), "serial", "8 hex digits of the factory UID, identical to the USB serial"),
           F(S(20), "build", "build id: the git commit the firmware was built from (8 hex digits), '-dirty' appended if the "
                       "working tree had uncommitted changes, 'nogit' for a build from a source archive")]),
  Res("STATE", 0x01, (GET,), "Live state: battery, connections, charging, rails and which activities are running. "
      "Also available as the periodic topic STATUS.",
      rsp=[F(U8, "battery_state", "0 normal, 1 low, 2 critical, 3 charging, 4 full"),
           F(U8, "battery_soc_pct", "state of charge", "%"),
           F(U16, "battery_mv", "", "mV"),
           F(U8, "usb_connected", "USB power present (VBUS)"),
           F(U8, "ble_connected"),
           F(U8, "charging"),
           F(U8, "force_charging", "a forced charge is armed (Commands FORCE_CHARGE)"),
           F(U8, "charge_inhibited", "charging is inhibited (Commands CHARGE_INHIBIT)"),
           F(U8, "rail_3v3_on", "switched 3V3 rail (EEPROM, BME280, battery divider)"),
           F(U8, "rail_5v_on", "5 V rail (display, analog front end, buzzer)"),
           F(U8, "demod_running", "the tilt demodulation is running"),
           F(U8, "phasor_stream_active", "a phasor stream subscription owns the ADC"),
           F(U8, "bulk_active", "a bulk transfer is active"),
           F(U8, "service_mode", "service mode is active (unlocks the gated commands and calibration writes)")]),
  Res("HEALTH", 0x02, (GET,), "Self-test results, error latches, communication counters, uptime and reset cause.",
      rsp=[F(U8, "adc_ok", "internal ADC calibrated at boot"),
           F(U8, "dac_ok", "AD9833 initialised"),
           F(U8, "ads_ok", "ADS131M04 initialised and answering"),
           F(U8, "display_ok"),
           F(U8, "bme280_ok", "BME280 present and read recently"),
           F(U8, "eeprom_selftest", "0 not run, 1 passed, 2 failed"),
           F(U8, "settings_save_failed", "latched: an EEPROM settings write failed since boot"),
           F(U8, "woke_from_standby"),
           F(U8, "reset_cause", "bit0 pin, bit1 power-on/brown-out, bit2 software, bit3 IWDG, bit4 WWDG, "
                               "bit5 low-power, bit6 option-byte load"),
           F(U8, "rtc_set", "the clock has been set since the backup domain last lost power"),
           F(U32, "uptime_s", "", "s"),
           F(U16, "usb_tx_dropped", "frames lost to a full USB transmit ring or too large for it"),
           F(U16, "ble_tx_dropped"),
           F(U16, "uart_tx_dropped"),
           F(U16, "rx_malformed", "received frames that could not be parsed"),
           F(U8, "last_fault_kind", "the fault that caused the previous reset: 0 none, 1 HardFault, 2 init failure "
                                    "(Error_Handler); a watchdog reset shows as reset_cause bit3/bit4 with kind 0"),
           F(U32, "last_fault_pc", "program counter at the fault"),
           F(U32, "last_fault_lr", "link register at the fault")]),
  Res("RTC", 0x03, (GET, SET), "Date and time. The clock has a 1/256 s sub-second counter, so drift can be measured "
      "to well under 1 ppm over a day (see Calibrations RTC_TRIM).",
      rsp=[F(U16, "year"), F(U8, "month"), F(U8, "day"), F(U8, "weekday", "1 = Monday .. 7 = Sunday"),
           F(U8, "hour"), F(U8, "minute"), F(U8, "second"),
           F(U8, "is_set", "0 = never set since power-up"),
           F(U8, "subsecond", "1/256 s, counting up")],
      req=[F(U16, "year", "2000..2099"), F(U8, "month"), F(U8, "day"), F(U8, "hour"), F(U8, "minute"),
           F(U8, "second")]),
])

# ---------------------------------------------------------------------------------------------------------------------
COMMANDS = Cat(0x1, "COMMANDS", "Commands", (EXECUTE,),
  "One-shot actions. Every command answers with a status byte (plus the payload noted). IDs by group: 0x40 power and "
  "charging, 0x50 measurement, 0x60 maintenance, 0x70 debug.", [
  Res("POWER_OFF", 0x40, (EXECUTE,), "Enter Standby now (wake: encoder press or USB plug-in). Answers first, then sleeps.",
      req=[]),
  Res("REBOOT", 0x41, (EXECUTE,), "Software reset. Answers first, then resets.", req=[]),
  Res("REBOOT_DFU", 0x42, (EXECUTE,), service=True, doc=
      "Reset into the ROM USB bootloader (VID 0x0483 / PID 0xDF11). Sets the nBOOT0 option byte to 0: the device STAYS "
      "in the bootloader on every boot until reflashed with nBOOT0 restored "
      "(`STM32_Programmer_CLI -c port=USB1 -w fw.hex -ob nSWBOOT0=1 nBOOT0=1 -v -rst`, or dfu_flash.ps1).", req=[]),
  Res("FORCE_CHARGE", 0x43, (EXECUTE,), "Charge regardless of the state of charge while USB power is present, until "
      "full or USB removal.", req=[]),
  Res("END_CHARGING", 0x44, (EXECUTE,), "Cancel an armed forced charge. Does not stop normal automatic charging "
      "(use CHARGE_INHIBIT).", req=[]),
  Res("CHARGE_INHIBIT", 0x45, (EXECUTE,), "Hold charging off (also cancels a forced charge). Stays in effect until "
      "cleared; not cleared by a USB replug.", req=[F(U8, "inhibit", "1 = inhibit, 0 = allow")]),
  Res("TILT_DEMOD", 0x50, (EXECUTE,), "Start or stop the tilt demodulation (runs from boot). BUSY_EXCLUSIVE while "
      "a bulk capture or the phasor stream owns the ADC.", req=[F(U8, "run", "1 = run, 0 = stop")]),
  Res("ZERO_CAL", 0x51, (EXECUTE,), service=True, doc="Flip (zero) calibration, the 180-degree reversal test. Step 1 averages the "
      "reading in the current orientation (optionally only the selected sensors), step 2 after turning the "
      "instrument 180 degrees averages again and stores the new zero of those sensors (Calibrations S1_ZERO / "
      "S2_ZERO). Progress: Procedures ZERO_CAL.",
      req=[F(U8, "action", "0 cancel, 1 step 1, 2 step 2"),
           F(U8, "sensor_mask", "step 1 only, optional: bit0 S1, bit1 S2 (default 3 = both)")], req_len=(1, 2)),
  Res("PRECISION", 0x52, (EXECUTE,), "Triggered precision measurement: one reliable value from the first clean 2 s "
      "window within 5 s, else an error. Needs the demodulation running; BUSY_RESOURCE during a zero calibration. "
      "Result: Procedures PRECISION.", req=[F(U8, "action", "0 start, 1 cancel")]),
  Res("FACTORY_DEFAULTS", 0x60, (EXECUTE,), service=True, doc="Reset ALL settings and calibrations to the compiled defaults and save "
      "them. Wipes the tilt calibrations: use with care.",
      req=[F(U8, "confirm", "must be 0xA5")]),
  Res("SERVICE_END", 0x62, (EXECUTE,), "Leave service mode now (it also ends by itself, see the protocol document). "
      "Entering it is only possible on the instrument: SETTINGS screen, Service mode.", req=[]),
  Res("CLEAR_COUNTERS", 0x61, (EXECUTE,), "Zero the communication, drop and fault counters and the sticky error "
      "latches (settings_save_failed).", req=[]),
  Res("TEST_BEEP", 0x70, (EXECUTE,), "Sound the buzzer for 100 ms.", req=[]),
  Res("POWER_TEST", 0x71, (EXECUTE,), service=True, doc="Power investigation: each bit of the mask keeps one subsystem on, cleared "
      "bits cut it immediately. Boot default is all bits set.",
      req=[F(U32, "mask", "bit0 5V rail, 1 3V3 rail, 2 AD9833, 3 ADS131M04, 4 BLE, 5 display, 6 LEDs, "
                         "7 CPU busy-loop (0 = WFI between ticks)")],
      rsp=[F(U32, "applied_mask")]),
  Res("PIN_TEST", 0x72, (EXECUTE,), service=True, doc="Drive the six MCU-to-level-converter lines as static outputs. Arming is "
      "irreversible without the reboot bit.",
      req=[F(U8, "pins", "bits 5..0: SCK, MOSI, CS, DISP_ON, VCOM, BUZZER; bit6 allow DISP_ON high (panel MUST be "
                         "unplugged); bit7 reboot to normal")]),
  Res("FAULT_TEST", 0x76, (EXECUTE,), service=True, doc="Provoke a failure on purpose, to prove the fault capture and the "
      "watchdog on a bench (answers first, then misbehaves). 1: a HardFault (recorded, then the instrument resets; "
      "System HEALTH shows it after the reboot). 2: the main loop hangs with interrupts running (the supervised watchdog "
      "resets after about 3 s; reset_cause shows a WWDG reset). 3: a hang with interrupts disabled (the hardware watchdog "
      "resets within about half a second).",
      req=[F(U8, "kind", "1 HardFault, 2 main-loop hang, 3 hang with interrupts off")]),
  Res("RAIL", 0x73, (EXECUTE,), service=True, doc="Switch one supply rail (shortcut for POWER_TEST bits 0 and 1).",
      req=[F(U8, "rail", "0 = 3V3 rail, 1 = 5V rail"), F(U8, "on", "1 on, 0 off")],
      rsp=[F(U32, "applied_mask", "the resulting POWER_TEST mask")]),
])

# ---------------------------------------------------------------------------------------------------------------------
_DS = "disp_%s_%s"
def _cal(*a, **k):
    """A calibration field: writing it needs service mode."""
    return FieldRes(*a, service=True, **k)


CALIBRATIONS = Cat(0x2, "CALIBRATIONS", "Calibrations", (GET, SET),
  "Constants that correct a sensor or a measurement. Stored in EEPROM; every SET persists immediately. IDs by group: "
  "0x40 tilt sensors, 0x50 battery, 0x60 temperature, 0x70 clock.", [
  _cal("S1_K", 0x40, I32, "disp_s1_k_micro", 100, 1000000,
           "Sensor 1 sensitivity k at PGA 1: tilt [mm/m] = (ratio - zero) / k", "1e-6 per mm/m"),
  _cal("S1_ZERO", 0x41, I32, "disp_s1_zero_ppm", -500000, 500000,
           "Sensor 1 zero (level point) as a ratio, independent of k", "ppm of the ratio"),
  _cal("S1_PHASE", 0x42, I16, "disp_s1_phase_cdeg", -4500, 4500,
           "Sensor 1 phase of the tilt signal relative to the excitation reference", "0.01 deg"),
  _cal("S1_INVERT", 0x43, U8, "disp_s1_invert", 0, 1, "Sensor 1 sign of the reported reading (1 = inverted)"),
  _cal("S2_K", 0x44, I32, "disp_s2_k_micro", 100, 1000000, "Sensor 2 k at PGA 1", "1e-6 per mm/m"),
  _cal("S2_ZERO", 0x45, I32, "disp_s2_zero_ppm", -500000, 500000, "Sensor 2 zero", "ppm of the ratio"),
  _cal("S2_PHASE", 0x46, I16, "disp_s2_phase_cdeg", -4500, 4500, "Sensor 2 phase", "0.01 deg"),
  _cal("S2_INVERT", 0x47, U8, "disp_s2_invert", 0, 1, "Sensor 2 sign of the reported reading"),
  _cal("VBAT_SCALE_NUM", 0x50, U16, "vbat_scale_num", 1, 10000, "Battery divider scale numerator "
           "(Vbat = Vadc * num / den + offset)"),
  _cal("VBAT_SCALE_DEN", 0x51, U16, "vbat_scale_den", 1, 10000, "Battery divider scale denominator"),
  _cal("VBAT_OFFSET", 0x52, I32, "vbat_offset_mv", -500, 500, "Battery voltage offset", "mV"),
  _cal("TMP236_SEG1_VOFFS", 0x60, U16, "tmp236_seg1_voffs_mv", 0, 3300, "TMP236 segment 1 voltage offset", "mV"),
  _cal("TMP236_SEG1_NUM", 0x61, U16, "tmp236_seg1_num", 1, 10000, "TMP236 segment 1 slope numerator"),
  _cal("TMP236_SEG1_DEN", 0x62, U16, "tmp236_seg1_den", 1, 10000, "TMP236 segment 1 slope denominator"),
  _cal("TMP236_SEG_BOUNDARY", 0x63, U16, "tmp236_seg_boundary_mv", 0, 3300, "TMP236 segment boundary", "mV"),
  _cal("TMP236_SEG2_VOFFS", 0x64, U16, "tmp236_seg2_voffs_mv", 0, 3300, "TMP236 segment 2 voltage offset", "mV"),
  _cal("TMP236_SEG2_NUM", 0x65, U16, "tmp236_seg2_num", 1, 10000, "TMP236 segment 2 slope numerator"),
  _cal("TMP236_SEG2_DEN", 0x66, U16, "tmp236_seg2_den", 1, 10000, "TMP236 segment 2 slope denominator"),
  _cal("TMP236_SEG2_TINFL", 0x67, U16, "tmp236_seg2_tinfl_cdeg", 0, 20000, "TMP236 segment 2 inflection "
           "temperature", "0.01 degC"),
  _cal("LM35_SCALE", 0x68, U16, "lm35_scale_mv_per_c", 1, 1000, "External LM35 scale", "mV per degC"),
  _cal("RTC_TRIM", 0x70, I16, "rtc_trim_ppm_x10", -4800, 4800,
           "Clock trim: positive makes the clock run FASTER. Applied by the RTC's digital calibration "
           "(0.954 ppm per step, range about +-488 ppm, rounded to the nearest step). Measure the error as "
           "(RTC time - reference time) / elapsed time over several hours (System RTC has 1/256 s resolution) and "
           "SET the opposite sign. The crystal drifts with temperature (about -0.034 ppm/degC^2 around its turnover "
           "point), so calibrate near the working temperature.", "0.1 ppm",
           after="rtc_trim"),
])

SETTINGS = Cat(0x3, "SETTINGS", "Settings", (GET, SET),
  "Behaviour: how often tasks run, thresholds, timeouts. Stored in EEPROM; every SET persists immediately. IDs by "
  "group: 0x40 task periods, 0x50 battery, 0x60 user interface and power.", [
  FieldRes("TASK_SENSORS", 0x40, U16, "task_sensors_ms", 1, 1000, "Sensor task period", "ms"),
  FieldRes("TASK_BLE", 0x41, U16, "task_ble_ms", 1, 250, "BLE task period", "ms"),
  FieldRes("TASK_USB", 0x42, U16, "task_usb_ms", 1, 250, "USB task period", "ms"),
  FieldRes("TASK_BATTERY", 0x43, U16, "task_battery_ms", 100, 10000, "Battery task period", "ms"),
  FieldRes("TASK_TEMPERATURE", 0x44, U16, "task_temperature_ms", 100, 60000, "Temperature task period", "ms"),
  FieldRes("BATTERY_CRITICAL", 0x50, U16, "battery_critical_mv", 2500, 3600,
           "Below this the instrument powers off (must stay below BATTERY_LOW)", "mV", check="battery_order"),
  FieldRes("BATTERY_LOW", 0x51, U16, "battery_low_mv", 3000, 4200,
           "Below this the low-battery warning shows (must stay above BATTERY_CRITICAL)", "mV",
           check="battery_order"),
  FieldRes("BATTERY_CHARGE_START", 0x52, U16, "battery_charge_start_mv", 3000, 4200,
           "Below this, with USB power present, charging starts", "mV"),
  FieldRes("AUTO_POWEROFF", 0x60, U16, "auto_poweroff_s", 0, 65535,
           "Idle time with no encoder input before the instrument powers off (0 = never)", "s"),
  FieldRes("ENCODER_COUNTS_PER_DETENT", 0x61, U16, "encoder_counts_per_detent", 1, 100,
           "Raw quadrature transitions per mechanical detent of the rotary encoders"),
])

# ---------------------------------------------------------------------------------------------------------------------
_ENV = [F(I16, "bme280_temp", "", "0.01 degC"), F(U32, "bme280_pressure", "", "Pa"),
        F(U16, "bme280_humidity", "", "0.01 %RH"), F(U8, "bme280_ok"),
        F(I16, "onboard_temp", "TMP236 on the board", "0.01 degC"),
        F(I16, "external_temp", "LM35 on the external connector", "0.01 degC"), F(U8, "external_temp_ok")]
_TILT_NOTE = ("Hann-weighted tilt reading, about 4.07 updates/s (delay ~0.3 s). Valid while TILT_FLAGS bit0 is set.")

MEASUREMENTS = Cat(0x4, "MEASUREMENTS", "Measurements", (GET, SUBSCRIBE, UNSUBSCRIBE),
  "Single values. GET reads one; SUBSCRIBE (payload: u32 interval_ms, 50..3 600 000) pushes it periodically as "
  "[issue_seq][page][value] under the same opcode. For several values at once use Topics.", [
  Res("ONBOARD_TEMP", 0x00, (GET, SUBSCRIBE, UNSUBSCRIBE), "On-board TMP236 temperature.",
      rsp=[F(I16, "value", "", "0.01 degC")], sub="interval"),
  Res("BATTERY_MV", 0x01, (GET, SUBSCRIBE, UNSUBSCRIBE), "Battery voltage.", rsp=[F(U16, "value", "", "mV")],
      sub="interval"),
  Res("BATTERY_SOC", 0x02, (GET, SUBSCRIBE, UNSUBSCRIBE), "Battery state of charge.", rsp=[F(U8, "value", "", "%")],
      sub="interval"),
  Res("BME_TEMP", 0x03, (GET, SUBSCRIBE, UNSUBSCRIBE), "BME280 temperature.", rsp=[F(I16, "value", "", "0.01 degC")],
      sub="interval"),
  Res("BME_PRESSURE", 0x04, (GET, SUBSCRIBE, UNSUBSCRIBE), "BME280 pressure.", rsp=[F(U32, "value", "", "Pa")],
      sub="interval"),
  Res("BME_HUMIDITY", 0x05, (GET, SUBSCRIBE, UNSUBSCRIBE), "BME280 relative humidity.",
      rsp=[F(U16, "value", "", "0.01 %RH")], sub="interval"),
  Res("BME_OK", 0x06, (GET, SUBSCRIBE, UNSUBSCRIBE), "BME280 reading is fresh.", rsp=[F(U8, "value")],
      sub="interval"),
  Res("EXT_TEMP", 0x07, (GET, SUBSCRIBE, UNSUBSCRIBE), "External LM35 temperature.",
      rsp=[F(I16, "value", "", "0.01 degC")], sub="interval"),
  Res("EXT_TEMP_OK", 0x08, (GET, SUBSCRIBE, UNSUBSCRIBE), "External temperature reading is in range.",
      rsp=[F(U8, "value")], sub="interval"),
  Res("TILT_S1", 0x09, (GET, SUBSCRIBE, UNSUBSCRIBE), "Sensor 1 tilt. " + _TILT_NOTE,
      rsp=[F(F32, "value", "", "mm/m")], sub="interval"),
  Res("TILT_S2", 0x0A, (GET, SUBSCRIBE, UNSUBSCRIBE), "Sensor 2 tilt. " + _TILT_NOTE,
      rsp=[F(F32, "value", "", "mm/m")], sub="interval"),
  Res("TILT_DIFF", 0x0B, (GET, SUBSCRIBE, UNSUBSCRIBE), "Differential S1 - S2, computed from the two sign-corrected "
      "values.", rsp=[F(F32, "value", "", "mm/m")], sub="interval"),
  Res("TILT_FLAGS", 0x0C, (GET, SUBSCRIBE, UNSUBSCRIBE), "Quality flags of the tilt readings.",
      rsp=[F(U8, "value", "bit0 reading valid (demodulation running and a display value exists), bit1 sensor 1 "
                          "doubtful, bit2 sensor 2 doubtful (the window's quadrature noise exceeds 6x the "
                          "instrument's quiet floor: show the value but flag it)")], sub="interval"),
  Res("TILT_SEQ", 0x0D, (GET, SUBSCRIBE, UNSUBSCRIBE), "Counts every new display value; compare it to tell a new "
      "reading from a repeated one.", rsp=[F(U16, "value")], sub="interval"),
])

_STATE_FIELDS = SYSTEM.resources[1].rsp
TOPICS = Cat(0x5, "TOPICS", "Topics", (GET, SUBSCRIBE, UNSUBSCRIBE),
  "Fixed bundles of related values, read or pushed atomically. Interval topics take SUBSCRIBE u32 interval_ms; "
  "event topics push when something new exists and ignore the interval (the field must still be present).", [
  Res("ENV", 0x00, (GET, SUBSCRIBE, UNSUBSCRIBE), "All environmental sensors.", rsp=_ENV, sub="interval"),
  Res("STATUS", 0x01, (GET, SUBSCRIBE, UNSUBSCRIBE), "Same payload as System STATE.", rsp=_STATE_FIELDS,
      sub="interval"),
  Res("LIVE", 0x02, (GET, SUBSCRIBE, UNSUBSCRIBE),
      "The live tilt reading with its quality flags, atomically. SUBSCRIBE pushes once per new display value "
      "(about 4.07/s). THE topic for a level display.",
      rsp=[F(U16, "seq", "increments with every new display value"),
           F(U8, "flags", "bit0 valid, bit1 sensor 1 doubtful, bit2 sensor 2 doubtful"),
           F(F32, "tilt_s1", "", "mm/m"), F(F32, "tilt_s2", "", "mm/m"), F(F32, "tilt_diff", "S1 - S2", "mm/m")],
      sub="event", ev="live"),
  Res("RAW", 0x03, (GET, SUBSCRIBE, UNSUBSCRIBE),
      "Per-batch (about 40.7/s) unsmoothed tilt and residual, for analysis. SUBSCRIBE at 50 ms to follow it "
      "essentially 1:1.",
      rsp=[F(F32, "tilt_s1", "", "mm/m"), F(F32, "residual_s1", "Im(x) quadrature part, near 0 when calibrated"),
           F(F32, "tilt_s2", "", "mm/m"), F(F32, "residual_s2"),
           F(U8, "window_ok_s1", "the newest display window of sensor 1 was not doubtful"),
           F(U8, "window_ok_s2"),
           F(F32, "tilt_diff", "", "mm/m"), F(U8, "window_ok_diff", "both sensors OK")],
      sub="interval"),
  Res("PHASOR_STREAM", 0x04, (SUBSCRIBE, UNSUBSCRIBE),
      "Gapless stream of every 64-cycle batch's raw phasors with a sequence number (about 40.7 pushes/s). The "
      "subscription arms the ADC for the stream; the demodulation keeps running. A gap in `seq` (modulo 65536, step "
      "64) means lost batches. Event topic: the interval field is ignored.",
      push=[F(F32, "iB"), F(F32, "qB"), F(F32, "iA"), F(F32, "qA"), F(F32, "iS1"), F(F32, "qS1"), F(F32, "iS2"),
            F(F32, "qS2"), F(U16, "seq", "cycle counter of the batch's last cycle (steps of 64, wraps at 65536)")],
      sub="event", ev="phasor_stream"),
  Res("SIGNAL_DIAG", 0x05, (GET, SUBSCRIBE, UNSUBSCRIBE),
      "Per-channel amplitude, peak-to-peak and phase at the ADC pins (channel order S2, B, A, S1), and the "
      "theoretical tilt each sensor channel implies (Wyler 20 uV per um/m).",
      rsp=[F(F32, "rms_mv_ch0", "", "mV"), F(F32, "rms_mv_ch1", "", "mV"), F(F32, "rms_mv_ch2", "", "mV"),
           F(F32, "rms_mv_ch3", "", "mV"),
           F(F32, "p2p_mv_ch0", "", "mV"), F(F32, "p2p_mv_ch1", "", "mV"), F(F32, "p2p_mv_ch2", "", "mV"),
           F(F32, "p2p_mv_ch3", "", "mV"),
           F(F32, "phase_deg_ch0", "ch2 = 0 by definition", "deg"), F(F32, "phase_deg_ch1", "", "deg"),
           F(F32, "phase_deg_ch2", "", "deg"), F(F32, "phase_deg_ch3", "", "deg"),
           F(F32, "theoretical_tilt_s1", "", "mm/m"), F(F32, "theoretical_tilt_s2", "", "mm/m")],
      sub="interval"),
])

DEBUG = Cat(0x6, "DEBUG", "Debug log", (SUBSCRIBE, UNSUBSCRIBE),
  "The device log, streamed live (Services/svc_log.c).", [
  Res("LOG", 0x00, (SUBSCRIBE, UNSUBSCRIBE),
      "Log lines of at least the requested severity. A subscription flushes the backlog still held on the device first. "
      "Each push: [issue_seq][page][severity][text, no terminator, up to 48 bytes].",
      sub="event", ev="log", sub_req=[F(U8, "min_severity", "0 info, 1 warning, 2 error")],
      push=[F(U8, "severity", "0 info, 1 warning, 2 error"), F(S(0), "text", "remainder of the payload")]),
])

DIAGNOSTICS = Cat(0x7, "DIAGNOSTICS", "Diagnostics", (GET,),
  "Counters and registers for investigating the acquisition chain. Read-only.", [
  Res("ADC", 0x00, (GET,), "ADS131M04 registers, acquisition integrity counters and the last bulk capture's statistics.",
      rsp=[F(U16, "id"), F(U16, "status"), F(U16, "mode"), F(U16, "clock"), F(U16, "gain1"), F(U16, "cfg"),
           F(U16, "clock_expected", "the CLOCK value the driver wrote; != clock means a register write did not land"),
           F(U8, "regs_read_ok"), F(U8, "ads_ok"),
           F(U16, "last_capture_samples"), F(U16, "last_capture_drops"), F(U32, "last_capture_ms"),
           F(U32, "frames_produced"), F(U32, "frames_drained"), F(U32, "tim7_fires"), F(U32, "ring_overflow"),
           F(U32, "drain_clamped"), F(U32, "framing_err"), F(U32, "crc_err"), F(U32, "run_ms"),
           F(I32, "frame_deficit"), F(I32, "frame_deficit_min"), F(I32, "frame_deficit_max"),
           F(U16, "drain_clamp_max"), F(U16, "word0_last"), F(U16, "crc_rx_last"), F(U16, "crc_calc_last"),
           F(U8, "fault_code", "0 none, 1 overrun, 2 framing, 3 crc, 4 slip"),
           F(U32, "now_ms", "device tick, a clock-independent time base for rate checks")]),
  Res("POWER", 0x01, (GET,), "Power-test mask and the switched rails.",
      rsp=[F(U32, "mask"), F(U8, "rails", "bit0 3V3 rail on, bit1 5V rail on")]),
  Res("TILT_DEMOD", 0x02, (GET,), "Tilt demodulation counters and scheduler timing.",
      rsp=[F(U16, "input_drop", "raw cycles dropped because the batch ring was full (multiples of 64)"),
           F(U16, "degenerate", "batches skipped because A == B"),
           F(U16, "clip", "raw samples within 1 % of ADC full scale"),
           F(U16, "amplitude_fault", "batch phasors above the theoretical maximum (data integrity)"),
           F(U16, "max_update_gap_ms", "longest gap between scheduler visits of the demodulation task"),
           F(U32, "max_gap_at_uptime_ms"),
           F(U16, "gap_over_threshold", "visits with a gap above 40 ms"),
           F(U8, "running")]),
])

BULK = Cat(0x8, "BULK", "Bulk transfers", (START_BULK, CANCEL_BULK),
  "Large RAM-buffered transfers streamed in chunks under the START opcode, paced by the transport's transmit ring.", [
  Res("RAW_ADC", 0x00, (START_BULK, CANCEL_BULK),
      "Raw ADC capture: 6144 samples x 4 channels x 24 bit (about 295 ms at 20.83 kHz), packed little-endian signed, "
      "channel order S2, B, A, S1. After the OK ack the chunks follow: [status][page][10 samples x 12 bytes]. "
      "Device-wide exclusive; stops the demodulation for the duration.",
      push=[F(U8, "page", "wrapping chunk counter")]),
])

PROCEDURES = Cat(0x9, "PROCEDURES", "Procedures", (GET, SUBSCRIBE, UNSUBSCRIBE),
  "Status and results of multi-second procedures started with a command. GET polls; SUBSCRIBE (u32, ignored) pushes "
  "when the phase changes and about every 250 ms while running.", [
  Res("ZERO_CAL", 0x00, (GET, SUBSCRIBE, UNSUBSCRIBE), "Flip (zero) calibration status (Commands ZERO_CAL).",
      rsp=[F(U8, "phase", "0 idle, 1 step 1 running, 2 step 1 done (turn the instrument), 3 step 2 running, "
                          "4 result ready (stored automatically, then back to idle)"),
           F(U16, "progress", "batches averaged in the current step"), F(U16, "target"),
           F(U8, "sensor_mask", "sensors covered by the current run")],
      sub="event", ev="zero_cal"),
  Res("PRECISION", 0x01, (GET, SUBSCRIBE, UNSUBSCRIBE), "Precision measurement status and result "
      "(Commands PRECISION).",
      rsp=[F(U8, "phase", "0 idle, 1 running, 2 done"),
           F(U16, "target", "window length in batches (81 = 1.99 s)"),
           F(U16, "count", "contiguous batches collected since the trigger"),
           F(U32, "elapsed_ms"),
           F(U8, "failed", "done without a result: no clean window within 5 s"),
           F(F32, "tilt_s1", "valid when phase == 2 and failed == 0", "mm/m"), F(F32, "tilt_s2", "", "mm/m"),
           F(F32, "tilt_diff", "", "mm/m"),
           F(U8, "disturbed", "while running: the newest full window is not clean")],
      sub="event", ev="precision"),
])

CATEGORIES = [SYSTEM, COMMANDS, CALIBRATIONS, SETTINGS, MEASUREMENTS, TOPICS, DEBUG, DIAGNOSTICS, BULK, PROCEDURES]

# Largest payload a packet may carry (API2_PACKET_MAX_SIZE 128 - header 4 - CRC 2); the status byte takes one of it.
MAX_PAYLOAD = 122
# RTC SET etc. use the request fields; topics push [issue_seq][page] before the payload.
PUSH_PREFIX = 2
API_VERSION = 3
