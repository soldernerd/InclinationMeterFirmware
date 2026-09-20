#!/usr/bin/env python3
"""
Name the MSO-X 3024A's channels from the command line instead of the
front panel -- 4 analog + 16 digital, one call.

  # inline, comma-separated ch:label pairs
  python scope_label_channels.py --analog 1:S1,2:S2,3:S3,4:S4 \\
                                  --digital 0:SCK,1:MOSI,2:CS,3:DRDY

  # from a JSON file: {"analog": {"1": "S1", ...}, "digital": {"0": "SCK", ...}}
  python scope_label_channels.py --json labels.json

  # just show what's currently set
  python scope_label_channels.py --list

  # turn labels off without changing them
  python scope_label_channels.py --hide

  # grab a screenshot after labeling (see it without walking to the bench)
  python scope_label_channels.py --analog 1:S1,2:S2 --screenshot scope.png

Labels are capped at 10 characters and the scope uppercases them --
see docs/bench_instruments.md.

Install:  pip install pyvisa
"""

import argparse
import json
import sys

from bench_instruments import ANALOG_CHANNELS, DIGITAL_CHANNELS, Scope


def parse_pairs(s):
    """'1:S1,2:S2' -> {1: 'S1', 2: 'S2'}"""
    out = {}
    if not s:
        return out
    for part in s.split(","):
        part = part.strip()
        if not part:
            continue
        if ":" not in part:
            sys.exit(f"bad pair {part!r} -- expected ch:label")
        ch_str, label = part.split(":", 1)
        out[int(ch_str)] = label
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--resource", help="override the scope's VISA resource string")
    ap.add_argument("--analog", help="ch:label pairs, e.g. 1:S1,2:S2")
    ap.add_argument("--digital", help="ch:label pairs, e.g. 0:SCK,1:MOSI")
    ap.add_argument("--json", help="JSON file: {\"analog\": {...}, \"digital\": {...}}")
    ap.add_argument("--hide", action="store_true", help="turn label display off (labels kept)")
    ap.add_argument("--no-enable", action="store_true",
                     help="set labels without also turning the channel/pod trace on")
    ap.add_argument("--list", action="store_true", help="print current labels and exit")
    ap.add_argument("--screenshot", help="save the screen as a PNG after labeling")
    args = ap.parse_args()

    kwargs = {"resource": args.resource} if args.resource else {}
    scope = Scope(**kwargs)
    print("Connected:", scope.idn())

    if args.list:
        for ch in ANALOG_CHANNELS:
            print(f"  CH{ch}: {scope.get_analog_label(ch)!r}")
        for ch in DIGITAL_CHANNELS:
            print(f"  D{ch}:  {scope.get_digital_label(ch)!r}")
        scope.close()
        return

    analog, digital = {}, {}
    if args.json:
        with open(args.json) as f:
            j = json.load(f)
        analog = {int(k): v for k, v in j.get("analog", {}).items()}
        digital = {int(k): v for k, v in j.get("digital", {}).items()}
    analog.update(parse_pairs(args.analog))
    digital.update(parse_pairs(args.digital))

    if args.hide:
        scope.show_labels(False)
        print("Labels hidden (not changed).")
    elif analog or digital:
        scope.label_all(analog, digital, show=True, enable=not args.no_enable)
        for ch, name in sorted(analog.items()):
            print(f"  CH{ch} -> {name!r}")
        for ch, name in sorted(digital.items()):
            print(f"  D{ch}  -> {name!r}")
    else:
        ap.error("nothing to do -- pass --analog/--digital/--json, --list, or --hide")

    if args.screenshot:
        scope.screenshot_png(args.screenshot)
        print(f"  wrote {args.screenshot}")

    scope.close()


if __name__ == "__main__":
    main()
