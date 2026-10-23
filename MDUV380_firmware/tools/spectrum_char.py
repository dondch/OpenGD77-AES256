#!/usr/bin/env python3
"""Characterise the fork's swept-RSSI receiver against a HackRF signal generator.

Answers the questions the Stage 0 timing measurements could not, because they were all
taken on the bare noise floor: how fast the RSSI reading responds to a real signal, what
the resolution bandwidth and filter shape actually are, how many dB one RSSI count is
worth, and where the noise floor and the top of the range sit.

The radio is on a Windows COM port and the HackRF lives in WSL, so this drives the radio
directly and shells out for the HackRF. Override --hackrf-cmd to run it all on one host.

  floor    no transmitter. Per-bin noise statistics and sweep-to-sweep repeatability,
           which is what sets the smallest change this thing can resolve.
  shape    TX a CW carrier, sweep finely across it at a long dwell. The result IS the
           resolution bandwidth and the IF filter shape.
  dwell    TX a CW carrier, sweep across it at a range of dwells, compared against a
           long-dwell reference. Gives the settle time AND shows up any smearing.
  level    TX a CW carrier at each HackRF TXVGA setting (exact 1 dB steps). Gives the
           RSSI-to-dB slope, the noise floor in the same units, and the compression point.

EVERY MODE EXCEPT `floor` TRANSMITS. Keep the level low, use a cable and attenuators if
you have them, and pick a frequency you are allowed to use.
"""
import argparse
import math
import os
import struct
import subprocess
import sys
import time

import serial
from serial.tools import list_ports

APP_VID, APP_PID = 0x1FC9, 0x0094
CMD_SWEEP, CMD_SESSION_BEGIN, CMD_SESSION_END = 0xA0, 0xA2, 0xA3

# ★ CORRECTED 2026-07-28. These were 0x04 / 0x08, one bit adrift of spectrum.h, from the
# day the file was written. The mode byte's low three bits are the RETUNE METHOD, so
# 0x04 was not "force FM" at all -- it was SPECTRUM_RETUNE_POKE30, which rewrites reg
# 0x30 with the value it already holds and is a measured no-op: the receiver never left
# the session's anchor frequency. And with --wide off, FORCE_FM was never set either, so
# the radio stayed in whatever mode it happened to be in.
#
# Nothing failed, because nothing can: every point returned the anchor's reading, which
# is a perfectly plausible flat trace. `shape` was measuring the filter shape of a
# receiver that was not being tuned across the carrier, and the settle number `dwell`
# reports was the settle of a retune that never happened. TREAT ANY spectrum_char.py
# RESULT TAKEN BEFORE THIS DATE AS VOID and re-measure.
#
# The retune method is now explicit rather than left at 0 (which is SPECTRUM_RETUNE_FAST,
# itself a proven no-op) -- see RETUNE_LATCH.
MODE_FORCE_FM, MODE_WIDE = 0x08, 0x10
RETUNE_LATCH = 0x03   # PLL registers + the RX off->on edge that actually moves the receiver
MAX_SWEEP_POINTS = 480


def buildMode(args):
    return RETUNE_LATCH | MODE_FORCE_FM | (MODE_WIDE if args.wide else 0)

# HackRF CW generation. The tone is offset from the LO so it does not sit on top of the
# transmitter's own DC/LO leakage, which would make a spur look like the signal.
TX_SAMPLE_RATE = 2000000
TX_TONE_OFFSET = 250000
TX_IQ_PATH = "~/cw_250k.iq"


# ------------------------------------------------------------------ radio side

def find_port(explicit=None):
    if explicit:
        return explicit
    for p in list_ports.comports():
        if p.vid == APP_VID and p.pid == APP_PID:
            return p.device
    sys.exit("radio not found (USB 1FC9:0094)")


