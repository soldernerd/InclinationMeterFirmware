"""
The checks shared by uart_test.py / hid_test.py / ble_test.py (API v3): one round trip through identity, state, health,
temperature, a settings write/readback, the clock, the environment sensors and the topics; plus the live-reading and
debug-log viewers.

A "link" is anything with  send(pkt: bytes)  and  recv(timeout) -> (opcode, status, data)  (blocking). The BLE script
wraps its asyncio link into that shape.
"""

import datetime
import struct
import time

import apiv3 as a


def request(link, op, payload=b"", timeout=3.0):
    link.send(a.build(op, payload))
    _op, status, data = link.recv(timeout)
    return status, data


def st(status):
    return a.STATUS.get(status, status)


def run_basic(link):
    s, d = request(link, a.OP_SYSTEM_IDENTITY_GET)
    print(f"  IDENTITY      [{st(s)}] {a.format_identity(d) if s == 0 else (d or b'').hex()}")

    s, d = request(link, a.OP_SYSTEM_STATE_GET)
    print(f"  STATE         [{st(s)}] {a.format_state(d) if s == 0 else (d or b'').hex()}")

    s, d = request(link, a.OP_SYSTEM_HEALTH_GET)
    h = a.decode_system_health_response(d) if s == 0 else None
    if h:
        flags = " ".join(f"{k}={h[k]}" for k in ("adc_ok", "dac_ok", "ads_ok", "display_ok", "bme280_ok",
                                                  "eeprom_selftest", "settings_save_failed"))
        print(f"  HEALTH        [OK] {flags}  reset_cause=0x{h['reset_cause']:02X}  uptime={h['uptime_s']}s  "
              f"tx_dropped usb/ble/uart={h['usb_tx_dropped']}/{h['ble_tx_dropped']}/{h['uart_tx_dropped']}  "
              f"rx_malformed={h['rx_malformed']}")
    else:
        print(f"  HEALTH        [{st(s)}]")

    s, d = request(link, a.OP_MEASUREMENTS_ONBOARD_TEMP_GET)
    if s == 0 and len(d) >= 2:
        print(f"  TEMP          [OK] {struct.unpack('<h', d[:2])[0] / 100:+.2f} C (onboard)")
    else:
        print(f"  TEMP          [{st(s)}] {(d or b'').hex()}")

    ste, de = request(link, a.OP_MEASUREMENTS_EXT_TEMP_GET)
    sto, do = request(link, a.OP_MEASUREMENTS_EXT_TEMP_OK_GET)
    if ste == 0 and sto == 0 and len(de) >= 2 and do:
        v = struct.unpack('<h', de[:2])[0] / 100
        print(f"  EXT TEMP      [OK] {v:+.2f} C (LM35)  {'valid' if do[0] else 'out of range / no sensor'}")
    else:
        print(f"  EXT TEMP      [{st(ste)}]")

    # settings: write another value, read it back, restore
    s, d = request(link, a.OP_SETTINGS_AUTO_POWEROFF_GET)
    if s == 0 and len(d) >= 2:
        cur = struct.unpack("<H", d[:2])[0]
        new = 250 if cur != 250 else 200
        s2, _ = request(link, a.OP_SETTINGS_AUTO_POWEROFF_SET, struct.pack("<H", new))
        s3, d3 = request(link, a.OP_SETTINGS_AUTO_POWEROFF_GET)
        back = struct.unpack("<H", d3[:2])[0] if (s3 == 0 and len(d3) >= 2) else None
        time.sleep(0.3)                                      # the EEPROM write of the previous SET must finish
        s4, _ = request(link, a.OP_SETTINGS_AUTO_POWEROFF_SET, struct.pack("<H", cur))
        print(f"  SETTINGS      auto_poweroff_s {cur} -> set {new} [{st(s2)}] -> read back {back}, restored [{st(s4)}]")
    else:
        print(f"  SETTINGS      GET auto_poweroff_s [{st(s)}]")

    # RTC: read, set to this host's wall clock, read back
    s, d = request(link, a.OP_SYSTEM_RTC_GET)
    before = a.format_rtc(d) if s == 0 else st(s)
    n = datetime.datetime.now()
    s2, _ = request(link, a.OP_SYSTEM_RTC_SET, a.build_rtc_set(n.year, n.month, n.day, n.hour, n.minute, n.second))
    s3, d3 = request(link, a.OP_SYSTEM_RTC_GET)
    after = a.format_rtc(d3) if s3 == 0 else st(s3)
    print(f"  RTC           [{before}] -> set [{st(s2)}] -> [{after}]")

    # tilt demodulation: start / stop
    s_on, _ = request(link, a.OP_COMMANDS_DISPLACEMENT_EXECUTE, b"\x01")
    s_live, d_live = request(link, a.OP_TOPICS_LIVE_GET)
    live = a.decode_topics_live_response(d_live) if s_live == 0 else None
    if live:
        print(f"  LIVE          seq {live['seq']}  S1 {live['tilt_s1']:+.4f}  S2 {live['tilt_s2']:+.4f}  "
              f"diff {live['tilt_diff']:+.4f} mm/m   {a.level_state_word(live['flags'])}")
    else:
        print(f"  LIVE          [{st(s_live)}]  (demodulation start [{st(s_on)}])")

    env = read_bme280(link)
    if env is None:
        print("  BME280        GET failed")
    else:
        t, p, hh, ok = env
        print(f"  BME280        {t:+.2f} C  {p:.1f} hPa  {hh:.1f} %RH   [{'fresh' if ok else 'STALE (sensor not connected)'}]")

    s, d = request(link, a.OP_TOPICS_ENV_GET)
    e = a.decode_topics_env_response(d) if s == 0 else None
    if e:
        print(f"  TOPIC env     bme280 {e['bme280_temp'] / 100:+.2f}C {e['bme280_pressure'] / 100:.1f}hPa "
              f"{e['bme280_humidity'] / 100:.1f}% ok={e['bme280_ok']}  onboard {e['onboard_temp'] / 100:+.2f}C  "
              f"ext {e['external_temp'] / 100:+.2f}C ok={e['external_temp_ok']}")
    else:
        print(f"  TOPIC env     [{st(s)}]")
    s, d = request(link, a.OP_TOPICS_STATUS_GET)
    print(f"  TOPIC status  [{st(s)}] {a.format_state(d) if s == 0 else ''}")


