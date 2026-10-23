#!/usr/bin/env python3
"""Drive the fork's dev-only swept-RSSI receiver over USB CPS.

This is NOT an SDR. The RF front end is an AT1846S -- a narrowband FM transceiver
chip with no IQ output, only an 8-bit RSSI register. All this can do is retune,
wait, read RSSI, repeat. Needs firmware built with ENABLE_SPECTRUM=1.

  probe   CPS 0xA1 -- retune -> settle -> RSSI timing. Run this first: the settle
          time is what decides whether a usable sweep rate is achievable at all.
  sweep   CPS 0xA0 -- one trace across a frequency range.
  water   CPS 0xA0 in a loop -- scrolling waterfall (tkinter).

Frequencies are given in MHz on the command line; the wire protocol uses OpenGD77's
usual 10 Hz units. Run from Windows or WSL; auto-detects the radio (1FC9:0094).

Examples:
  python sweep.py probe --fa 446.0 --fb 433.5 --retune fast --samples 120
  python sweep.py sweep --start 446.0 --stop 446.2 --step 6.25 --dwell 1000
  python sweep.py water --start 446.0 --stop 446.2 --step 12.5 --dwell 500
"""
import argparse
import struct
import sys
import time

import serial
from serial.tools import list_ports

APP_VID, APP_PID = 0x1FC9, 0x0094

CMD_SWEEP = 0xA0
CMD_PROBE = 0xA1
CMD_SESSION_BEGIN = 0xA2
CMD_SESSION_END = 0xA3

# Retune method. `fast` is kept only as a negative control: it writes the PLL registers
# and does NOT move the receiver (measured against a HackRF carrier 2026-07-27), so a
# sweep built on it silently returns the anchor frequency over and over. `latch` is the
# cheapest method that actually retunes.
RETUNE_NAMES = {"fast": 0, "radio": 1, "trx": 2, "latch": 3,
                "poke30": 4, "poke05": 5, "late30": 6,
                # Candidate cheap triggers added 2026-07-28; whether each actually moves
                # the receiver is exactly what `settle.py triggers` measures, so none of
                # them belongs in RETUNE_VALID until it has been shown to.
                "band0f": 7, "sqtoggle": 8, "xtal": 9, "hilast": 10}
RETUNE_VALID = ("radio", "trx", "latch", "late30")

# The retune index is 4 bits, split: the low 3 stay put and bit 5 carries the top one, so
# a host that predates the extra methods still encodes the old ones identically.
MODE_RETUNE_MASK = 0x07
MODE_RETUNE_HI = 0x20
MODE_FORCE_FM = 0x08
MODE_WIDE = 0x10

# Measured settle after a latching retune: ~2.6 ms to detect a carrier at all, ~4.7 ms
# for the level to be accurate to +/-2 counts. Anything shorter reads a settling receiver.
DEFAULT_DWELL_US = 4700
DETECT_DWELL_US = 2600

MAX_PROBE_SAMPLES = 200
MAX_SWEEP_POINTS = 480


def find_port(explicit=None):
    if explicit:
        return explicit
    for p in list_ports.comports():
        if p.vid == APP_VID and p.pid == APP_PID:
            return p.device
    sys.exit("radio not found (USB 1FC9:0094) -- is it in normal mode, not DFU?")


def open_radio(args):
    return serial.Serial(find_port(args.port), 115200, timeout=2.0)


def mhz(v):
    """MHz -> OpenGD77 10 Hz units."""
    return int(round(v * 1e5))


def khz(v):
    """kHz -> OpenGD77 10 Hz units."""
    return int(round(v * 1e2))


def build_mode(args):
    if args.retune not in RETUNE_VALID:
        print("WARNING: --retune %s does not actually move the receiver. Every point will "
              "return the anchor frequency's reading. Use it only as a negative control."
              % args.retune, file=sys.stderr)
    kind = RETUNE_NAMES[args.retune]
    mode = (kind & MODE_RETUNE_MASK) | (MODE_RETUNE_HI if (kind & 0x08) else 0)
    if not args.no_fm:
        mode |= MODE_FORCE_FM
    if args.wide:
        mode |= MODE_WIDE
    return mode


