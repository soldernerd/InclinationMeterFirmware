#!/usr/bin/env python3
"""Module dependency graph of the firmware, as one self-contained HTML page (no libraries, no network).

    python tools/depgraph.py [output.html]        default: docs/firmware-module-graph.html

A node is a module = a .c/.h pair (or a lone header). An edge A -> B means a file of A includes a header of B, i.e. A uses
B. That is the best static stand-in for "what calls what": a function call needs the callee's declaration. Vendored code
and the CubeMX-generated peripheral initialisation are collapsed into one node each. The rows are the longest dependency
path from main.c, so the picture reads top-down: main at the top, the hardware-facing code at the bottom. The few modules
nearly everything includes (config, pin map, system state, ...) are drawn in a strip on the side and their edges are hidden
unless you ask for them, otherwise they would bury the structure.

Click a module: its direct dependencies (green) and users (orange) are highlighted and its files listed.
"""
import html, json, os, re, sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIRS = ["App", "Services", "Drivers_App", "HAL_App", "Math", "Config", "Core", "USB_Device"]
TOP = ["system_state.c", "system_state.h"]
INC = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.M)

# used by (nearly) everything: their edges are hidden by default
GLOBAL = {"Config/config", "Config/pin_config", "Config/app_version", "system_state", "Drivers_App/drv_common",
          "Config/build_info"}
CORE_MAIN = {"Core/Src/main", "Core/Inc/main"}
CORE_GROUP = "CubeMX peripheral init (Core/)"
USB_GROUP = "CubeMX USB device (USB_Device/)"

def vendor_of(path):
    p = path.replace("\\", "/")
    if "STM32G0xx_HAL_Driver" in p or "CMSIS" in p or re.search(r"(^|/)(stm32g0|system_stm32g0)", p):
        return "ST HAL + CMSIS"
    if "u8g2" in p:
        return "u8g2 (graphics)"
    if "STM32_USB_Device_Library" in p or p.startswith("Middlewares"):
        return "ST USB library"
    return None

files = []
for d in DIRS:
    for dp, _, fs in os.walk(os.path.join(ROOT, d)):
        for f in fs:
            if f.endswith((".c", ".h")):
                files.append(os.path.relpath(os.path.join(dp, f), ROOT).replace("\\", "/"))
files += [f for f in TOP if os.path.exists(os.path.join(ROOT, f))]

by_name = defaultdict(list)
for f in files:
    by_name[os.path.basename(f)].append(f)

def module(f):
    m = os.path.splitext(f)[0]
    if m in CORE_MAIN:
        return "Core/main"
    if m.startswith("Core/"):
        return CORE_GROUP
    if m.startswith("USB_Device/"):
        return USB_GROUP
    return m

def folder(m):
    if m == "Core/main":
        return "main"
    if m in (CORE_GROUP, USB_GROUP) or m in ("ST HAL + CMSIS", "u8g2 (graphics)", "ST USB library"):
        return "vendor"
    return m.split("/")[0] if "/" in m else "App-global"

nodes = defaultdict(lambda: {"files": set()})
edges = set()
for f in files:
    m = module(f)
    nodes[m]["files"].add(f)
    text = open(os.path.join(ROOT, f), encoding="utf-8", errors="replace").read()
    for inc in INC.findall(text):
        base = os.path.basename(inc)
        tgt = None
        if base in by_name:
            c = by_name[base]
            tgt = module(c[0]) if len(c) == 1 else module(sorted(c, key=lambda x: os.path.dirname(x) != os.path.dirname(f))[0])
        else:
            tgt = vendor_of(inc)
            if tgt:
                nodes[tgt]
        if tgt and tgt != m:
            edges.add((m, tgt))
for v in ("ST HAL + CMSIS", "u8g2 (graphics)", "ST USB library"):
    nodes[v]

main = "Core/main"
# main depends on the CubeMX init and the USB device by construction (it calls MX_*_Init)
edges.add((main, CORE_GROUP)); edges.add((main, USB_GROUP))
glob = {m for m in nodes if m in GLOBAL}
vis = [m for m in nodes if m not in glob]
adj = defaultdict(set)
for a, b in edges:
    if a in vis and b in vis:
        adj[a].add(b)

# ---- rank = longest path from main (DFS, back edges ignored so a cycle cannot loop) ----
rank = {}
state = {}
order = []
def dfs(u):
    state[u] = 1
    for v in sorted(adj[u]):
        if state.get(v) == 1:
            continue                  # back edge
        if v not in state:
            dfs(v)
    state[u] = 2
    order.append(u)
dfs(main)
for u in vis:                          # anything main does not reach (tests of nothing, orphans): own roots
    if u not in state:
        dfs(u)
back = set()
state2 = {}
def find_back(u):
    state2[u] = 1
    for v in sorted(adj[u]):
        if state2.get(v) == 1:
            back.add((u, v))
        elif v not in state2:
            find_back(v)
    state2[u] = 2
for r in [main] + sorted(vis):
    if r not in state2:
        find_back(r)
dag = {u: {v for v in adj[u] if (u, v) not in back} for u in vis}
rank = {u: 0 for u in vis}
for u in reversed(order):              # reverse post-order = topological order of the DAG
    for v in dag[u]:
        rank[v] = max(rank[v], rank[u] + 1)
for v in ("ST HAL + CMSIS", "u8g2 (graphics)", "ST USB library"):   # vendor libraries all on the last row
    rank[v] = 0
maxr = max(rank.values())
for v in ("ST HAL + CMSIS", "u8g2 (graphics)", "ST USB library"):
    rank[v] = maxr
# roots that main does not reach sit just under main
for u in vis:
    if u != main and rank[u] == 0:
        rank[u] = 1

rows = defaultdict(list)
for u in vis:
    rows[rank[u]].append(u)

