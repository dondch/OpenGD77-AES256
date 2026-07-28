#!/usr/bin/env python3
"""Take the AT1846S retune settle apart: is the ~4.4 ms lock, filter, or neither?

Everything measured so far has treated "settle" as one number, because the probe was
always read as a single RSSI curve. It is not one thing. VK3KYY's description of the
wall is *"PLL lock time and low pass filter on the RSSI and also S/N values"* -- two
mechanisms with completely different fixes, never separated. And the trigger we pay for
(an RX off->on edge on 30H[5], costing a full receiver restart) is nowhere in the vendor
guide, whose entire frequency procedure is "write 29H and 2AH" -- so it is undocumented
behaviour rather than a datasheet requirement, and a cheaper trigger may exist.

  split      Park on fA, jump to fB with a carrier there, and report the RSSI curve and
             the noise/SNR curve SEPARATELY -- both from the same 0x1B read, so the two
             share a timestamp and need no cross-run alignment. Answers lead 1: if RSSI
             is there long before noise is, the scanner is waiting on the wrong byte.
  triggers   The same jump under every retune method, including the two controls, and a
             separation column that says which ones moved the receiver at all.
             Answers lead 2.
  regs       Snapshot every AT1846S register; diff two snapshots. Used to find which
             status bit follows the hardware squelch, by comparing carrier on vs off.
  sq         Report the squelch threshold the scanner is really using, and the live
             reading, straight from the radio (CPS 0xAC).

Needs firmware built with ENABLE_SPECTRUM. Every mode except `sq` and `regs` needs a
carrier on fB from a signal generator; NOTHING here transmits.

Examples:
  python settle.py sq
  python settle.py split    --fa 430.0 --fb 433.5 --reps 5 --csv split.csv
  python settle.py triggers --fa 430.0 --fb 433.5 --reps 3
  python settle.py regs --save carrier_on.json          # then again with TX off
  python settle.py regs --diff carrier_on.json carrier_off.json
"""
import argparse
import json
import struct
import sys

import serial
from serial.tools import list_ports

APP_VID, APP_PID = 0x1FC9, 0x0094

CMD_SWEEP = 0xA0
CMD_PROBE = 0xA1
CMD_SESSION_BEGIN = 0xA2
CMD_SESSION_END = 0xA3
CMD_OVERRIDES = 0xA4
CMD_REG_READ = 0xA5
CMD_REG_WRITE = 0xAB
CMD_SQUELCH = 0xAC

# Retune index -> name. 0..6 predate this session; 7..10 are the candidate cheap triggers.
# `fast` writes only the PLL registers and is PROVEN not to move the receiver: it is the
# negative control and the yardstick every other method's separation is measured against.
# `latch` is the cheapest method known to work: it is the positive control.
RETUNE = {
    "fast": 0, "radio": 1, "trx": 2, "latch": 3,
    "poke30": 4, "poke05": 5, "late30": 6,
    "band0f": 7, "sqtoggle": 8, "xtal": 9, "hilast": 10,
}
CONTROL_NEG, CONTROL_POS = "fast", "latch"

# Mode-byte layout, and the reason this file asserts the radio echoed what it was sent:
# spectrum_char.py shipped with these two constants one bit adrift of spectrum.h and
# silently sent every measurement it ever took as retune method 4 (a no-op) with FM force
# off. A mode-bit mistake does not raise anything -- it returns a plausible flat trace.
MODE_FORCE_FM = 0x08
MODE_WIDE = 0x10
MODE_RETUNE_MASK = 0x07
MODE_RETUNE_HI = 0x20

MAX_SAMPLES = 200


def modeByte(retuneName, fm=True, wide=False):
    kind = RETUNE[retuneName]
    mode = (kind & MODE_RETUNE_MASK) | (MODE_RETUNE_HI if (kind & 0x08) else 0)
    if fm:
        mode |= MODE_FORCE_FM
    if wide:
        mode |= MODE_WIDE
    return mode


def mhz(v):
    """MHz -> OpenGD77 10 Hz units."""
    return int(round(v * 1e5))


# ------------------------------------------------------------------- transport