def read_exact(ser, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = ser.read(n - len(buf))
        if not chunk:
            break
        buf += chunk
    return bytes(buf)


# ------------------------------------------------------------------ Stage 0

def do_probe(ser, args):
    mode = build_mode(args)
    req = struct.pack(
        ">BBIIBBH", ord("C"), CMD_PROBE,
        mhz(args.fa), mhz(args.fb), mode, min(args.samples, MAX_PROBE_SAMPLES),
        args.interval,
    )
    ser.reset_input_buffer()
    ser.write(req)
    ser.flush()

    head = read_exact(ser, 8)
    if len(head) < 8 or head[0] != ord("C") or head[1] != CMD_PROBE:
        sys.exit("bad probe reply: %s" % head.hex())
    count, gotmode = head[2], head[3]
    if gotmode != mode:
        sys.exit("radio ran mode 0x%02X, host asked for 0x%02X -- host and firmware "
                 "disagree about the mode bits, so this measurement would be of "
                 "something other than what was asked for" % (gotmode, mode))
    retune_us, read_us = struct.unpack_from(">HH", head, 4)
    body = read_exact(ser, count * 4)
    if len(body) < count * 4:
        sys.exit("short probe body: got %d of %d" % (len(body), count * 4))

    samples = [struct.unpack_from(">HBB", body, i * 4) for i in range(count)]

    print("probe %.5f MHz -> %.5f MHz   retune=%s%s%s" % (
        args.fa, args.fb, args.retune,
        "  fm" if (gotmode & MODE_FORCE_FM) else "",
        "  25kHz" if (gotmode & MODE_WIDE) else "  12.5kHz"))
    print("  retune write sequence : %5d us" % retune_us)
    print("  one RSSI register read: %5d us" % read_us)
    print("  %d samples" % count)
    print()
    print("     t_us   rssi  noise")
    lo = min(s[1] for s in samples) if samples else 0
    hi = max(s[1] for s in samples) if samples else 0
    span = max(hi - lo, 1)
    for t, rssi, noise in samples:
        bar = "#" * int(round(40.0 * (rssi - lo) / span))
        print("  %7d   %3d   %3d  %s" % (t, rssi, noise, bar))

    if samples:
        analyse_settle(samples, retune_us)

    if args.csv:
        with open(args.csv, "w") as fh:
            fh.write("t_us,rssi,noise\n")
            for t, rssi, noise in samples:
                fh.write("%d,%d,%d\n" % (t, rssi, noise))
        print("\nwrote %s" % args.csv)


def analyse_settle(samples, retune_us):
    """Report when each curve last moved by more than a tolerance from its final value.

    'Settled' is defined against the tail of the trace, which is the honest
    definition for a swept receiver: the number we care about is how long after a
    retune the reading is the same as it would be if we waited forever.

    ★ BOTH bytes, not just RSSI. The scanner's stop decision is `trxRxNoise < squelch`,
    i.e. the LOW byte, and the two are filtered separately in the chip -- reporting only
    RSSI answers a question nobody is asking. `settle.py split` does this properly, with
    repeats and a measured floor; this is the quick look."""
    tail = samples[-max(3, len(samples) // 5):]

    print()
    for idx, label in ((1, "rssi "), (2, "noise")):
        vals = [s[idx] for s in tail]
        final = sum(vals) / float(len(vals))
        print("  final %s %.1f (last %d samples, spread %d)"
              % (label, final, len(vals), max(vals) - min(vals)))
        for tol in (1, 2, 3):
            settled_at = None
            for s in samples:
                if abs(s[idx] - final) > tol:
                    settled_at = None
                elif settled_at is None:
                    settled_at = s[0]
            if settled_at is None:
                print("    within +/-%d of final : never" % tol)
            else:
                print("    within +/-%d of final : %d us after the retune started "
                      "(%d us after it finished)"
                      % (tol, settled_at, max(0, settled_at - retune_us)))


# ------------------------------------------------------------------ Stage 1/2

def sweep_once(ser, f_start, step, n_points, dwell_us, mode):
    """One 0xA0 call. Returns (rssi list, noise list, elapsed_ms). May come back
    short: the firmware caps how long it holds its critical section."""
    req = struct.pack(">BBIIHHB", ord("C"), CMD_SWEEP, f_start, step, n_points, dwell_us, mode)
    ser.reset_input_buffer()
    ser.write(req)
    ser.flush()

    head = read_exact(ser, 6)
    if len(head) < 6 or head[0] != ord("C") or head[1] != CMD_SWEEP:
        raise IOError("bad sweep reply: %s" % head.hex())
    got, elapsed_ms = struct.unpack_from(">HH", head, 2)
    if got == 0:
        raise IOError("sweep refused -- out of band, or start and stop are in "
                      "different bands (a sweep may not straddle VHF/UHF)")
    body = read_exact(ser, got * 2)
    if len(body) < got * 2:
        raise IOError("short sweep body: got %d of %d" % (len(body), got * 2))
    return list(body[0::2]), list(body[1::2]), elapsed_ms


def session_begin(ser, anchor, mode):
    """Configure the receiver once and leave it running.

    Without this every 0xA0 call restarts the receiver, and the first tens of
    milliseconds of the trace are the receiver settling rather than the spectrum
    (measured: RSSI stable at ~15 ms, noise reading at ~30 ms). It also matters for
    a sweep that spans several calls -- otherwise the chunks are not comparable."""
    ser.reset_input_buffer()
    ser.write(struct.pack(">BBIB", ord("C"), CMD_SESSION_BEGIN, anchor, mode))
    ser.flush()
    reply = read_exact(ser, 3)
    if len(reply) < 3 or reply[2] != 1:
        raise IOError("could not open a sweep session (frequency out of band?)")


def session_end(ser):
    try:
        ser.write(struct.pack(">BB", ord("C"), CMD_SESSION_END))
        ser.flush()
        read_exact(ser, 3)
    except (serial.SerialException, OSError):
        pass   # the radio closes an abandoned session by itself after 3 s


def sweep_range(ser, args, in_session=False):
    """Full sweep, chunked over as many 0xA0 calls as it takes."""
    f_start = mhz(args.start)
    step = khz(args.step)
    n_total = int((mhz(args.stop) - f_start) // step) + 1
    mode = build_mode(args)

    if not in_session:
        session_begin(ser, f_start, mode)
    try:
        rssi, noise = [], []
        elapsed_ms = 0
        while len(rssi) < n_total:
            want = min(MAX_SWEEP_POINTS, n_total - len(rssi))
            r, nz, ms = sweep_once(ser, f_start + len(rssi) * step, step, want,
                                   args.dwell, mode)
            rssi += r
            noise += nz
            elapsed_ms += ms
    finally:
        if not in_session:
            session_end(ser)
    return rssi, noise, elapsed_ms, f_start, step


def do_sweep(ser, args):
    t0 = time.time()
    rssi, noise, elapsed_ms, f_start, step = sweep_range(ser, args)
    wall = time.time() - t0

    print("%d points  %.5f -> %.5f MHz  step %g kHz  dwell %d us" % (
        len(rssi), f_start / 1e5, (f_start + (len(rssi) - 1) * step) / 1e5,
        args.step, args.dwell))
    print("radio-side sweep time %d ms (%.0f us/point), wall %.0f ms including USB" % (
        elapsed_ms, 1000.0 * elapsed_ms / max(len(rssi), 1), wall * 1000))
    print()

    lo, hi = min(rssi), max(rssi)
    span = max(hi - lo, 1)
    for i, (r, nz) in enumerate(zip(rssi, noise)):
        f = (f_start + i * step) / 1e5
        bar = "#" * int(round(50.0 * (r - lo) / span))
        print("  %10.5f  %3d  %3d  %s" % (f, r, nz, bar))

    if args.csv:
        with open(args.csv, "w") as fh:
            fh.write("freq_hz,rssi,noise\n")
            for i, (r, nz) in enumerate(zip(rssi, noise)):
                fh.write("%d,%d,%d\n" % ((f_start + i * step) * 10, r, nz))
        print("\nwrote %s" % args.csv)


def do_waterfall(ser, args):
    import tkinter as tk

    # One session for the whole run: the receiver stays up between sweeps, which is
    # the only way the repeat rate is set by the sweep itself rather than by a 30 ms
    # receiver restart on every frame.
    session_begin(ser, mhz(args.start), build_mode(args))

    width = int((mhz(args.stop) - mhz(args.start)) // khz(args.step)) + 1
    height = args.rows
    scale = max(1, args.zoom)

    root = tk.Tk()
    root.title("UV390 swept RSSI  %.4f - %.4f MHz" % (args.start, args.stop))
    canvas = tk.Canvas(root, width=width * scale, height=height * scale + 40, bg="black",
                       highlightthickness=0)
    canvas.pack()
    status = tk.StringVar(value="starting...")
    tk.Label(root, textvariable=status, anchor="w").pack(fill="x")

    img = tk.PhotoImage(width=width, height=height)
    canvas.create_image(0, 0, image=img, anchor="nw")
    if scale > 1:
        canvas.scale("all", 0, 0, scale, scale)
    rows = []
    trace_ids = []
    state = {"running": True, "lo": args.floor, "hi": args.ceiling}

    def colour(v):
        """Simple black -> blue -> green -> yellow -> white ramp."""
        lo, hi = state["lo"], state["hi"]
        x = 0.0 if hi <= lo else max(0.0, min(1.0, (v - lo) / float(hi - lo)))
        if x < 0.25:
            r, g, b = 0, 0, int(255 * (x / 0.25))
        elif x < 0.5:
            r, g, b = 0, int(255 * ((x - 0.25) / 0.25)), 255 - int(255 * ((x - 0.25) / 0.25))
        elif x < 0.75:
            r, g, b = int(255 * ((x - 0.5) / 0.25)), 255, 0
        else:
            r = g = 255
            b = int(255 * ((x - 0.75) / 0.25))
        return "#%02x%02x%02x" % (r, g, b)

    def tick():
        if not state["running"]:
            return
        try:
            rssi, _noise, elapsed_ms, f_start, step = sweep_range(ser, args, in_session=True)
        except IOError as exc:
            status.set(str(exc))
            root.after(1000, tick)
            return

        if args.auto:
            state["lo"], state["hi"] = min(rssi), max(min(rssi) + 4, max(rssi))

        rows.insert(0, rssi[:width])
        del rows[height:]
        for y, row in enumerate(rows):
            img.put("{" + " ".join(colour(v) for v in row) + "}", to=(0, y))

        # live trace along the bottom
        for tid in trace_ids:
            canvas.delete(tid)
        del trace_ids[:]
        base = height * scale + 38
        lo, hi = state["lo"], state["hi"]
        rng = max(hi - lo, 1)
        pts = []
        for x, v in enumerate(rssi[:width]):
            pts += [x * scale, base - int(36.0 * max(0, min(rng, v - lo)) / rng)]
        if len(pts) >= 4:
            trace_ids.append(canvas.create_line(*pts, fill="#40ff40"))

        peak = max(range(len(rssi)), key=lambda i: rssi[i])
        status.set("peak %.5f MHz  rssi %d   scale %d..%d   sweep %d ms (%.0f us/pt)" % (
            (f_start + peak * step) / 1e5, rssi[peak], state["lo"], state["hi"],
            elapsed_ms, 1000.0 * elapsed_ms / max(len(rssi), 1)))
        root.after(1, tick)

    def stop(_evt=None):
        state["running"] = False
        root.destroy()

    root.protocol("WM_DELETE_WINDOW", stop)
    root.bind("<Escape>", stop)
    root.after(100, tick)
    try:
        root.mainloop()
    finally:
        session_end(ser)


# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="serial port (default: auto-detect 1FC9:0094)")
    sub = ap.add_subparsers(dest="cmd", required=True)

    def common(p):
        p.add_argument("--retune", choices=sorted(RETUNE_NAMES), default="latch",
                       help="latch = PLL registers + the RX off/on edge that actually "
                            "moves the receiver (default); radio = radioSetFrequency(); "
                            "trx = full trxSetFrequency(). fast/poke30/poke05 DO NOT "
                            "retune at all and exist only as negative controls")
        p.add_argument("--no-fm", action="store_true",
                       help="leave the radio in whatever mode it is in (default: force analog FM)")
        p.add_argument("--wide", action="store_true", help="25 kHz IF instead of 12.5 kHz")
        p.add_argument("--csv", help="also write the readings to this file")

    p = sub.add_parser("probe", help="Stage 0: retune -> settle -> RSSI timing")
    common(p)
    p.add_argument("--fa", type=float, required=True, help="park frequency, MHz")
    p.add_argument("--fb", type=float, required=True, help="frequency to jump to, MHz")
    p.add_argument("--samples", type=int, default=120, help="RSSI samples after the jump")
    p.add_argument("--interval", type=int, default=0,
                   help="us between samples (0 = as fast as the I2C bus allows)")

    p = sub.add_parser("sweep", help="one trace")
    common(p)
    p.add_argument("--start", type=float, required=True, help="MHz")
    p.add_argument("--stop", type=float, required=True, help="MHz")
    p.add_argument("--step", type=float, default=12.5, help="kHz (default 12.5)")
    p.add_argument("--dwell", type=int, default=DEFAULT_DWELL_US,
                   help="us to wait after each retune (default %d = measured settle to "
                        "+/-2 counts; %d is enough merely to detect a carrier)"
                        % (DEFAULT_DWELL_US, DETECT_DWELL_US))

    p = sub.add_parser("water", help="scrolling waterfall")
    common(p)
    p.add_argument("--start", type=float, required=True, help="MHz")
    p.add_argument("--stop", type=float, required=True, help="MHz")
    p.add_argument("--step", type=float, default=12.5, help="kHz (default 12.5)")
    p.add_argument("--dwell", type=int, default=DETECT_DWELL_US,
                   help="us to wait after each retune (default %d = measured carrier-"
                        "detection time; the waterfall wants rate over accuracy)"
                        % DETECT_DWELL_US)
    p.add_argument("--rows", type=int, default=200, help="waterfall history rows")
    p.add_argument("--zoom", type=int, default=2, help="pixel scale")
    p.add_argument("--floor", type=int, default=40, help="RSSI mapped to black")
    p.add_argument("--ceiling", type=int, default=110, help="RSSI mapped to white")
    p.add_argument("--auto", action="store_true", help="autoscale the colour ramp each sweep")

    args = ap.parse_args()
    ser = open_radio(args)
    try:
        {"probe": do_probe, "sweep": do_sweep, "water": do_waterfall}[args.cmd](ser, args)
    finally:
        ser.close()


if __name__ == "__main__":
    main()