# ---- order within a row: barycentre sweeps, then fall back to folder grouping ----
folder_order = {"main": 0, "App": 1, "Services": 2, "Drivers_App": 3, "HAL_App": 4, "Math": 5, "vendor": 6, "App-global": 7}
for r in rows:
    rows[r].sort(key=lambda m: (folder_order.get(folder(m), 9), m))
pos = {}
def place():
    for r in rows:
        for i, m in enumerate(rows[r]):
            pos[m] = i
place()
preds = defaultdict(set)
for a in dag:
    for b in dag[a]:
        preds[b].add(a)
for _ in range(6):
    for r in sorted(rows)[1:]:
        rows[r].sort(key=lambda m: (sum(pos[p] / max(1, len(rows[rank[p]])) for p in preds[m]) / max(1, len(preds[m])) if preds[m] else 0.5,
                                    folder_order.get(folder(m), 9), m))
        place()
    for r in sorted(rows, reverse=True)[1:]:
        rows[r].sort(key=lambda m: (sum(pos[s] / max(1, len(rows[rank[s]])) for s in dag[m]) / max(1, len(dag[m])) if dag[m] else 0.5,
                                    folder_order.get(folder(m), 9), m))
        place()

# ---- geometry ----
NODE_W, NODE_H, GAP_X, GAP_Y, PAD = 150, 34, 12, 88, 20
widest = max(len(v) for v in rows.values())
side_w = 190
W = PAD * 2 + widest * (NODE_W + GAP_X) + side_w
H = PAD * 2 + (maxr + 1) * (NODE_H + GAP_Y)
geom = {}
for r, ms in rows.items():
    total = len(ms) * (NODE_W + GAP_X) - GAP_X
    x0 = PAD + side_w + (W - side_w - PAD * 2 - total) / 2
    for i, m in enumerate(ms):
        geom[m] = (x0 + i * (NODE_W + GAP_X), PAD + r * (NODE_H + GAP_Y))
for i, m in enumerate(sorted(glob)):
    geom[m] = (PAD, PAD + i * (NODE_H + 14) + 30)

def label(m):
    return m.split("/")[-1] if "/" in m and m not in (CORE_GROUP, USB_GROUP) else m

data = {
    "w": W, "h": H, "nw": NODE_W, "nh": NODE_H,
    "nodes": [{"id": m, "label": label(m), "x": geom[m][0], "y": geom[m][1], "folder": folder(m),
               "glob": m in glob, "files": sorted(nodes[m]["files"])} for m in sorted(geom)],
    "edges": [{"a": a, "b": b, "back": (a, b) in back} for a, b in sorted(edges) if a in geom and b in geom],
}


# ======================= overview (subsystems) =======================
# Hand-written: which module belongs to which subsystem, and what each one is for in plain words.
GROUPS = [
    # id, title, row, plain-English description
    ("entry",   "Boot and entry",            0, "main() starts the hardware in a fixed order, then never returns: it hands control to the scheduler. The CubeMX-generated code sets up the chip's peripherals."),
    ("app",     "Scheduler and screens",     1, "The cooperative main loop (no RTOS) calls every task in turn. The screen code turns knob presses into menus and draws the LCD."),
    ("api",     "Host API",                  1, "The one protocol every host speaks (USB, Bluetooth, wired UART). A dispatcher checks a request packet and routes it to a handler; the tables are generated from tools/api_spec.py."),
    ("front",   "Front panel",               2, "What the user touches and sees: two knobs with buttons, the buzzer, the memory LCD."),
    ("measure", "Tilt measurement",          2, "The heart of the instrument: sine excitation out, four ADC channels back, demodulated into tilt per sensor, plus the zero calibration and the precision measurement."),
    ("transp",  "Transports",                2, "Move API packets over USB HID, Bluetooth (RN4871) or the debug UART, each with its own outgoing frame queue."),
    ("power",   "Power and safety",          3, "Battery state and charging, auto power-off, service mode (unlocks dangerous commands), the supervised watchdog and HardFault capture."),
    ("storage", "Settings storage",          3, "Settings and calibrations in the EEPROM, in CRC-protected pages that are reseeded to defaults if damaged."),
    ("env",     "Temperature and environment", 3, "Board temperature (TMP236, LM35) and the BME280 temperature / pressure / humidity sensor."),
    ("math",    "Pure maths",                4, "Hardware-free calculations: CRC, phasor combination, demodulation, quality windows, BME280 compensation, watchdog logic. All unit-tested on the PC."),
    ("hal",     "Hardware abstraction (HAL_App)", 4, "Thin wrappers over the ST HAL: pins, I2C, SPI, UARTs, timers, ADC, RTC, SysTick, power modes, watchdog, DFU."),
    ("shared",  "Shared state and constants", 4, "The global SystemState and DeviceSettings, the pin map, tuning constants, the log ring. Almost everything includes these."),
    ("vendor",  "Vendor libraries",          5, "ST HAL + CMSIS, the ST USB stack, u8g2 graphics. Not our code."),
]
GROUP_OF = {}
def put(gid, *mods):
    for m in mods:
        GROUP_OF[m] = gid
put("entry", "Core/main", CORE_GROUP, USB_GROUP)
put("app", "App/app_scheduler", "App/app_ui", "App/app_display", "App/app_leds", "App/u8g2_hal_callback")
put("api", "Services/svc_api", "Services/svc_api_core", "Services/svc_api_defs", "Services/svc_api_tables", "Services/svc_api_fields",
    "Services/svc_api_res", "Services/svc_api_res_cmd", "Services/svc_api_res_data", "Services/svc_api_res_stream",
    "Services/svc_api_res_system")
put("front", "Drivers_App/drv_encoder", "Drivers_App/drv_buzzer", "Drivers_App/drv_sharp_lcd", "Services/svc_input")
put("measure", "Services/svc_displacement", "Services/svc_displacement_internal", "Services/svc_disp_capture",
    "Services/svc_disp_phasor_stream", "Services/svc_disp_procedures", "Drivers_App/drv_ads131m04", "Drivers_App/drv_ad9833")