def read_exact(ser, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = ser.read(n - len(buf))
        if not chunk:
            break
        buf += chunk
    return bytes(buf)


def session_begin(ser, anchor, mode):
    ser.reset_input_buffer()
    ser.write(struct.pack(">BBIB", ord("C"), CMD_SESSION_BEGIN, anchor, mode))
    ser.flush()
    r = read_exact(ser, 3)
    if len(r) < 3 or r[2] != 1:
        raise IOError("could not open a sweep session (frequency out of band?)")


def session_end(ser):
    try:
        ser.write(struct.pack(">BB", ord("C"), CMD_SESSION_END))
        ser.flush()
        read_exact(ser, 3)
    except (serial.SerialException, OSError):
        pass


def sweep(ser, f_start, step, n_points, dwell_us, mode):
    """One sweep, chunked as needed. Frequencies in OpenGD77 10 Hz units."""
    rssi, noise = [], []
    while len(rssi) < n_points:
        want = min(MAX_SWEEP_POINTS, n_points - len(rssi))
        ser.reset_input_buffer()
        ser.write(struct.pack(">BBIIHHB", ord("C"), CMD_SWEEP,
                              f_start + len(rssi) * step, step, want, dwell_us, mode))
        ser.flush()
        head = read_exact(ser, 6)
        if len(head) < 6 or head[0] != ord("C"):
            raise IOError("bad sweep reply: %s" % head.hex())
        got, _ms = struct.unpack_from(">HH", head, 2)
        if got == 0:
            raise IOError("sweep refused (out of band, or start and stop differ in band)")
        body = read_exact(ser, got * 2)
        rssi += list(body[0::2])
        noise += list(body[1::2])
    return rssi, noise


# ----------------------------------------------------------------- HackRF side

class HackRF:
    """CW generator. Each level change restarts hackrf_transfer, because its gain is
    fixed for the life of the process."""

    def __init__(self, cmd_prefix, dry_run=False):
        self.prefix = cmd_prefix
        self.dry_run = dry_run
        self.proc = None

    def _run(self, shell_cmd, background=False):
        argv = self.prefix + [shell_cmd] if self.prefix else ["bash", "-lc", shell_cmd]
        if self.dry_run:
            print("    [dry-run] %s" % shell_cmd)
            return None
        if background:
            return subprocess.Popen(argv, stdout=subprocess.DEVNULL,
                                    stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL)
        return subprocess.run(argv, capture_output=True, text=True, timeout=120)

    def make_cw(self):
        """Write a one-second looping complex sinusoid as signed 8-bit IQ."""
        # The offset divides exactly into the sample rate, so one 2000-sample period is a
        # whole number of cycles and hackrf_transfer -R loops it without a phase step.
        # Written via shell redirection so the shell, not Python, expands the ~ in the path.
        gen = (
            "python3 -c \""
            "import sys,math;"
            "o=%d;sr=%d;"
            "a=[(int(round(110*math.cos(2*math.pi*o*i/sr))),"
            "int(round(110*math.sin(2*math.pi*o*i/sr)))) for i in range(sr//1000)];"
            "sys.stdout.buffer.write(bytes(bytearray([x&0xff for p in a for x in p]))*1000)"
            "\" > %s"
        ) % (TX_TONE_OFFSET, TX_SAMPLE_RATE, TX_IQ_PATH)
        r = self._run(gen)
        if r is not None and r.returncode != 0:
            raise RuntimeError("could not build the CW file: %s" % r.stderr)

    def tx_start(self, freq_hz, txvga_db, amp=False):
        """Transmit CW at freq_hz. The HackRF is tuned low by the tone offset so the
        carrier lands exactly on freq_hz."""
        lo = freq_hz - TX_TONE_OFFSET
        cmd = ("hackrf_transfer -t %s -f %d -s %d -x %d -a %d -R"
               % (TX_IQ_PATH, lo, TX_SAMPLE_RATE, txvga_db, 1 if amp else 0))
        self.tx_stop()
        self.proc = self._run(cmd, background=True)
        time.sleep(1.2)   # let it tune and start streaming

    def tx_stop(self):
        if self.proc is not None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
            self.proc = None
        # -x, not -f: the shell we are running this in has "hackrf_transfer" in its own
        # command line, so a -f match would kill the shell doing the killing.
        self._run("pkill -x hackrf_transfer || true")
        time.sleep(0.4)


# ------------------------------------------------------------------ experiments

def stat(xs):
    m = sum(xs) / float(len(xs))
    sd = math.sqrt(sum((x - m) ** 2 for x in xs) / float(len(xs))) if len(xs) > 1 else 0.0
    return m, sd


def do_floor(ser, args, _hrf):
    """No transmitter. How steady is a bin, and how steady is the floor across a span?"""
    f0, step, n = mhz(args.freq) - (args.points // 2) * khz(args.step), khz(args.step), args.points
    mode = buildMode(args)

    session_begin(ser, f0, mode)
    try:
        sweeps = [sweep(ser, f0, step, n, args.dwell, mode)[0] for _ in range(args.repeats)]
    finally:
        session_end(ser)

    print("noise floor, no transmitter: %d sweeps x %d bins, %.2f kHz step, dwell %d us"
          % (args.repeats, n, args.step, args.dwell))
    allv = [v for s in sweeps for v in s]
    m, sd = stat(allv)
    print("  overall  mean %.2f  sd %.2f  min %d  max %d" % (m, sd, min(allv), max(allv)))

    # Per-bin repeatability: the sd of one bin across sweeps is the measurement noise,
    # i.e. how big a change has to be before it means anything.
    perbin = [stat([s[i] for s in sweeps])[1] for i in range(n)]
    pm, _ = stat(perbin)
    print("  per-bin sd across sweeps: mean %.2f  max %.2f  (counts)" % (pm, max(perbin)))

    # Spatial variation of the floor: the shape of the front end across the span.
    means = [stat([s[i] for s in sweeps])[0] for i in range(n)]
    print("  floor across the span: min %.1f  max %.1f  (tilt %.1f counts)"
          % (min(means), max(means), max(means) - min(means)))
    print("  => a single sweep resolves a change of about %.1f counts (3 sigma)" % (3 * pm))
    if args.csv:
        write_csv(args.csv, f0, step, [("bin_mean", means), ("bin_sd", perbin)])


def do_shape(ser, args, hrf):
    """The filter shape measured at a long dwell IS the resolution bandwidth."""
    mode = buildMode(args)
    f_centre = mhz(args.freq)
    step = khz(args.step)
    n = args.points
    f0 = f_centre - (n // 2) * step

    hrf.tx_start(int(args.freq * 1e6), args.gain)
    try:
        session_begin(ser, f0, mode)
        try:
            acc = [0] * n
            for _ in range(args.repeats):
                r, _nz = sweep(ser, f0, step, n, args.dwell, mode)
                acc = [a + b for a, b in zip(acc, r)]
            prof = [a / float(args.repeats) for a in acc]
        finally:
            session_end(ser)
    finally:
        hrf.tx_stop()

    peak = max(prof)
    print("filter shape at %.5f MHz, TXVGA %d dB, dwell %d us, %s IF"
          % (args.freq, args.gain, args.dwell, "25 kHz" if args.wide else "12.5 kHz"))
    print("  peak %.1f counts at offset %+.2f kHz"
          % (peak, (prof.index(peak) - n // 2) * args.step))
    for down in (3, 6, 20, 40):
        bw = bandwidth_at(prof, peak, down, args.step)
        print("  -%2d dB bandwidth: %s" % (down, ("%.2f kHz" % bw) if bw else "beyond the span"))
    print()
    print("   offset kHz   rssi")
    for i, v in enumerate(prof):
        off = (i - n // 2) * args.step
        bar = "#" * int(round(50.0 * (v - min(prof)) / max(peak - min(prof), 1)))
        print("  %+9.2f   %5.1f  %s" % (off, v, bar))
    if args.csv:
        write_csv(args.csv, f0, step, [("rssi", prof)])


def bandwidth_at(prof, peak, down_db, step_khz):
    """Width where the response falls `down_db` below the peak, assuming 1 count = 1 dB
    (which `level` verifies). Interpolated between bins."""
    thr = peak - down_db
    idx = [i for i, v in enumerate(prof) if v >= thr]
    if not idx or idx[0] == 0 or idx[-1] == len(prof) - 1:
        return None
    return (idx[-1] - idx[0]) * step_khz


def do_dwell(ser, args, hrf):
    """The Stage 0 number that the noise floor could not give: how long after a retune is
    the reading right? Compared against a very long dwell as ground truth."""
    mode = buildMode(args)
    step = khz(args.step)
    n = args.points
    f0 = mhz(args.freq) - (n // 2) * step
    dwells = [int(d) for d in args.dwells.split(",")]

    hrf.tx_start(int(args.freq * 1e6), args.gain)
    try:
        session_begin(ser, f0, mode)
        try:
            ref = averaged(ser, f0, step, n, args.reference_dwell, mode, args.repeats)
            results = [(d, averaged(ser, f0, step, n, d, mode, args.repeats)) for d in dwells]
        finally:
            session_end(ser)
    finally:
        hrf.tx_stop()

    ref_peak = max(ref)
    ref_floor = min(ref)
    print("dwell response at %.5f MHz, TXVGA %d dB, %.2f kHz step"
          % (args.freq, args.gain, args.step))
    print("  reference dwell %d us: peak %.1f, floor %.1f (%.1f counts of signal)"
          % (args.reference_dwell, ref_peak, ref_floor, ref_peak - ref_floor))
    print()
    print("  dwell us   peak   loss vs ref   asymmetry   us/point")
    for d, prof in results:
        peak = max(prof)
        asym = tail_asymmetry(prof, prof.index(max(prof)))
        print("  %8d  %5.1f   %+6.1f dB     %+6.1f     %6.0f"
              % (d, peak, peak - ref_peak, asym, 250 + d))
    print()
    print("  'loss vs ref' is how much signal a short dwell throws away; it going to zero")
    print("  is the settle time. 'asymmetry' is the mean of the 3 bins after the peak minus")
    print("  the 3 before it -- positive means a strong signal is smearing forwards into")
    print("  the bins the sweep visits next, which costs resolution no matter the IF filter.")
    if args.csv:
        write_csv(args.csv, f0, step,
                  [("ref_%d" % args.reference_dwell, ref)] +
                  [("dwell_%d" % d, p) for d, p in results])


def averaged(ser, f0, step, n, dwell, mode, repeats):
    acc = [0] * n
    for _ in range(repeats):
        r, _nz = sweep(ser, f0, step, n, dwell, mode)
        acc = [a + b for a, b in zip(acc, r)]
    return [a / float(repeats) for a in acc]


def tail_asymmetry(prof, pk):
    if pk < 3 or pk + 3 >= len(prof):
        return 0.0
    after = sum(prof[pk + 1:pk + 4]) / 3.0
    before = sum(prof[pk - 3:pk]) / 3.0
    return after - before


def do_level(ser, args, hrf):
    """RSSI against a transmitter level that steps in exact 1 dB increments. Gives the
    counts-per-dB slope, the usable range, and where it compresses."""
    mode = buildMode(args)
    f = mhz(args.freq)
    gains = list(range(args.gain_min, args.gain_max + 1, args.gain_step))

    session_begin(ser, f, mode)
    try:
        hrf.tx_stop()
        floor = averaged(ser, f, 0, 8, args.dwell, mode, args.repeats)
        floor_mean = sum(floor) / len(floor)
        rows = []
        for g in gains:
            hrf.tx_start(int(args.freq * 1e6), g)
            v = averaged(ser, f, 0, 8, args.dwell, mode, args.repeats)
            rows.append((g, sum(v) / len(v)))
            print("    TXVGA %2d dB -> rssi %.1f" % (g, rows[-1][1]))
    finally:
        hrf.tx_stop()
        session_end(ser)

    print()
    print("level response at %.5f MHz, dwell %d us" % (args.freq, args.dwell))
    print("  noise floor with the transmitter off: %.1f counts" % floor_mean)
    print()
    print("  TXVGA dB   rssi   counts/dB (local)")
    prev = None
    for g, v in rows:
        slope = "" if prev is None else "%.2f" % ((v - prev[1]) / float(g - prev[0]))
        print("  %8d  %5.1f   %s" % (g, v, slope))
        prev = (g, v)

    lin = [(g, v) for g, v in rows if v > floor_mean + 3 and v < 250]
    if len(lin) >= 2:
        slope = (lin[-1][1] - lin[0][1]) / float(lin[-1][0] - lin[0][0])
        print()
        print("  slope over the usable region: %.3f counts/dB "
              "(1.000 would mean 1 count = 1 dB, which is what the firmware assumes)" % slope)
        print("  usable range seen here: %.1f dB of transmitter level, "
              "rssi %.0f..%.0f" % (lin[-1][0] - lin[0][0], lin[0][1], lin[-1][1]))
    if args.csv:
        with open(args.csv, "w") as fh:
            fh.write("txvga_db,rssi\n")
            fh.write("off,%.2f\n" % floor_mean)
            for g, v in rows:
                fh.write("%d,%.2f\n" % (g, v))
        print("\nwrote %s" % args.csv)


def write_csv(path, f0, step, series):
    n = len(series[0][1])
    with open(path, "w") as fh:
        fh.write("freq_hz," + ",".join(name for name, _ in series) + "\n")
        for i in range(n):
            fh.write("%d," % ((f0 + i * step) * 10) +
                     ",".join("%.3f" % vals[i] for _, vals in series) + "\n")
    print("\nwrote %s" % path)


def mhz(v):
    return int(round(v * 1e5))


def khz(v):
    return int(round(v * 1e2))


# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["floor", "shape", "dwell", "level"])
    ap.add_argument("--freq", type=float, required=True, help="carrier / centre, MHz")
    ap.add_argument("--port", help="radio serial port (default: auto-detect)")
    ap.add_argument("--step", type=float, default=1.0, help="sweep step, kHz")
    ap.add_argument("--points", type=int, default=101, help="bins, centred on --freq")
    ap.add_argument("--dwell", type=int, default=0, help="us")
    ap.add_argument("--repeats", type=int, default=8, help="sweeps to average")
    ap.add_argument("--wide", action="store_true", help="25 kHz IF instead of 12.5 kHz")
    ap.add_argument("--gain", type=int, default=20, help="HackRF TXVGA gain, 0-47 dB")
    ap.add_argument("--gain-min", type=int, default=0)
    ap.add_argument("--gain-max", type=int, default=47)
    ap.add_argument("--gain-step", type=int, default=2)
    ap.add_argument("--dwells", default="0,100,200,500,1000,2000,5000")
    ap.add_argument("--reference-dwell", type=int, default=20000)
    ap.add_argument("--csv")
    ap.add_argument("--hackrf-cmd", default="wsl -d Ubuntu-24.04 bash -lc",
                    help="prefix used to run HackRF commands ('' = run them locally)")
    ap.add_argument("--dry-run", action="store_true", help="print HackRF commands, do not TX")
    args = ap.parse_args()

    hrf = HackRF(args.hackrf_cmd.split() if args.hackrf_cmd else None, args.dry_run)
    if args.mode != "floor":
        print("*** %s transmits on %.5f MHz ***" % (args.mode, args.freq))
        hrf.make_cw()

    ser = serial.Serial(find_port(args.port), 115200, timeout=2.0)
    try:
        {"floor": do_floor, "shape": do_shape, "dwell": do_dwell, "level": do_level}[args.mode](
            ser, args, hrf)
    finally:
        hrf.tx_stop()
        ser.close()


main()