def read_bme280(link):
    """GET the four BME280 measurements -> (temp_C, pressure_hPa, humidity_pct, ok) or None."""
    def meas(op):
        s, d = request(link, op)
        return d if s == 0 else None
    dt, dp, dh, dok = (meas(a.OP_MEASUREMENTS_BME_TEMP_GET), meas(a.OP_MEASUREMENTS_BME_PRESSURE_GET),
                       meas(a.OP_MEASUREMENTS_BME_HUMIDITY_GET), meas(a.OP_MEASUREMENTS_BME_OK_GET))
    if None in (dt, dp, dh, dok):
        return None
    return (struct.unpack("<h", dt[:2])[0] / 100, struct.unpack("<I", dp[:4])[0] / 100,
            struct.unpack("<H", dh[:2])[0] / 100, bool(dok[0]))


def run_env(link, period=1.0):
    print("Polling BME280 once/sec. Ctrl+C to stop.\n")
    try:
        while True:
            env = read_bme280(link)
            ts = datetime.datetime.now().strftime("%H:%M:%S")
            if env is None:
                print(f"  {ts}  GET failed")
            else:
                t, p, h, ok = env
                print(f"  {ts}  {t:+6.2f} C   {p:8.2f} hPa   {h:5.1f} %RH   {'' if ok else '[STALE]'}")
            time.sleep(period)
    except KeyboardInterrupt:
        print("\nstopped.")


def run_live(link):
    """Subscribe to the LIVE topic: one push per new display value (about 4/s)."""
    link.send(a.build(a.OP_TOPICS_LIVE_SUBSCRIBE, a.build_interval(1000)))
    link.recv(2.0)
    print("Live reading (Hann display value). Ctrl+C to stop.\n")
    try:
        while True:
            op, s, d = link.recv(5.0)
            if op != a.OP_TOPICS_LIVE_SUBSCRIBE or s != 0 or d is None or len(d) < 2 + a.SIZE_TOPICS_LIVE_RESPONSE:
                continue
            v = a.decode_topics_live_response(d[2:])
            print(f"  #{d[0]:<3} seq {v['seq']:<5} S1 {v['tilt_s1']:+9.4f}  S2 {v['tilt_s2']:+9.4f}  "
                  f"diff {v['tilt_diff']:+9.4f} mm/m   {a.level_state_word(v['flags'])}")
    except KeyboardInterrupt:
        link.send(a.build(a.OP_TOPICS_LIVE_UNSUBSCRIBE))
        time.sleep(0.1)
        print("\nunsubscribed.")


def run_topics(link, interval_ms=1000):
    """Subscribe to the env + status topics and print the pushes."""
    subs = [("env", a.OP_TOPICS_ENV_SUBSCRIBE, a.OP_TOPICS_ENV_UNSUBSCRIBE, a.decode_topics_env_response),
            ("status", a.OP_TOPICS_STATUS_SUBSCRIBE, a.OP_TOPICS_STATUS_UNSUBSCRIBE,
             lambda d: a.format_state(d))]
    by_op = {sub: (name, dec) for name, sub, _u, dec in subs}
    for _n, sub, _u, _d in subs:
        link.send(a.build(sub, a.build_interval(interval_ms)))
        link.recv(2.0)
    print(f"Subscribed to env + status @ {interval_ms} ms. Ctrl+C to stop.\n")
    try:
        while True:
            op, s, d = link.recv(5.0)
            if op not in by_op or s != 0 or d is None or len(d) < 2:
                continue
            name, dec = by_op[op]
            print(f"  [{name:6}] #{d[0]:<3} {dec(d[2:])}")
    except KeyboardInterrupt:
        for _n, _s, unsub, _d in subs:
            link.send(a.build(unsub))
        time.sleep(0.1)
        print("\nunsubscribed.")


def run_log(link, min_sev=0):
    print(f"Subscribing to the debug log (min severity {a.SEVERITY[min_sev]}). Ctrl+C to stop.\n")
    link.send(a.build(a.OP_DEBUG_LOG_SUBSCRIBE, bytes([min_sev])))
    try:
        while True:
            op, s, d = link.recv(5.0)
            if op != a.OP_DEBUG_LOG_SUBSCRIBE or s != 0 or d is None or len(d) < 3:
                continue
            print(f"  [{a.SEVERITY.get(d[2], d[2]):5}] #{d[0]:<3} {d[3:].decode('ascii', 'replace')}")
    except KeyboardInterrupt:
        link.send(a.build(a.OP_DEBUG_LOG_UNSUBSCRIBE))
        time.sleep(0.1)
        print("\nunsubscribed.")