put("transp", "Services/svc_usb", "Services/svc_ble", "Services/svc_uart", "Services/svc_txframe", "Drivers_App/drv_rn4871")
put("power", "Services/svc_battery", "Services/svc_power", "Services/svc_powertest", "Services/svc_service")
put("storage", "Services/svc_storage", "Drivers_App/drv_24lc256")
put("env", "Drivers_App/drv_bme280", "Drivers_App/drv_tmp236", "Drivers_App/drv_lm35")
put("math", "Math/math_crc", "Math/math_phasor", "Math/math_window", "Math/math_displacement", "Math/math_quality",
    "Math/math_bme280", "Math/math_fault", "Math/math_supervisor")
put("hal", *[m for m in nodes if m.startswith("HAL_App/")])
put("shared", "system_state", "Config/config", "Config/pin_config", "Config/app_version", "Drivers_App/drv_common", "Services/svc_log")
put("vendor", "ST HAL + CMSIS", "u8g2 (graphics)", "ST USB library")
# power-and-safety also owns the watchdog / fault pieces that live in the HAL and Math layers
for m in ("HAL_App/hal_wdt", "HAL_App/hal_fault", "HAL_App/hal_power", "HAL_App/hal_dfu", "Math/math_fault", "Math/math_supervisor"):
    GROUP_OF[m] = "power"
GROUP_OF["Math/math_bme280"] = "env"
for m in ("Math/math_displacement", "Math/math_phasor", "Math/math_quality", "Math/math_window"):
    GROUP_OF[m] = "measure"
GROUP_OF["Math/math_crc"] = "math"
GROUP_OF["HAL_App/hal_pintest"] = "power"
missing = [m for m in nodes if m not in GROUP_OF]
assert not missing, missing

PURPOSE = {
    "Core/main": "The boot sequence: log ring, rails, clocks, peripherals, drivers, services, transports, then app_scheduler_run().",
    CORE_GROUP: "ST-generated setup of ADC, DMA, GPIO, I2C, SPI, timers, UARTs, RTC, plus the interrupt handlers (HardFault hook in a USER CODE block).",
    USB_GROUP: "ST-generated USB device stack setup: descriptors and the custom HID interface.",
    "App/app_scheduler": "The main loop: a table of tasks with periods; calls each when it is due. Order within a tick matters (input before UI before display).",
    "App/app_ui": "Screen state machine: knob turns/presses become screen changes, setting edits, and actions (charge, zero cal, service mode, power off).",
    "App/app_display": "Draws the LIVE, STATUS, SETTINGS and DIAGNOSTICS screens in bands, only redrawing what changed.",
    "App/app_leds": "Status and power LEDs.",
    "App/u8g2_hal_callback": "Glue between the u8g2 graphics library and the LCD driver.",
    "Services/svc_api": "The dispatcher: checks category, verb, resource, CRC, length and the service-mode gate, then calls the handler. Owns subscriptions.",
    "Services/svc_api_core": "Types shared by the dispatcher, the generated tables and the handlers.",
    "Services/svc_api_defs": "GENERATED opcodes and wire structs.",
    "Services/svc_api_tables": "GENERATED resource table the dispatcher walks.",
    "Services/svc_api_fields": "One generic handler for every settings/calibration value (a table row each).",
    "Services/svc_api_res": "Small header shared by the handler files.",
    "Services/svc_api_res_cmd": "Handlers for Commands: power off, charging, zero cal, precision, rails, DFU, factory defaults.",
    "Services/svc_api_res_data": "Handlers for Measurements, Diagnostics and Procedures read-outs.",
    "Services/svc_api_res_stream": "Handlers for live Topics, the debug-log stream and the bulk raw capture.",
    "Services/svc_api_res_system": "Handlers for System: identity, state, health, clock.",
    "Drivers_App/drv_encoder": "Quadrature decode of the two knobs (interrupt driven).",
    "Drivers_App/drv_buzzer": "Non-blocking click tones.",
    "Drivers_App/drv_sharp_lcd": "The memory LCD: frame buffer, line updates, VCOM toggle (must never stop).",
    "Services/svc_input": "Polls the push buttons and mirrors them and the knob counts into the system state.",
    "Services/svc_displacement": "Measurement core: collects ADC samples into 64-cycle batches, demodulates tilt, exposes the readings.",
    "Services/svc_displacement_internal": "Private interface between the measurement pieces.",
    "Services/svc_disp_capture": "Bulk raw ADC capture buffer (API Bulk).",
    "Services/svc_disp_phasor_stream": "Gapless FIFO of raw phasors for hosts that log the signal.",
    "Services/svc_disp_procedures": "Flip (zero) calibration and the precision measurement.",
    "Drivers_App/drv_ads131m04": "The 4-channel simultaneous ADC: configuration, DMA read, integrity checks.",
    "Drivers_App/drv_ad9833": "The sine excitation source.",
    "Math/math_phasor": "Combines per-position sums into a phasor (the matched filter).",
    "Math/math_displacement": "Per-batch tilt arithmetic: ratio to the excitation difference, phase rotation, zero and k.",
    "Math/math_window": "Hann window means and the quiet-floor quality indicator.",
    "Math/math_quality": "The LIVE display stream and the precision measurement as state machines.",
    "Services/svc_usb": "USB HID transport: reports, fragmentation of packets over 64 bytes.",
    "Services/svc_ble": "Bluetooth transport over the RN4871 transparent UART.",
    "Services/svc_uart": "Wired UART transport (the debug header).",
    "Services/svc_txframe": "Per-transport outgoing frame FIFO with a reserve so command responses are never starved.",
    "Drivers_App/drv_rn4871": "The RN4871 Bluetooth module: reset and configuration over the UART.",
    "Services/svc_battery": "Battery voltage and state of charge, charge-enable policy, VBUS debounce, critical-battery shutdown.",
    "Services/svc_power": "Auto power-off after inactivity.",
    "Services/svc_powertest": "Bench aid: switch rails and clocks on and off to find where current goes.",
    "Services/svc_service": "Service mode: the temporary unlock for dangerous commands.",
    "HAL_App/hal_wdt": "Supervised window watchdog, refreshed from SysTick while the main loop makes progress.",
    "HAL_App/hal_fault": "HardFault capture into backup registers, reset, report next boot.",
    "HAL_App/hal_power": "Standby entry, wake pins, rail retention.",
    "HAL_App/hal_dfu": "Reboot into the ROM bootloader by option-byte reload.",
    "HAL_App/hal_pintest": "Diagnostic: drive display/buzzer lines statically while measuring current.",
    "Math/math_fault": "Packing and unpacking the fault record kept across a reset.",
    "Math/math_supervisor": "The watchdog's decision logic.",
    "Services/svc_storage": "Loads and saves settings pages to the EEPROM with retries, CRC and defaults on damage.",
    "Drivers_App/drv_24lc256": "EEPROM byte/page access over I2C.",
    "Drivers_App/drv_bme280": "BME280 over I2C, forced mode once a second.",
    "Math/math_bme280": "BME280 compensation formulas.",
    "Drivers_App/drv_tmp236": "TMP236 board temperature through the internal ADC.",
    "Drivers_App/drv_lm35": "External LM35 temperature through the internal ADC.",
    "Math/math_crc": "CRC-16 used by the API and the EEPROM pages.",
    "system_state": "SystemState (live values, flags) and DeviceSettings (EEPROM-backed settings and calibrations).",
    "Config/config": "Tuning constants, task periods, EEPROM layout.",
    "Config/pin_config": "The authoritative pin/net map of REV B.",
    "Config/app_version": "Firmware version numbers.",
    "Drivers_App/drv_common": "DrvStatus codes.",
    "Services/svc_log": "RAM ring of log lines, streamed to hosts over the API.",
    "HAL_App/hal_adc": "Internal ADC scan and conversions.",
    "HAL_App/hal_gpio": "Pin read/write and defaults.",
    "HAL_App/hal_i2c": "I2C with short timeouts so a missing device cannot stall the loop.",
    "HAL_App/hal_mcu": "Unique device ID.",
    "HAL_App/hal_rtc": "Real-time clock, sub-second counter, trim.",
    "HAL_App/hal_spi": "SPI for display, DAC and ADC (raw DMA read for the ADC).",
    "HAL_App/hal_systick": "Millisecond time base.",
    "HAL_App/hal_tim": "Timers: VCOM, ADC trigger, MCLK, buzzer.",
    "HAL_App/hal_uart": "UARTs with DMA, non-blocking.",
    "HAL_App/hal_usb": "USB device glue for the HID transport.",
    "ST HAL + CMSIS": "STMicroelectronics' HAL drivers and Arm CMSIS headers.",
    "u8g2 (graphics)": "The u8g2 monochrome graphics library.",
    "ST USB library": "STMicroelectronics' USB device stack.",
}

