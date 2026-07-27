#!/usr/bin/env python3
"""Read the fork's dev-only scan-step profiler (CPS 0xA9).

Why this exists: an analog scan step costs `dwell + 18 ms`, and the 18 ms is constant
across every dwell from 6 to 480 ms. Constant means it is not per-main-loop work -- the
dwell counts main-loop iterations, so per-iteration cost would scale with it. It is a
one-shot cost paid in the branch that moves to the next frequency, and this tool says
which part of that branch.

Needs firmware built with ENABLE_SPECTRUM=1 (and ENABLE_KEY_INJECTION=1 for `run`,
which starts and stops the scan itself).

  dump    read the table (optionally zeroing it afterwards)
  reset   zero the table
  run     set the dwell, arm, start a VFO scan, wait, stop, dump

The radio must already be in VFO mode with sane scan limits before `run` -- see the
traps in NEXT_SESSION_SCANNER_SPEED.md, particularly that digits only mean "scan limits"
while screenOperationMode == SCAN.

Examples:
  python scanprof.py run --dwell 30 --seconds 6
  python scanprof.py run --dwell 6  --seconds 6
  python scanprof.py dump
"""
import argparse
import struct
import sys
import time

import serial
from serial.tools import list_ports

APP_VID, APP_PID = 0x1FC9, 0x0094

CMD_SCAN_DWELL = 0xA6
CMD_FUNC_INJECT = 0xA7
CMD_PROF = 0xA9
CMD_KEY_INJECT = 0x96

PROF_RESET_AFTER = 0x01
PROF_RESET_ONLY = 0x02

FUNC_START_SCANNING = 0x8001
KEY_RED = 27

# (slot number in spectrum.h, label). Listed as a call tree rather than in slot order --
# indentation shows containment, so a row's time is part of the row above it.
DISPLAY_ORDER = [
    (0,  "step period       (dwell + overhead)"),
    (1,  "step total        (the whole step branch)"),
    (2,  "  handleUpKey     (of which)"),
    (3,  "    stepFrequency"),
    (10, "      trxSetFrequency"),
    (15, "        trxUpdateRadioCalibration"),
    (14, "        trxUpdateC6000Calibration"),
    (11, "        radioSetFrequency"),
    (13, "        radioSetRx"),
    (16, "          HRC6000SetFMRx"),
    (12, "        radioSetIF"),
    (4,  "    uiVFOModeUpdateScreen"),
    (8,  "      displayRenderRows (LCD DMA blit)"),
    (9,  "scanning()        (whole call)"),
    (7,  "menuSystemCallCurrentMenuTick"),
    (5,  "loop period       (nominally 1 ms)"),
    (6,  "loop body         (excluding the 1 ms pad)"),
]


def find_port(explicit=None):
    if explicit:
        return explicit
    for p in list_ports.comports():
        if p.vid == APP_VID and p.pid == APP_PID:
            return p.device
    sys.exit("radio not found (USB 1FC9:0094) -- is it in normal mode, not DFU?")


def read_exact(ser, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = ser.read(n - len(buf))
        if not chunk:
            break
        buf += chunk
    return bytes(buf)


def prof_read(ser, action=0):
    ser.reset_input_buffer()
    ser.write(bytes([ord("C"), CMD_PROF, action]))
    ser.flush()

    head = read_exact(ser, 5)
    if len(head) < 5 or head[0] != ord("C") or head[1] != CMD_PROF:
        sys.exit("bad profiler reply: %s -- is this an ENABLE_SPECTRUM build?"
                 % head.hex())
    nslots, cycles_per_us = head[2], struct.unpack_from(">H", head, 3)[0]

    if action & PROF_RESET_ONLY:
        return nslots, cycles_per_us, []

    body = read_exact(ser, nslots * 10)
    if len(body) < nslots * 10:
        sys.exit("short profiler body: got %d of %d" % (len(body), nslots * 10))

    slots = [struct.unpack_from(">HHHHH", body, i * 10) for i in range(nslots)]
    return nslots, cycles_per_us, slots


def set_dwell(ser, ms):
    ser.reset_input_buffer()
    ser.write(struct.pack(">BBH", ord("C"), CMD_SCAN_DWELL, ms))
    ser.flush()
    read_exact(ser, 4)


def inject_func(ser, code):
    ser.reset_input_buffer()
    ser.write(struct.pack(">BBH", ord("C"), CMD_FUNC_INJECT, code))
    ser.flush()
    read_exact(ser, 4)


def inject_key(ser, code, long_press=False):
    ser.reset_input_buffer()
    ser.write(bytes([ord("C"), CMD_KEY_INJECT, code & 0xFF,
                     0x01 if long_press else 0x00]))
    ser.flush()
    read_exact(ser, 1)


def print_table(cycles_per_us, slots):
    print("core clock: %d MHz" % cycles_per_us)
    print()
    print("  %-40s %7s %8s %8s %8s" %
          ("", "count", "min", "mean", "max"))
    for slot, label in DISPLAY_ORDER:
        if slot >= len(slots):
            continue
        count, _last, mn, mx, mean = slots[slot]
        if count == 0:
            print("  %-40s %7d %8s %8s %8s" % (label, 0, "-", "-", "-"))
            continue
        print("  %-40s %7d %7.2f %7.2f %7.2f" %
              (label, count, mn / 1000.0, mean / 1000.0, mx / 1000.0))
    print()
    print("  all times in milliseconds. The trxSetFrequency children are instrumented at")
    print("  their definitions, so `count` is calls per scan step, not calls per step of 1.")

    # The headline: how much of the step period is explained by the step branch.
    period = slots[0] if len(slots) > 0 else None
    total = slots[1] if len(slots) > 1 else None
    if period and total and period[0] and total[0]:
        p_mean, t_mean = period[4] / 1000.0, total[4] / 1000.0
        print()
        print("  step period %.2f ms, of which %.2f ms is the step branch "
              "(%.0f%%); %.2f ms is the dwell loop." %
              (p_mean, t_mean, 100.0 * t_mean / p_mean, p_mean - t_mean))


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("action", choices=("dump", "reset", "run"))
    ap.add_argument("--port")
    ap.add_argument("--reset", action="store_true",
                    help="dump: zero the table after reading")
    ap.add_argument("--dwell", type=int, default=0,
                    help="run: analog scan dwell override in ms (0 = stock)")
    ap.add_argument("--seconds", type=float, default=6.0,
                    help="run: how long to let the scan run")
    args = ap.parse_args()

    ser = serial.Serial(find_port(args.port), 115200, timeout=2.0)

    if args.action == "reset":
        prof_read(ser, PROF_RESET_ONLY)
        print("profiler table zeroed")
        return

    if args.action == "run":
        set_dwell(ser, args.dwell)
        print("dwell override: %s" % ("stock" if args.dwell == 0 else "%d ms" % args.dwell))
        inject_func(ser, FUNC_START_SCANNING)
        time.sleep(0.4)                     # let the scan get going before arming
        prof_read(ser, PROF_RESET_ONLY)     # arm: zero the table mid-scan
        time.sleep(args.seconds)
        _, cycles_per_us, slots = prof_read(ser)
        inject_key(ser, KEY_RED)            # stop scanning
        print("scanned for %.1f s" % args.seconds)
        print()
        print_table(cycles_per_us, slots)
        return

    _, cycles_per_us, slots = prof_read(ser, PROF_RESET_AFTER if args.reset else 0)
    print_table(cycles_per_us, slots)


if __name__ == "__main__":
    main()