def findPort(explicit=None):
    if explicit:
        return explicit
    for p in list_ports.comports():
        if (p.vid == APP_VID) and (p.pid == APP_PID):
            return p.device
    sys.exit("radio not found (USB 1FC9:0094) -- is it in normal mode, not DFU?")


def readExact(ser, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = ser.read(n - len(buf))
        if not chunk:
            break
        buf += chunk
    return bytes(buf)


def probe(ser, fA, fB, mode, nSamples=MAX_SAMPLES, intervalUs=0, reg=0):
    """CPS 0xA1. Returns (samples, retuneUs, readUs) with samples = [(tUs, hi, lo)].

    `reg` 0 means 0x1B, whose bytes are (rssi, noise). Point it elsewhere to time a
    different register on the same footing -- the hardware squelch status, say."""
    req = struct.pack(">BBIIBBHB", ord("C"), CMD_PROBE, fA, fB, mode,
                      min(nSamples, MAX_SAMPLES), intervalUs, reg)
    ser.reset_input_buffer()
    ser.write(req)
    ser.flush()

    head = readExact(ser, 8)
    if (len(head) < 8) or (head[0] != ord("C")) or (head[1] != CMD_PROBE):
        sys.exit("bad probe reply: %s" % head.hex())
    count, gotMode = head[2], head[3]
    retuneUs, readUs = struct.unpack_from(">HH", head, 4)

    # The whole point of the echo. A silently wrong mode is the failure this tooling has
    # already had once, and it looks like data rather than like an error.
    if gotMode != mode:
        sys.exit("radio ran mode 0x%02X, host asked for 0x%02X -- host and firmware "
                 "disagree about the mode bits" % (gotMode, mode))
    if count == 0:
        sys.exit("probe returned nothing -- frequency out of band?")

    body = readExact(ser, count * 4)
    if len(body) < (count * 4):
        sys.exit("short probe body: got %d of %d" % (len(body), count * 4))
    samples = [struct.unpack_from(">HBB", body, i * 4) for i in range(count)]
    return samples, retuneUs, readUs


def readReg(ser, reg):
    ser.reset_input_buffer()
    ser.write(struct.pack(">BBB", ord("C"), CMD_REG_READ, reg))
    ser.flush()
    r = readExact(ser, 6)
    if (len(r) < 6) or (r[5] != 1):
        return None
    return (r[3] << 8) | r[4]


def writeReg(ser, reg, value):
    ser.reset_input_buffer()
    ser.write(struct.pack(">BBBBB", ord("C"), CMD_REG_WRITE, reg,
                          (value >> 8) & 0xFF, value & 0xFF))
    ser.flush()
    r = readExact(ser, 6)
    return (len(r) == 6) and (r[5] == 1)


def sessionBegin(ser, anchor, mode):
    """Park the receiver on `anchor` and leave it there (CPS 0xA2).

    A register snapshot is only worth taking while the receiver is actually on the
    carrier, and the radio otherwise sits on whatever channel it is tuned to. The
    session self-closes after SPECTRUM_SESSION_TIMEOUT_MS (3 s) without a sweep, which
    is shorter than a full 128-register read takes -- hence sessionPoke() below."""
    ser.reset_input_buffer()
    ser.write(struct.pack(">BBIB", ord("C"), CMD_SESSION_BEGIN, anchor, mode))
    ser.flush()
    r = readExact(ser, 3)
    if (len(r) < 3) or (r[2] != 1):
        sys.exit("could not open a sweep session (frequency out of band?)")


def sessionPoke(ser, anchor, mode):
    """A one-point sweep, purely to reset the session's idle timer."""
    ser.reset_input_buffer()
    ser.write(struct.pack(">BBIIHHB", ord("C"), CMD_SWEEP, anchor, 0, 1, 100, mode))
    ser.flush()
    readExact(ser, 8)


def sessionEnd(ser):
    try:
        ser.write(struct.pack(">BB", ord("C"), CMD_SESSION_END))
        ser.flush()
        readExact(ser, 3)
    except (serial.SerialException, OSError):
        pass   # the radio closes an abandoned session by itself after 3 s


def setOverrides(ser, triplets):
    """CPS 0xA4: registers re-applied immediately after EVERY retune.

    A plain 0xAB write does not survive on a register the retune itself touches -- reg
    0x30 carries rx_on, so every latch rewrites it and undoes anything set there a
    moment earlier. The override table is applied last, which is the only way to hold a
    bit like sq_on on across a sweep or a probe."""
    payload = bytearray([ord("C"), CMD_OVERRIDES, len(triplets)])
    for reg, hi, lo in triplets:
        payload += bytes([reg, hi, lo])
    ser.reset_input_buffer()
    ser.write(bytes(payload))
    ser.flush()
    r = readExact(ser, 3)
    return (len(r) == 3) and (r[2] == len(triplets))


def readSquelch(ser):
    """(threshold, rssi, noise). The threshold is the one trxCarrierDetected() uses."""
    ser.reset_input_buffer()
    ser.write(struct.pack(">BB", ord("C"), CMD_SQUELCH))
    ser.flush()
    r = readExact(ser, 5)
    if (len(r) < 5) or (r[1] != CMD_SQUELCH):
        sys.exit("no squelch reply (firmware too old for CPS 0xAC?): %s" % r.hex())
    return r[2], r[3], r[4]


# -------------------------------------------------------------------- analysis

def finalValue(vals, frac=0.2):
    """Mean of the tail. 'Settled' can only honestly be defined against where the
    reading ends up if you wait forever, so everything below is relative to this."""
    n = max(3, int(len(vals) * frac))
    tail = vals[-n:]
    return sum(tail) / float(len(tail))


def settleTime(ts, vals, final, tol):
    """Last time the curve was further than `tol` from `final`, i.e. the time after
    which it never leaves the band again. None means it never settles that tightly."""
    settledAt = None
    for t, v in zip(ts, vals):
        if abs(v - final) > tol:
            settledAt = None
        elif settledAt is None:
            settledAt = t
    return settledAt


def crossTime(ts, vals, level, rising):
    """First time the curve crosses `level` and stays across for the rest of the trace.

    Sustained, not first-touch: a scanner that stops on a single lucky sample and then
    resumes is not detecting, and one noisy sample early would otherwise report a
    detection time far below anything reproducible."""
    crossedAt = None
    for t, v in zip(ts, vals):
        over = (v >= level) if rising else (v <= level)
        if not over:
            crossedAt = None
        elif crossedAt is None:
            crossedAt = t
    return crossedAt


def fmtUs(v):
    return "never" if v is None else ("%d us" % v)


def median(xs):
    xs = sorted(x for x in xs if x is not None)
    if not xs:
        return None
    mid = len(xs) // 2
    return xs[mid] if (len(xs) % 2) else ((xs[mid - 1] + xs[mid]) / 2.0)


# ------------------------------------------------------------------ mode: split

def doSplit(ser, args):
    mode = modeByte(args.retune, fm=(not args.no_fm), wide=args.wide)
    fA, fB = mhz(args.fa), mhz(args.fb)
    squelch, liveRssi, liveNoise = readSquelch(ser)

    print("split settle: %.5f -> %.5f MHz, retune=%s, mode 0x%02X, %d reps"
          % (args.fa, args.fb, args.retune, mode, args.reps))
    print("radio: squelch threshold %d (the scanner stops on noise < %d), "
          "live rssi %d noise %d" % (squelch, squelch, liveRssi, liveNoise))

    # Baseline: the same probe with no jump. Its tail is what the receiver reads while
    # parked on fA, which is the floor both curves start from. Measured rather than taken
    # from the first few samples of the real jump, where the retune writes are still in
    # flight and the reading is neither one frequency nor the other.
    base, _, _ = probe(ser, fA, fA, mode, args.samples, args.interval, args.reg)
    baseRssi = finalValue([s[1] for s in base])
    baseNoise = finalValue([s[2] for s in base])
    print("parked on fA (no jump): rssi %.1f  noise %.1f" % (baseRssi, baseNoise))
    print()

    runs = []
    for rep in range(args.reps):
        samples, retuneUs, readUs = probe(ser, fA, fB, mode, args.samples, args.interval,
                                          args.reg)
        ts = [s[0] for s in samples]
        rssi = [s[1] for s in samples]
        noise = [s[2] for s in samples]
        runs.append((ts, rssi, noise, retuneUs, readUs))

    span = runs[0][0][-1]
    print("retune writes %d us, one 0x1B read %d us, trace %d samples over %.1f ms"
          % (runs[0][3], runs[0][4], len(runs[0][0]), span / 1000.0))
    print()

    # ---- the trace itself, first rep, so the shape is visible and not just summarised
    ts, rssi, noise, _, _ = runs[0]
    finalRssi, finalNoise = finalValue(rssi), finalValue(noise)
    print("     t_us   rssi  noise   (r = rssi, n = noise, S = noise below squelch)")
    step = max(1, len(ts) // args.rows)
    rLo, rHi = min(rssi), max(rssi)
    nLo, nHi = min(noise), max(noise)
    for i in range(0, len(ts), step):
        rBar = int(round(24.0 * (rssi[i] - rLo) / max(rHi - rLo, 1)))
        nBar = int(round(24.0 * (noise[i] - nLo) / max(nHi - nLo, 1)))
        print("  %7d   %3d   %3d   %-25s|%-25s %s"
              % (ts[i], rssi[i], noise[i], "r" * rBar, "n" * nBar,
                 "S" if noise[i] < squelch else ""))
    print()

    # ---- the numbers, over all reps
    print("final values: rssi %.1f (floor %.1f, lift %+.1f)   noise %.1f (floor %.1f, "
          "lift %+.1f)" % (finalRssi, baseRssi, finalRssi - baseRssi,
                           finalNoise, baseNoise, finalNoise - baseNoise))
    print()
    print("  ---- settle to within +/-tol of each curve's own final value ----")
    print("  %-6s %-22s %-22s" % ("tol", "rssi", "noise"))
    for tol in (1, 2, 3, 4):
        r = median([settleTime(t, rs, finalValue(rs), tol) for t, rs, _, _, _ in runs])
        n = median([settleTime(t, ns, finalValue(ns), tol) for t, _, ns, _, _ in runs])
        print("  +/-%-3d %-22s %-22s" % (tol, fmtUs(r), fmtUs(n)))

    print()
    print("  ---- time to travel X% of the way from the fA floor to the fB final ----")
    print("  (threshold-free, so the two curves are directly comparable)")
    print("  %-6s %-22s %-22s" % ("frac", "rssi", "noise"))
    for frac in (0.5, 0.9, 0.95):
        rLevel = baseRssi + frac * (finalRssi - baseRssi)
        nLevel = baseNoise + frac * (finalNoise - baseNoise)
        r = median([crossTime(t, rs, rLevel, rising=(finalRssi > baseRssi))
                    for t, rs, _, _, _ in runs])
        n = median([crossTime(t, ns, nLevel, rising=(finalNoise > baseNoise))
                    for t, _, ns, _, _ in runs])
        print("  %-6.0f%% %-22s %-22s" % (frac * 100, fmtUs(r), fmtUs(n)))

    print()
    if args.reg not in (0, 0x1B):
        # `noise < squelch` is a statement about 0x1B's low byte and nothing else. Run it
        # against another register and it compares the squelch threshold to whatever that
        # register's low byte happens to be -- for 0x1C that is a constant 0, which reads
        # as "detected 444 us after the retune" and is pure nonsense.
        print("  ---- detection section skipped: register 0x%02X is not 0x1B ----" % args.reg)
        print("  Those rules are defined on 0x1B's two bytes. Compare the rise times")
        print("  above against an 0x1B run instead.")
    else:
        print("  ---- time to DETECT, i.e. what a scanner would actually wait for ----")
        nDet = median([crossTime(t, ns, squelch - 1, rising=False)
                       for t, _, ns, _, _ in runs])
        print("  noise < %-3d (the real rule)      %s" % (squelch, fmtUs(nDet)))
        for margin in (3, 6, 10):
            rDet = median([crossTime(t, rs, baseRssi + margin, rising=True)
                           for t, rs, _, _, _ in runs])
            print("  rssi > floor + %-2d                 %s" % (margin, fmtUs(rDet)))

    if args.csv:
        with open(args.csv, "w") as fh:
            fh.write("rep,t_us,rssi,noise\n")
            for rep, (t, rs, ns, _, _) in enumerate(runs):
                for i in range(len(t)):
                    fh.write("%d,%d,%d,%d\n" % (rep, t[i], rs[i], ns[i]))
        print("\nwrote %s" % args.csv)


# --------------------------------------------------------------- mode: triggers

def doTriggers(ser, args):
    fA, fB = mhz(args.fa), mhz(args.fb)
    squelch, _, _ = readSquelch(ser)
    methods = args.methods.split(",") if args.methods else list(RETUNE)

    print("retune trigger comparison: %.5f -> %.5f MHz, %d reps each"
          % (args.fa, args.fb, args.reps))
    print("a carrier must be on fB and NOT on fA: separation is the whole measurement")
    print("squelch threshold %d\n" % squelch)

    results = {}
    for name in methods:
        if name not in RETUNE:
            sys.exit("unknown retune method %r" % name)
        mode = modeByte(name, fm=(not args.no_fm), wide=args.wide)
        finals, settles, retuneUs = [], [], []
        for _rep in range(args.reps):
            samples, tRetune, _ = probe(ser, fA, fB, mode, args.samples, args.interval)
            rssi = [s[1] for s in samples]
            ts = [s[0] for s in samples]
            f = finalValue(rssi)
            finals.append(f)
            settles.append(settleTime(ts, rssi, f, 2))
            retuneUs.append(tRetune)
        results[name] = (median(finals), median(settles), median(retuneUs))

    if CONTROL_NEG not in results:
        print("NOTE: %r was not run, so there is no negative control to measure "
              "separation against." % CONTROL_NEG)
    ref = results.get(CONTROL_NEG, (None,))[0]
    pos = results.get(CONTROL_POS, (None,))[0]

    print("  %-9s %8s %10s %12s %10s   %s"
          % ("method", "bus us", "final rssi", "sep vs fast", "settle+-2", "verdict"))
    for name in methods:
        finalRssi, settle, tRetune = results[name]
        sep = None if (ref is None) else (finalRssi - ref)
        verdict = "?"
        if (sep is not None) and (pos is not None) and (ref is not None):
            full = pos - ref
            if abs(full) < 5:
                verdict = "NO CONTRAST -- is the carrier on?"
            elif sep > (0.7 * full):
                verdict = "RETUNES"
            elif sep < (0.2 * full):
                verdict = "no-op"
            else:
                verdict = "partial -- look at the trace"
        print("  %-9s %8d %10.1f %12s %10s   %s"
              % (name, tRetune, finalRssi,
                 ("--" if sep is None else "%+.1f" % sep), fmtUs(settle), verdict))

    print()
    print("A method that RETUNES with a settle materially under %r's is the win; one that"
          % CONTROL_POS)
    print("retunes at the same settle is still worth having if its bus time is lower.")

    if args.both:
        print("\n--- reverse direction (park on the carrier, jump away) ---")
        print("A method that only 'works' one way is reading a coincidence, not a retune.")
        args.fa, args.fb = args.fb, args.fa
        args.both = False
        doTriggers(ser, args)


# ------------------------------------------------------------------- mode: regs

def doRegs(ser, args):
    if args.diff:
        a = json.load(open(args.diff[0]))
        b = json.load(open(args.diff[1]))
        print("diff %s -> %s" % (args.diff[0], args.diff[1]))
        changed = 0
        for key in sorted(set(a) | set(b), key=lambda k: int(k, 0)):
            va, vb = a.get(key), b.get(key)
            if va != vb:
                changed += 1
                print("  reg 0x%02X: %s -> %s  (xor %s)"
                      % (int(key, 0),
                         "----" if va is None else "%04X" % va,
                         "----" if vb is None else "%04X" % vb,
                         "----" if (va is None or vb is None) else "%04X" % (va ^ vb)))
        if changed == 0:
            print("  no register changed")
        else:
            print("\n  %d changed. 0x1B is rssi/noise and always moves; ignore it and look"
                  % changed)
            print("  for a register with ONE bit flipping -- that is a status flag.")
        return

    mode = modeByte("latch", fm=True)
    if args.anchor:
        sessionBegin(ser, mhz(args.anchor), mode)
        print("receiver parked on %.5f MHz for the snapshot" % args.anchor)

    regs = {}
    try:
        for reg in range(args.first, args.last + 1):
            if args.anchor and ((reg - args.first) % 16 == 0):
                sessionPoke(ser, mhz(args.anchor), mode)
            value = readReg(ser, reg)
            if value is not None:
                regs["0x%02X" % reg] = value
    finally:
        if args.anchor:
            sessionEnd(ser)

    print("read %d registers 0x%02X..0x%02X" % (len(regs), args.first, args.last))
    if args.save:
        with open(args.save, "w") as fh:
            json.dump(regs, fh, indent=1, sort_keys=True)
        print("wrote %s" % args.save)
    else:
        for key in sorted(regs, key=lambda k: int(k, 0)):
            print("  %s = %04X" % (key, regs[key]))


# --------------------------------------------------------------------- mode: sq

def doSq(ser, _args):
    squelch, rssi, noise = readSquelch(ser)
    print("squelch threshold : %d   (the scanner stops when noise < this)" % squelch)
    print("live rssi         : %d" % rssi)
    print("live noise        : %d   -> %s"
          % (noise, "OPEN (carrier)" if noise < squelch else "closed"))
    reg1b = readReg(ser, 0x1B)
    reg30 = readReg(ser, 0x30)
    if reg1b is not None:
        print("chip 0x1B         : %04X  (rssi %d, noise %d)"
              % (reg1b, reg1b >> 8, reg1b & 0xFF))
    if reg30 is not None:
        print("chip 0x30         : %04X  (rx_on %d, sq_on %d)"
              % (reg30, (reg30 >> 5) & 1, (reg30 >> 3) & 1))


# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="serial port (default: auto-detect 1FC9:0094)")
    sub = ap.add_subparsers(dest="cmd", required=True)

    def probeArgs(p):
        p.add_argument("--fa", type=float, required=True, help="park frequency, MHz")
        p.add_argument("--fb", type=float, required=True,
                       help="frequency to jump to, MHz -- put the carrier here")
        p.add_argument("--samples", type=int, default=MAX_SAMPLES)
        p.add_argument("--interval", type=int, default=0,
                       help="us between samples (0 = as fast as the I2C bus allows)")
        p.add_argument("--reps", type=int, default=5,
                       help="repeats; every number reported is the median")
        p.add_argument("--no-fm", action="store_true",
                       help="leave the radio in whatever mode it is in")
        p.add_argument("--wide", action="store_true", help="25 kHz IF instead of 12.5")

    p = sub.add_parser("split", help="RSSI vs noise settle, separately (lead 1)")
    probeArgs(p)
    p.add_argument("--retune", choices=sorted(RETUNE), default="latch")
    p.add_argument("--rows", type=int, default=60, help="trace lines to print")
    p.add_argument("--csv", help="also write every sample of every rep here")
    p.add_argument("--reg", type=lambda v: int(v, 0), default=0,
                   help="AT1846S register to time instead of 0x1B, e.g. a status "
                        "register carrying the chip's own squelch flag. The two columns "
                        "then hold that register's high and low bytes, and the "
                        "rssi/noise labels are just names for them")

    p = sub.add_parser("triggers", help="compare retune methods (lead 2)")
    probeArgs(p)
    p.set_defaults(reps=3)
    p.add_argument("--methods", help="comma-separated subset (default: all)")
    p.add_argument("--both", action="store_true",
                   help="also run fB -> fA, to catch a method that only appears to work")

    p = sub.add_parser("regs", help="snapshot / diff the AT1846S register file")
    p.add_argument("--first", type=lambda v: int(v, 0), default=0x00)
    p.add_argument("--last", type=lambda v: int(v, 0), default=0x7F)
    p.add_argument("--save", help="write the snapshot to this JSON file")
    p.add_argument("--diff", nargs=2, metavar=("A", "B"), help="compare two snapshots")
    p.add_argument("--anchor", type=float,
                   help="park the receiver on this MHz for the snapshot, so the carrier "
                        "is actually being received while the registers are read")

    sub.add_parser("sq", help="what threshold is the scanner really using?")

    args = ap.parse_args()

    if (args.cmd == "regs") and args.diff:
        doRegs(None, args)
        return

    ser = serial.Serial(findPort(args.port), 115200, timeout=2.0)
    try:
        {"split": doSplit, "triggers": doTriggers, "regs": doRegs, "sq": doSq}[args.cmd](ser, args)
    finally:
        ser.close()


if __name__ == "__main__":
    main()