# the main-loop task table, read from the source so it cannot go stale
sched = open(os.path.join(ROOT, "App/app_scheduler.c"), encoding="utf-8", errors="replace").read()
TASKS = []
for m_ in re.finditer(r"\{\s*(task_\w+)\s*,\s*(&g_device_settings\.(\w+)|NULL)\s*,\s*([A-Z_0-9]+)", sched):
    name, _, setting, per = m_.groups()
    TASKS.append({"name": name[5:], "period": ("setting " + setting) if setting else ("every tick" if per == "EVERY_TICK" else per.replace("DEFAULT_TASK_", "").replace("_MS", " (fixed period)").lower())})

# boot order, read from main.c: the *_init calls in the order they appear
mainc = open(os.path.join(ROOT, "Core/Src/main.c"), encoding="utf-8", errors="replace").read()
BOOT = []
for m_ in re.finditer(r"^\s*((?:svc|drv|hal|app)_\w+_init)\(", mainc, re.M):
    if m_.group(1) not in BOOT:
        BOOT.append(m_.group(1))

# aggregated edges between subsystems (module include counts), global strip hidden by default
gedges = defaultdict(int)
for a, b in edges:
    ga, gb = GROUP_OF.get(a), GROUP_OF.get(b)
    if ga and gb and ga != gb:
        gedges[(ga, gb)] += 1

overview = {
    "groups": [{"id": g, "title": t, "row": r, "desc": d,
                "modules": [{"id": m, "label": label(m), "purpose": PURPOSE.get(m, ""), "files": sorted(nodes[m]["files"]),
                             "uses": sorted(b for (a, b) in edges if a == m and b not in glob and GROUP_OF.get(b) and b != m),
                             "usedby": sorted(a for (a, b) in edges if b == m and a != m)}
                            for m in sorted(GROUP_OF) if GROUP_OF[m] == g]}
               for g, t, r, d in GROUPS],
    "edges": [{"a": a, "b": b, "n": n} for (a, b), n in sorted(gedges.items())],
    "tasks": TASKS, "boot": BOOT,
}

_ALLCSS = '.legend span{display:inline-flex;align-items:center;gap:5px;margin-right:12px;color:var(--mut);font-size:12px}\n.legend i{width:11px;height:11px;border-radius:3px;display:inline-block}\n\n#scroll{flex:1;overflow:auto;height:calc(100vh - 60px)}\n#side{width:300px;border-left:1px solid var(--line);padding:12px 14px;height:calc(100vh - 85px);overflow:auto;background:var(--panel);font-size:13px}\n#side h2{font-size:14px;margin:0 0 6px}\n#side ul{margin:4px 0 10px;padding-left:18px}\n.sw{display:inline-block;width:9px;height:9px;border-radius:2px;margin-right:5px}\nsvg text{font:11px system-ui,sans-serif;pointer-events:none}\n.node rect{stroke-width:1.5;rx:6;fill:var(--panel)}\n.node{cursor:pointer}\n.edge{fill:none;stroke:var(--edge);stroke-width:1;opacity:.35}\n.edge.back{stroke-dasharray:4 3}\n.dim .edge{opacity:.05}.dim .node{opacity:.28}\n.dim .edge.dep{stroke:var(--dep);opacity:1;stroke-width:1.8}\n.dim .edge.use{stroke:var(--use);opacity:1;stroke-width:1.8}\n.dim .node.sel,.dim .node.dep,.dim .node.use{opacity:1}\n.dim .node.dep rect{stroke:var(--dep)!important;stroke-width:2.5}.dim .node.use rect{stroke:var(--use)!important;stroke-width:2.5}\n.dim .node.sel rect{stroke-width:3.5}\n\n'
_ALLJS = 'const colors = {main:\'--c-main\',App:\'--c-App\',Services:\'--c-Services\',Drivers_App:\'--c-Drivers_App\',HAL_App:\'--c-HAL_App\',Math:\'--c-Math\',vendor:\'--c-vendor\',\'App-global\':\'--c-glob\',Config:\'--c-glob\'};\nconst col = f => \'var(\' + (colors[f] || \'--c-glob\') + \')\';\nconst NS = \'http://www.w3.org/2000/svg\';\nconst svg = document.getElementById(\'svg\');\nsvg.setAttribute(\'width\', D.w); svg.setAttribute(\'height\', D.h); svg.setAttribute(\'viewBox\', `0 0 ${D.w} ${D.h}`);\nconst byId = Object.fromEntries(D.nodes.map(n => [n.id, n]));\nconst el = (t, a, p) => { const e = document.createElementNS(NS, t); for (const k in a) e.setAttribute(k, a[k]); (p || svg).appendChild(e); return e; };\nconst gE = el(\'g\', {}), gN = el(\'g\', {});\nconst edgeEls = [];\nfunction path(a, b) {\n  const A = byId[a], B = byId[b];\n  const ax = A.x + D.nw / 2, bx = B.x + D.nw / 2;\n  const down = B.y > A.y, same = B.y === A.y;\n  let ay = down ? A.y + D.nh : A.y, by = down ? B.y : (same ? B.y : B.y + D.nh);\n  if (same) { ay = A.y + D.nh; by = B.y + D.nh; const m = ay + 22; return `M${ax},${ay} C${ax},${m} ${bx},${m} ${bx},${by}`; }\n  const dy = Math.abs(by - ay) / 2;\n  return `M${ax},${ay} C${ax},${ay + (down ? dy : -dy)} ${bx},${by - (down ? dy : -dy)} ${bx},${by}`;\n}\nD.edges.forEach(e => {\n  const p = el(\'path\', {d: path(e.a, e.b), class: \'edge\' + (e.back ? \' back\' : \'\'), \'marker-end\': \'\'}, gE);\n  edgeEls.push({e, p});\n});\nconst nodeEls = {};\nD.nodes.forEach(n => {\n  const g = el(\'g\', {class: \'node\', transform: `translate(${n.x},${n.y})`}, gN);\n  const r = el(\'rect\', {width: D.nw, height: D.nh, stroke: col(n.folder)}, g);\n  const t = el(\'text\', {x: D.nw / 2, y: D.nh / 2 + 4, \'text-anchor\': \'middle\'}, g);\n  t.textContent = n.label.length > 24 ? n.label.slice(0, 23) + \'…\' : n.label;\n  t.setAttribute(\'fill\', \'currentColor\');\n  const tt = el(\'title\', {}, g); tt.textContent = n.id + \'\\n\' + n.files.join(\'\\n\');\n  g.addEventListener(\'click\', ev => { ev.stopPropagation(); select(n.id); });\n  nodeEls[n.id] = g;\n});\nif (D.nodes.some(n => n.glob)) { const gl = el(\'text\', {x: 20, y: 24, fill: \'var(--mut)\'}, gN); gl.textContent = \'used by almost everything\'; gl.setAttribute(\'style\', \'font-size:11px\'); }\nconst showglob = document.getElementById(\'showglob\'), showback = document.getElementById(\'showback\');\nfunction applyVis() {\n  edgeEls.forEach(({e, p}) => {\n    const g = byId[e.a].glob || byId[e.b].glob;\n    p.style.display = ((g && !showglob.checked) || (e.back && !showback.checked)) ? \'none\' : \'\';\n  });\n}\nshowglob.onchange = showback.onchange = applyVis; applyVis();\nlet sel = null;\nfunction select(id) {\n  sel = id;\n  svg.classList.add(\'dim\');\n  Object.values(nodeEls).forEach(g => g.setAttribute(\'class\', \'node\'));\n  edgeEls.forEach(({p}) => p.setAttribute(\'class\', p.getAttribute(\'class\').replace(/ ?(dep|use)/g, \'\')));\n  nodeEls[id].classList.add(\'sel\');\n  const deps = [], uses = [];\n  edgeEls.forEach(({e, p}) => {\n    if (e.a === id) { p.classList.add(\'dep\'); p.style.display = \'\'; nodeEls[e.b].classList.add(\'dep\'); deps.push(e.b); }\n    if (e.b === id) { p.classList.add(\'use\'); p.style.display = \'\'; nodeEls[e.a].classList.add(\'use\'); uses.push(e.a); }\n  });\n  const n = byId[id];\n  const li = a => a.length ? \'<ul>\' + a.sort().map(x => `<li><a href="#" data-id="${x}">${x}</a></li>`).join(\'\') + \'</ul>\' : \'<p style="color:var(--mut)">none</p>\';\n  document.getElementById(\'side\').innerHTML =\n    `<h2><span class="sw" style="background:${col(n.folder)}"></span>${n.id}</h2>\n     <p style="color:var(--mut)">${n.files.join(\'<br>\')}</p>\n     <b style="color:var(--dep)">uses (${deps.length})</b>${li(deps)}\n     <b style="color:var(--use)">used by (${uses.length})${n.glob ? \' - everything, edges hidden\' : \'\'}</b>${li(uses)}`;\n  document.querySelectorAll(\'#side a[data-id]\').forEach(a => a.onclick = ev => { ev.preventDefault(); select(a.dataset.id); });\n}\nsvg.addEventListener(\'click\', () => { sel = null; svg.classList.remove(\'dim\'); Object.values(nodeEls).forEach(g => g.setAttribute(\'class\', \'node\')); applyVis(); });\nconst lg = document.getElementById(\'legend\');\n[[\'main\',\'main\'],[\'App\',\'App\'],[\'Services\',\'Services\'],[\'Drivers_App\',\'Drivers\'],[\'HAL_App\',\'HAL\'],[\'Math\',\'Math\'],[\'vendor\',\'generated / vendor\'],[\'App-global\',\'global\']].forEach(([k, l]) => {\n  lg.insertAdjacentHTML(\'beforeend\', `<span><i style="background:${col(k)}"></i>${l}</span>`);\n});\n'

NEWPAGE = r"""<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Firmware overview</title>
<style>
:root{--bg:#fafaf8;--fg:#1c1c1a;--mut:#66665f;--edge:#8d8d86;--panel:#ffffff;--line:#d8d8d2;--accent:#2f6f9f;--dep:#1f9d55;--use:#e07a1f;
--c-entry:#1c1c1a;--c-app:#2f6f9f;--c-api:#2e8b6a;--c-front:#3d86a8;--c-measure:#b24a63;--c-transp:#3f9b8a;--c-power:#c0582b;--c-storage:#8a6d1f;--c-env:#6f8f2f;--c-math:#9b4fa3;--c-hal:#7a4fa3;--c-shared:#8a8a50;--c-vendor:#77777a;--c-main:#1c1c1a;--c-App:#2f6f9f;--c-Services:#2e8b6a;--c-Drivers_App:#a8651f;--c-HAL_App:#7a4fa3;--c-Math:#b24a63;--c-glob:#8a8a50;--c-App-global:#8a8a50}
@media (prefers-color-scheme:dark){:root:not([data-theme=light]){--bg:#17181a;--fg:#e8e8e4;--mut:#a0a09a;--edge:#6a6a64;--panel:#202225;--line:#34363a}}
*{box-sizing:border-box}
html,body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.5 system-ui,sans-serif}
header{padding:14px 18px 0}
h1{font-size:19px;margin:0 0 4px}
.lead{color:var(--mut);max-width:900px;margin:0 0 10px}
nav{display:flex;gap:4px;padding:0 18px;border-bottom:1px solid var(--line);flex-wrap:wrap}
nav button{background:none;border:0;border-bottom:3px solid transparent;color:var(--mut);padding:8px 12px;font:inherit;cursor:pointer}
nav button.on{color:var(--fg);border-bottom-color:var(--accent)}
.view{display:none;padding:14px 18px}.view.on{display:block}
/* overview */
#ov{display:flex;gap:18px;align-items:flex-start}
#stage{position:relative;flex:1;min-width:640px;overflow:auto}
#rows{position:relative;z-index:2;display:flex;flex-direction:column;gap:46px}
.row{display:flex;justify-content:center;gap:14px;flex-wrap:nowrap}
.box{width:190px;min-height:92px;background:var(--panel);border:2px solid var(--c);border-radius:10px;padding:8px 10px;cursor:pointer;position:relative}
.box h3{margin:0 0 3px;font-size:14px;color:var(--c)}
.box p{margin:0;font-size:12px;color:var(--mut);line-height:1.35}
.box .n{position:absolute;right:8px;top:6px;font-size:11px;color:var(--mut)}
.box.sel{box-shadow:0 0 0 3px var(--c)}
.box.dep{outline:3px solid var(--dep)}.box.use{outline:3px solid var(--use)}
.dimmed .box:not(.sel):not(.dep):not(.use){opacity:.35}
#ovsvg{position:absolute;left:0;top:0;z-index:1;pointer-events:none}
#ovsvg path{fill:none;stroke:var(--edge);stroke-width:1.4;opacity:.45}
#ovsvg path.up{stroke-dasharray:5 4}
#ovsvg path.dep{stroke:var(--dep);opacity:1;stroke-width:2.6}
#ovsvg path.use{stroke:var(--use);opacity:1;stroke-width:2.6}
.dimmed #ovsvg path:not(.dep):not(.use){opacity:.08}
#detail{width:380px;flex:none;background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:12px 14px;max-height:calc(100vh - 150px);overflow:auto;position:sticky;top:10px}
#detail h2{font-size:16px;margin:0 0 4px}
#detail .m{border-top:1px solid var(--line);padding:7px 0;font-size:13px}
#detail .m b{font-size:13px}
#detail .m span{color:var(--mut)}
#detail .f{font-size:11px;color:var(--mut);word-break:break-all}
#detail a{color:var(--accent);cursor:pointer;text-decoration:none}
.hint{color:var(--mut);font-size:13px}
/* how it runs */
.cols{display:flex;gap:28px;flex-wrap:wrap}
.cols>div{flex:1;min-width:300px}
ol.steps{padding-left:22px;margin:6px 0}
ol.steps li{margin:3px 0}
code{background:rgba(127,127,127,.15);padding:0 4px;border-radius:3px;font-size:13px}
table{border-collapse:collapse;font-size:13px}td,th{border:1px solid var(--line);padding:3px 9px;text-align:left}
.flow{display:flex;flex-wrap:wrap;align-items:center;gap:6px;margin:8px 0}
.flow span.b{background:var(--panel);border:1.5px solid var(--accent);border-radius:8px;padding:4px 9px;font-size:13px}
.flow span.a{color:var(--mut)}
/* all modules (original graph) */
__ALLCSS__
@media (max-width:1100px){#ov{flex-direction:column}#detail{width:auto;position:static;max-height:none}}
</style></head><body>
<header><h1>Firmware overview</h1>
<p class="lead">STM32G0B1 inclination meter. About 75 source modules, grouped here into 13 subsystems. Start on <b>Overview</b>; click any box. The other tabs show how the firmware starts and runs, and every module with every arrow.</p></header>
<nav><button data-v="ov" class="on">Overview</button><button data-v="run">How it runs</button><button data-v="all">All modules</button></nav>

<section class="view on" id="v-ov">
<div id="ov"><div id="stage"><div id="rows"></div><svg id="ovsvg"></svg></div>
<div id="detail"><h2>Reading the diagram</h2>
<p class="hint">Each box is a group of related source files. An arrow means "uses": the box at the tail includes code from the box at the head. Rows go from the program's entry point at the top to the hardware at the bottom. Dashed arrows point upward (a lower layer calling back, for example a callback).</p>
<p class="hint">Click a box to see its modules in plain words, and which boxes it uses (<span style="color:var(--dep)">green</span>) and which use it (<span style="color:var(--use)">orange</span>).</p>
<p class="hint">To keep the picture readable, arrows <i>from</i> Boot and entry (main starts everything) and arrows <i>into</i> the HAL, pure maths, shared state and vendor libraries (everything uses those) are hidden until you select a box, or:
<label><input type="checkbox" id="sh"> show all arrows</label></p></div></div>
</section>

<section class="view" id="v-run">
<div class="cols">
<div><h2>The path of a reading</h2>
<div class="flow"><span class="b">AD9833 sine</span><span class="a">&rarr; sensors &rarr;</span><span class="b">ADS131M04 ADC (4 channels)</span><span class="a">&rarr; DMA &rarr;</span><span class="b">svc_displacement: sums per batch</span><span class="a">&rarr;</span><span class="b">Math: phasor, ratio, zero, k</span><span class="a">&rarr;</span><span class="b">Hann window + quality flag</span></div>
<div class="flow"><span class="a">then to</span><span class="b">LIVE screen (app_display)</span><span class="a">and</span><span class="b">Host API: Measurements, Topics</span><span class="a">&rarr;</span><span class="b">USB / BLE / UART</span><span class="a">&rarr; host app</span></div>
<p class="hint">The ADC produces 20,833 samples per second. The sample callback only adds numbers into a batch (cheap, runs in the SysTick drain); the maths runs once per 64-cycle batch (40.7 per second) in the normal loop.</p>
<h2>A request from the host</h2>
<div class="flow"><span class="b">USB / BLE / UART bytes</span><span class="a">&rarr;</span><span class="b">transport reassembles a packet</span><span class="a">&rarr;</span><span class="b">svc_api dispatcher checks it</span><span class="a">&rarr;</span><span class="b">a handler in svc_api_res_*</span><span class="a">&rarr;</span><span class="b">the module that owns the thing</span><span class="a">&rarr; response into the transport's TX queue</span></div>
<h2>Boot order</h2><p class="hint">The init calls in <code>Core/Src/main.c</code>, in the order they run:</p><ol class="steps" id="boot"></ol></div>
<div><h2>The main loop</h2><p class="hint">After boot, <code>app_scheduler_run()</code> never returns. Each pass it runs every task that is due (table in <code>App/app_scheduler.c</code>):</p><table id="tasks"><tr><th>task</th><th>runs</th></tr></table>
<p class="hint">Order matters inside a tick: input is read before the UI acts, the UI before the display draws, and the transports before the API so a command received this tick is answered in it. A separate 1 ms SysTick interrupt drains the ADC, refreshes the watchdog (only while the loop makes progress) and keeps time; a timer interrupt toggles the display's VCOM line, which must never stop.</p></div>
</div></section>

<section class="view" id="v-all">
<p class="hint">Every module and every include, rows = longest dependency path from main. Click a module for its neighbours.</p>
<header style="padding:0;border:0;display:flex;flex-wrap:wrap;gap:8px 20px;align-items:center">
<span class="legend" id="legend"></span>
<label><input type="checkbox" id="showglob"> show edges of the global modules</label>
<label><input type="checkbox" id="showback" checked> dashed = up-calls / cycles</label></header>
<main style="display:flex;align-items:flex-start"><div id="scroll"><svg id="svg"></svg></div><div id="side"></div></main>
</section>

<script>
const OV = __OV__;
const D = __DATA__;
const $ = id => document.getElementById(id);
document.querySelectorAll('nav button').forEach(b => b.onclick = () => {
  document.querySelectorAll('nav button').forEach(x => x.classList.toggle('on', x === b));
  document.querySelectorAll('.view').forEach(v => v.classList.toggle('on', v.id === 'v-' + b.dataset.v));
  if (b.dataset.v === 'ov') drawEdges();
});
// ---- overview ----
const byG = Object.fromEntries(OV.groups.map(g => [g.id, g]));
const rowsEl = $('rows');
const maxRow = Math.max(...OV.groups.map(g => g.row));
for (let r = 0; r <= maxRow; r++) {
  const row = document.createElement('div'); row.className = 'row';
  OV.groups.filter(g => g.row === r).forEach(g => {
    const b = document.createElement('div'); b.className = 'box'; b.id = 'g-' + g.id; b.style.setProperty('--c', 'var(--c-' + g.id + ')');
    b.innerHTML = `<h3>${g.title}</h3><span class="n">${g.modules.length}</span><p>${g.desc.length > 120 ? g.desc.slice(0, g.desc.lastIndexOf(' ', 118)) + '…' : g.desc}</p>`;
    b.onclick = e => { e.stopPropagation(); selectG(g.id); };
    row.appendChild(b);
  });
  rowsEl.appendChild(row);
}
const ovsvg = $('ovsvg');
let sel = null;
function drawEdges() {
  const st = $('stage'); const sr = st.getBoundingClientRect();
  ovsvg.setAttribute('width', st.scrollWidth); ovsvg.setAttribute('height', rowsEl.scrollHeight + 10);
  ovsvg.innerHTML = '';
  OV.edges.forEach(e => {
    const base = e.a === 'entry' || ['shared', 'hal', 'math', 'vendor'].includes(e.b);   // "everything uses the layers below" -- noise unless asked for
    if (base && !$('sh').checked && !(sel && (sel === e.a || sel === e.b))) return;
    const A = $('g-' + e.a).getBoundingClientRect(), B = $('g-' + e.b).getBoundingClientRect();
    const ox = st.scrollLeft - sr.left, oy = st.scrollTop - sr.top;
    const ax = A.left + A.width / 2 + ox, bx = B.left + B.width / 2 + ox;
    let ay, by, d, up = false;
    if (Math.abs(A.top - B.top) < 5) {          // same row: arc over the top
      const x1 = A.left + A.width / 2 + ox, x2 = B.left + B.width / 2 + ox, y = A.top + oy;
      d = `M${x1},${y} C${x1},${y - 34} ${x2},${y - 34} ${x2},${y}`;
    } else if (B.top > A.top) {
      ay = A.bottom + oy; by = B.top + oy; const m = (ay + by) / 2;
      const off = ((e.a.charCodeAt(0) + e.b.charCodeAt(1)) % 5 - 2) * 14;
      d = `M${ax + off},${ay} C${ax + off},${m} ${bx + off},${m} ${bx + off},${by}`;
    } else {
      up = true; ay = A.top + oy; by = B.bottom + oy; const m = (ay + by) / 2;
      d = `M${ax},${ay} C${ax},${m} ${bx},${m} ${bx},${by}`;
    }
    const p = document.createElementNS('http://www.w3.org/2000/svg', 'path');
    p.setAttribute('d', d); if (up) p.setAttribute('class', 'up');
    p.dataset.a = e.a; p.dataset.b = e.b;
    ovsvg.appendChild(p);
    // arrow head
    const t = document.createElementNS('http://www.w3.org/2000/svg', 'path');
    const hx = (Math.abs(A.top - B.top) < 5 ? B.left + B.width / 2 + ox : bx + (B.top > A.top ? 0 : 0)), hy = (Math.abs(A.top - B.top) < 5 ? A.top + oy : (B.top > A.top ? by : by));
    const dirDown = !up && Math.abs(A.top - B.top) >= 5;
    const s = 5;
    t.setAttribute('d', dirDown ? `M${hx - s},${hy - 2 * s} L${hx},${hy} L${hx + s},${hy - 2 * s} Z` : `M${hx - s},${hy + 2 * s} L${hx},${hy} L${hx + s},${hy + 2 * s} Z`);
    t.setAttribute('style', 'fill:var(--edge);stroke:none;opacity:.6'); t.dataset.a = e.a; t.dataset.b = e.b; t.dataset.h = 1;
    ovsvg.appendChild(t);
  });
  paint();
}
function paint() {
  const root = $('stage');
  root.classList.toggle('dimmed', !!sel);
  document.querySelectorAll('.box').forEach(b => b.classList.remove('sel', 'dep', 'use'));
  ovsvg.querySelectorAll('path').forEach(p => { p.classList.remove('dep', 'use'); if (p.dataset.h) { p.style.fill = 'var(--edge)'; } });
  if (!sel) return;
  $('g-' + sel).classList.add('sel');
  ovsvg.querySelectorAll('path').forEach(p => {
    if (p.dataset.a === sel) { p.classList.add('dep'); $('g-' + p.dataset.b).classList.add('dep'); if (p.dataset.h) p.style.fill = 'var(--dep)'; }
    if (p.dataset.b === sel) { p.classList.add('use'); $('g-' + p.dataset.a).classList.add('use'); if (p.dataset.h) p.style.fill = 'var(--use)'; }
  });
}
function selectG(id) {
  sel = id; drawEdges();
  const g = byG[id];
  const out = OV.edges.filter(e => e.a === id).map(e => e.b), inn = OV.edges.filter(e => e.b === id).map(e => e.a);
  const lk = a => a.length ? a.map(x => `<a data-g="${x}">${byG[x].title}</a>`).join(', ') : '<span class="hint">nothing</span>';
  $('detail').innerHTML = `<h2 style="color:var(--c-${id})">${g.title}</h2><p>${g.desc}</p>
    <p><b style="color:var(--dep)">Uses:</b> ${lk(out)}<br><b style="color:var(--use)">Used by:</b> ${lk(inn)}</p>
    ${g.modules.map(m => `<div class="m"><b>${m.label}</b><br>${m.purpose ? m.purpose : ''}<div class="f">${m.files.join(' · ')}</div></div>`).join('')}`;
  document.querySelectorAll('#detail a[data-g]').forEach(a => a.onclick = () => selectG(a.dataset.g));
}
document.addEventListener('click', e => { if (sel && !e.target.closest('.box') && !e.target.closest('#detail') && e.target.closest('#v-ov')) { sel = null; drawEdges(); } });
$('sh').onchange = drawEdges;
window.addEventListener('resize', drawEdges);
drawEdges(); setTimeout(drawEdges, 60);
// ---- how it runs ----
$('boot').innerHTML = OV.boot.map(b => `<li><code>${b}()</code></li>`).join('');
$('tasks').insertAdjacentHTML('beforeend', OV.tasks.map(t => `<tr><td><code>${t.name}</code></td><td>${t.period}</td></tr>`).join(''));
// ---- all modules (original graph, ids prefixed) ----
(function(){
__ALLJS__
})();
</script></body></html>
"""

out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "docs", "firmware-module-graph.html")
page = NEWPAGE.replace("__ALLCSS__", _ALLCSS).replace("__ALLJS__", _ALLJS).replace("__OV__", json.dumps(overview)).replace("__DATA__", json.dumps(data))
open(out, "w", encoding="utf-8").write(page)
print("wrote", out, "-", len(data["nodes"]), "modules,", len(overview["groups"]), "subsystems,", len(data["edges"]), "edges")
