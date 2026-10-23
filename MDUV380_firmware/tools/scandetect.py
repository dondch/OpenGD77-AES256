#!/usr/bin/env python3
"""Run a real VFO scan under a chosen detection rule and watch what it does.

The RSSI-vs-floor rule (CPS 0xAD mode 3) is only exercised by the live scanner --
trxCarrierDetected() is where it lives, and nothing calls that when the radio is idle.
So the estimator cannot be tested from a probe; it needs a scan actually running.

What to look for:
  * the floor converging from its seed towards the band noise, within a few steps;
  * with no carrier, the floor tracking and nothing detecting;
  * with a carrier, the scan stopping -- rssi pinned high, and the floor FROZEN, because
    a detected sample is deliberately excluded from the estimate. A floor that keeps
    climbing while parked on a carrier means the exclusion is broken and the radio is
    about to go deaf on the frequency it just found.

Injected keypresses are unreliable for seconds after a flash, so every step is verified
from firmware RAM rather than slept through. See sweepmode.py, same pattern.
"""
import struct
import sys
import time

sys.path.insert(0, "/home/dondch/repo/MDUV380_firmware/tools")
sys.path.insert(0, r"\\wsl.localhost\Ubuntu-24.04\home\dondch\repo\MDUV380_firmware\tools")
sys.path.insert(0, r"C:\Users\ddona\OneDrive\Desktop\HAM_Radio\TYT UV390Plus\chirp-opengd77-aes")

import serial
import fwsym
from fwsym import sym
import fbmirror
import settle

NORMAL, SCAN, DUAL_SCAN, SWEEP = 0, 1, 2, 3
KEY_RED = 27
FUNC_START_SCANNING = 0x8001
CMD_FUNC_INJECT = 0xA7
CMD_SCAN_DWELL = 0xA6
CMD_SETTLE_TICKS = 0xAA


def readmem(ser, addr, n):
    ser.reset_input_buffer()
    ser.write(struct.pack(">BBIH", ord("R"), 5, (addr - 0x08000000) & 0xFFFFFFFF, n))
    ser.flush()
    return ser.read(n + 3)[3:3 + n]


def key(ser, code, flags=0):
    ser.reset_input_buffer()
    ser.write(bytes([ord("C"), 0x96, code & 0xFF, flags]))
    ser.flush()
    ser.read(8)


def opMode(ser):
    return readmem(ser, sym("screenOperationMode"), 2)[0]


def inChannelMode(ser):
    """Channel mode draws "Ch:" where VFO draws the Rx/Tx frequency pair."""
    rgb = fbmirror.fb_to_rgb(fbmirror.read_fb(ser))
    ink = 0
    for y in range(88, 120):
        for x in range(0, 160):
            if rgb[(y * 160 + x) * 3] < 128:
                ink += 1
    return ink < 200


def setWord(ser, cmd, value):
    ser.reset_input_buffer()
    ser.write(struct.pack(">BBH", ord("C"), cmd, value))
    ser.flush()
    ser.read(4)


def injectFunc(ser, code):
    ser.reset_input_buffer()
    ser.write(struct.pack(">BBH", ord("C"), CMD_FUNC_INJECT, code))
    ser.flush()
    ser.read(8)


def intoVfo(ser):
    for attempt in range(8):
        if not inChannelMode(ser):
            return True
        print("  attempt %d: channel mode -> RED" % attempt)
        key(ser, KEY_RED)
        time.sleep(1.2)
    return False


def countStops(ser, seconds, interval):
    """Stops observed, and the rssi spread seen while actually scanning.

    The spread is the point: the rule compares one sample per step against an average of
    previous steps' samples, so the margin has to clear the step-to-step spread of that
    sample. Sampling early in the settle, where the curve is steep, makes that spread
    enormous -- which is a statement about WHEN to sample, not about the margin."""
    stops = 0
    wasScanning = True
    seen = []
    noiseSeen = []
    t0 = time.time()
    while (time.time() - t0) < seconds:
        _sq, rssi, noise, floor, _m, state, active = settle.readState(ser)
        scanning = active and (state == 0)
        if wasScanning and not scanning:
            stops += 1
        if scanning:
            seen.append(rssi)
            noiseSeen.append(noise)
        wasScanning = scanning
        time.sleep(interval)
    return stops, seen, floor, noiseSeen


def wsl(cmd):
    import subprocess
    return subprocess.run(["wsl", "-d", "Ubuntu-24.04", "bash", "-lc", cmd],
                          capture_output=True, text=True, timeout=120)


def txStop():
    wsl("pkill -x hackrf_transfer")
    time.sleep(1.2)


def txStart(txvga, iq, carrierHz=433502500):
    """Start the carrier and CONFIRM it started -- a silent failure to transmit reads
    exactly like 'too weak to detect', which is the thing being measured."""
    txStop()
    for _ in range(4):
        wsl("cd ~ && setsid nohup hackrf_transfer -t %s -f %d -s 2000000 -x %d -a 0 -R "
            "> /tmp/hrf.log 2>&1 < /dev/null &" % (iq, carrierHz - 250000, txvga))
        time.sleep(4.5)
        # Alive is the test. An earlier version also demanded a 'dBfs' throughput line
        # within 3 s and killed a perfectly healthy transmitter when the log had not been
        # flushed yet -- a check strict enough to cause the failure it was watching for.
        r = wsl("pgrep -x hackrf_transfer >/dev/null && echo UP")
        if "UP" in r.stdout:
            return
        txStop()
    sys.exit("could not start the HackRF at txvga %d" % txvga)


def stopsWithin(ser, seconds, interval=0.2):
    """Did the scan stop on something within `seconds`? True/False."""
    t0 = time.time()
    while (time.time() - t0) < seconds:
        _sq, _r, _n, _f, _m, state, active = settle.readState(ser)
        if active and (state != 0):
            return True
        if not active:
            return False        # scan ended (SCAN_MODE_STOP), which counts as a detection
        time.sleep(interval)
    return False


def thresholdRun(ser, args):
    """Lowest carrier level at which the scan still stops, for each reject setting.

    This is the number the whole fast-reject design turns on. The claim is that a wrong
    KEEP costs only time, so sensitivity should be identical to stock -- but that holds
    only if a real carrier is never REJECTED, and the settle curves say a weak one has
    not lifted at 3 ms. Measured rather than argued."""
    configs = [(0, 0)] + [tuple(int(x) for x in c.split(":"))
                          for c in args.sens.split(",")]
    # --tx-external: the transmitter is already running at one level and this run just
    # tests every config against it. Needed because a hackrf_transfer launched from a
    # Windows Python subprocess is torn down along with the transient WSL session that
    # spawned it, setsid and nohup notwithstanding, while one launched from a persistent
    # shell survives. Losing the carrier mid-sweep reads as "too weak to detect", which
    # is precisely the thing being measured.
    levels = [("external", 0)] if args.tx_external else (
        [("cw_250k.iq", v) for v in (20, 14, 8, 4, 0)] +
        [("cw_a35.iq", v) for v in (14, 8, 4, 0)])

    if args.dwell:
        setWord(ser, CMD_SCAN_DWELL, args.dwell)
    settle.setDetect(ser, settle.DETECT_NAMES["stock"])
    print("  dwell %d ms, stock detection rule, scan range must contain the carrier\n"
          % args.dwell)
    print("  %-22s %s" % ("reject", "".join("%12s" % ("%s%+d" % (iq[3:6], v))
                                            for iq, v in levels)))

    for ticks, margin in configs:
        cells = []
        for iq, txvga in levels:
            if not args.tx_external:
                txStart(txvga, iq)
            if not intoVfo(ser):
                sys.exit("lost VFO mode")
            settle.setReject(ser, ticks=ticks, margin=margin)
            injectFunc(ser, FUNC_START_SCANNING)
            time.sleep(0.8)
            cells.append("STOP" if stopsWithin(ser, args.seconds) else "-")
            key(ser, KEY_RED)
            time.sleep(0.4)
        label = "off (stock)" if ticks == 0 else "%d ticks / margin %d" % (ticks, margin)
        print("  %-22s %s" % (label, "".join("%12s" % c for c in cells)))

    if not args.tx_external:
        txStop()
    settle.setReject(ser, ticks=0)
    if args.dwell:
        setWord(ser, CMD_SCAN_DWELL, 0)
    print("\n  A reject row that loses a STOP the stock row has is costing sensitivity.")
    print("  transmitter stopped, overrides returned to stock")


def rejectRun(ser, args):
    """Steps per second with and without the fast reject, on the same scan range.

    Detection stays on the stock rule throughout, so any difference is speed and not
    sensitivity -- that is the property the whole design is chosen for, and running both
    arms back to back on one range is what makes the comparison mean anything."""
    ticks, _, margin = args.reject.partition(":")
    ticks, margin = int(ticks), int(margin or 0)

    if args.dwell:
        setWord(ser, CMD_SCAN_DWELL, args.dwell)
    settle.setDetect(ser, settle.DETECT_NAMES["stock"])

    print("  dwell %d ms, stock detection rule, %.0f s per arm\n" % (args.dwell, args.seconds))
    print("  %-22s %10s %10s %10s %9s" % ("", "steps", "rejected", "steps/s", "ms/step"))

    results = {}
    for label, t in (("stock (reject off)", 0), ("reject %d ticks/m%d" % (ticks, margin), ticks)):
        if not intoVfo(ser):
            sys.exit("lost VFO mode")
        settle.setReject(ser, ticks=t, margin=margin)
        injectFunc(ser, FUNC_START_SCANNING)
        time.sleep(1.0)
        settle.setReject(ser, ticks=t, margin=margin)   # zero the counters after start-up
        t0 = time.time()
        time.sleep(args.seconds)
        _tk, _m, total, rejected = settle.setReject(ser)
        elapsed = time.time() - t0
        key(ser, KEY_RED)
        time.sleep(0.5)
        rate = total / elapsed if elapsed else 0
        results[t] = rate
        print("  %-22s %10d %10d %10.1f %9.2f"
              % (label, total, rejected, rate, (1000.0 / rate) if rate else 0))

    if results.get(0) and results.get(ticks):
        print("\n  speed-up %.2fx" % (results[ticks] / results[0]))

    settle.setReject(ser, ticks=0)
    if args.dwell:
        setWord(ser, CMD_SCAN_DWELL, 0)
    print("  reject disabled, overrides returned to stock")


def paramSweep(ser, args):
    ticksList, _, marginList = args.sweep.partition(":")
    ticksList = [int(v) for v in ticksList.split(",")]
    marginList = [int(v) for v in marginList.split(",")]

    if args.dwell:
        setWord(ser, CMD_SCAN_DWELL, args.dwell)
    print("  dwell %d ms, %.0f s per cell -- run this with NO carrier\n"
          % (args.dwell, args.seconds))
    print("  cell = falseStops/rssiSpread")
    print("  %-8s %s" % ("ticks", "".join("%12s" % ("margin %d" % m) for m in marginList)))

    for ticks in ticksList:
        cells = []
        for margin in marginList:
            # ★ Re-verify VFO mode EVERY cell. Each cell ends with RED to stop the scan,
            # and a RED from a VFO screen that is no longer scanning switches to CHANNEL
            # mode -- where this radio's channels are blank, currentChannelData points at
            # one of them, and the squelch threshold reads garbage (234 rather than 43).
            # The sweep then quietly measures a channel scan over empty channels and
            # returns a full table of plausible numbers about nothing. This is a
            # documented trap in the scanner-speed handoff and it caught this tool anyway.
            if not intoVfo(ser):
                sys.exit("lost VFO mode partway through the sweep")
            setWord(ser, CMD_SETTLE_TICKS, ticks)
            settle.setDetect(ser, settle.DETECT_NAMES["auto"],
                             margin=margin, shift=args.shift or 3)
            injectFunc(ser, FUNC_START_SCANNING)
            time.sleep(1.2)
            squelch = settle.readState(ser)[0]
            stops, seen, floor, _noise = countStops(ser, args.seconds, args.interval)
            key(ser, KEY_RED)
            time.sleep(0.5)
            spread = (max(seen) - min(seen)) if len(seen) > 1 else 0
            # A cell measured against the wrong squelch is a cell about nothing. 43 is
            # what this radio's VFO uses; anything else means the mode check above did
            # not hold and the number should not be believed.
            cells.append("%d/%d%s" % (stops, spread, "" if squelch < 100 else " !sq%d" % squelch))
        print("  %-8d %s" % (ticks, "".join("%12s" % c for c in cells)))

    setWord(ser, CMD_SETTLE_TICKS, 0)
    if args.dwell:
        setWord(ser, CMD_SCAN_DWELL, 0)
    settle.setDetect(ser, settle.DETECT_NAMES["stock"])
    print("\n  cell = false stops (rssi spread while scanning). Zero is the only")
    print("  acceptable value with no carrier. Overrides returned to stock.")


def countsSensRun(ser, args):
    """Does a shorter noise average ever MISS a carrier the stock setting catches?

    This is the gate. The speed-up is worthless if it costs a detection, and this project
    has already built and thrown away an RSSI detector for exactly that. Same protocol as
    the run that validated the fast reject: one carrier, one scan range containing it, the
    stock detection rule in every arm, and the arms differing only in 0x5A.

    Unlike the reject -- whose sensitivity is identical to stock BY CONSTRUCTION, because
    it never touches the arbiter -- noise_ct_u changes the arbiter's own input. So there
    is a real mechanism for a miss here and it has to be measured, not argued: a shorter
    average settles to a lower value but jitters more, and a weak carrier that only just
    drags the noise byte under the threshold could be resolved differently.

    ★ Trials, not one shot. At the marginal level detection is a coin flip and a single
    trial per arm produces a table of noise that reads like a result.

    ★★ And the trials are INTERLEAVED -- one round visits every arm once, then repeats --
    rather than run as a block per arm. The marginal level does not hold still: measured
    here, stock scored 2/4 and then 8/8 at the same TXVGA minutes apart, with the
    transmitter verified up at a constant -11.2 dBfs throughout, so the drift is in the RF
    path and not in the source. Run in blocks, a slow drift lands entirely on whichever
    arm happened to be running and comes out as a sensitivity difference between
    registers. Interleaved, every arm sees the same drift and the comparison survives it.
    This is the only reason the table below can be trusted at a level chosen precisely
    because it is unstable.

    The carrier must already be running: a hackrf_transfer started from a Windows Python
    subprocess dies with the transient WSL session that spawned it, and a carrier that
    quietly dropped reads exactly like "too weak to detect"."""
    if args.dwell:
        setWord(ser, CMD_SCAN_DWELL, args.dwell)
    settle.setDetect(ser, settle.DETECT_NAMES["stock"])

    ticks, _, margin = (args.reject or "0:0").partition(":")
    ticks, margin = int(ticks), int(margin or 0)

    print("  dwell %d ms, stock detection rule, reject %s, %d trial(s) per arm, "
          "%.1f s each" % (args.dwell or 30, "off" if ticks == 0
                           else "%d ticks/m%d" % (ticks, margin),
                           args.trials, args.seconds))
    print("  the carrier must be up and inside the scan range NOW\n")
    cts = [int(c) for c in args.counts.split(",")]
    # ★ Dwell is part of the interleave, not an outer loop, for the same drift reason.
    # It also turns this into the measurement that actually matters: over EMPTY spectrum
    # noise_ct_u changes the step rate by nothing at all (measured: 27.7 steps/s in every
    # row), because an empty step runs the full dwell whatever the arbiter is doing. The
    # register buys speed only by making a SHORTER DWELL viable -- stock's rule needs
    # ~8.9 ms to fire and a 6 ms dwell gives it a ~5 ms window, which is the documented
    # sub-4 ms cliff seen from the other side. So the end-to-end win is a dwell floor,
    # and this table is where it gets measured.
    dwells = [int(d) for d in (args.dwells or str(args.dwell or 30)).split(",")]
    stock = settle.decodeCounts(settle.STOCK_DETECT_COUNTS)["noise"]
    cells = [(d, ct) for d in dwells for ct in cts]
    hits = {c: 0 for c in cells}
    perRound = {c: [] for c in cells}

    try:
        for rnd in range(args.trials):
            for cell in cells:
                dwell, ct = cell
                setWord(ser, CMD_SCAN_DWELL, dwell)
                want = settle.setCounts(ser, noise=ct)
                if not intoVfo(ser):
                    sys.exit("lost VFO mode partway through")
                settle.setReject(ser, ticks=ticks, margin=margin)
                injectFunc(ser, FUNC_START_SCANNING)
                time.sleep(0.8)
                got = stopsWithin(ser, args.seconds)
                hits[cell] += 1 if got else 0
                perRound[cell].append("Y" if got else ".")
                key(ser, KEY_RED)
                time.sleep(0.4)
                settle.assertCounts(ser, want)
            print("  round %d/%d: %s"
                  % (rnd + 1, args.trials,
                     "  ".join("%dms/ct%d %s" % (d, ct, perRound[(d, ct)][-1])
                               for (d, ct) in cells)))

        print("\n  %-8s %-10s %8s %10s %8s   %s"
              % ("dwell", "noise_ct", "detects", "of trials", "0x5A", "per round"))
        for (d, ct) in cells:
            print("  %-8s %-10s %8d %10d %8s   %s"
                  % ("%d ms" % d, "%d%s" % (ct, " *" if ct == stock else ""),
                     hits[(d, ct)], args.trials,
                     "%04X" % settle.buildCounts(noise=ct),
                     "".join(perRound[(d, ct)])))
    finally:
        settle.restoreCounts(ser)
        settle.setReject(ser, ticks=0)
        if args.dwell:
            setWord(ser, CMD_SCAN_DWELL, 0)
        print("\n  0x5A restored to %s, reject off, dwell back to stock"
              % settle.formatCounts(settle.readReg(ser, settle.REG_DETECT_COUNTS)))
    print("  Any row that detects less often than the stock row is costing sensitivity,")
    print("  which is disqualifying however much speed it buys.")


def countsRun(ser, args):
    """False stops with no carrier, against 0x5A's averaging length.

    `noise_ct_u` shortens the average behind `trxRxNoise`, which is the byte the stock
    stop rule decides on -- measured 2.9x faster to fire. The vendor's stated price is
    that the reading jitters, and the scanner re-evaluates the rule on EVERY tick from
    the end of the settling interval to the end of the dwell, so one 30 ms step gets ~28
    independent chances to dip below the threshold. A single dip is a false stop. This
    counts them on a real scan, which is the only place the live rule runs.

    ★ The denominator is reported, and that is not decoration. The precedent this test
    exists because of is the ticks=5 row in the settle work, which scored a clean zero
    because the test window `timeout < dwellTime - SETTLING` was EMPTY and the scanner
    never evaluated at all. Zero stops out of zero steps is not a pass. `steps` comes
    from the firmware's own step counter, which counts whether or not the reject is on.

    Detection stays on the stock rule and the reject stays off unless asked for, so the
    only thing varying between rows is the register."""
    if args.dwell:
        setWord(ser, CMD_SCAN_DWELL, args.dwell)
    settle.setDetect(ser, settle.DETECT_NAMES["stock"])

    ticks, _, margin = (args.reject or "0:0").partition(":")
    ticks, margin = int(ticks), int(margin or 0)

    print("  dwell %d ms, stock detection rule, reject %s, %.0f s per row"
          % (args.dwell or 30, "off" if ticks == 0 else "%d ticks/m%d" % (ticks, margin),
             args.seconds))
    print("  RUN THIS WITH NO CARRIER -- every row should read 0 false stops\n")
    # ★ The step counter is a MORE sensitive false-stop detector than polling the scan
    # state, and it is what this table is really built on. A false detection does not end
    # the scan: it sets SHORT_PAUSED for SCAN_SHORT_PAUSE_TIME (500 ms) and resumes when
    # the audio amp never opens. So it costs ~500 ms of not stepping -- about 14 steps at
    # a 30 ms dwell -- and if the host happens not to poll during that window the stop is
    # invisible while the missing steps are not. Reported as `lost`, in steps, against
    # the best row: independent of the poll rate entirely.
    print("  %-10s %8s %8s %9s %8s %7s %8s %7s"
          % ("noise_ct", "stops", "steps", "steps/s", "ms/step", "lost", "min", "0x5A"))

    stock = settle.decodeCounts(settle.STOCK_DETECT_COUNTS)["noise"]
    rows = []
    try:
        for ct in [int(c) for c in args.counts.split(",")]:
            # Re-verify VFO mode every row: each row ends with RED, and a RED from a
            # VFO screen that is no longer scanning switches to CHANNEL mode, where this
            # radio's blank channels give a garbage squelch threshold and the rest of the
            # table is about nothing.
            if not intoVfo(ser):
                sys.exit("lost VFO mode partway through the sweep")
            want = settle.setCounts(ser, noise=ct)

            injectFunc(ser, FUNC_START_SCANNING)
            time.sleep(1.0)
            squelch = settle.readState(ser)[0]
            settle.setReject(ser, ticks=ticks, margin=margin)   # also zeroes the counters
            t0 = time.time()
            stops, _seen, _floor, noiseSeen = countStops(ser, args.seconds, args.interval)
            elapsed = time.time() - t0
            _t, _m, steps, _rej = settle.setReject(ser)
            key(ser, KEY_RED)
            time.sleep(0.5)

            # The register is only rewritten by AT1846sInit(), so a row that lost its
            # setting would look exactly like a row that passed.
            settle.assertCounts(ser, want)

            rate = steps / elapsed if elapsed else 0
            rows.append((ct, stops, steps, rate,
                         min(noiseSeen) if noiseSeen else None, want, squelch))
            if steps == 0:
                print("       ^ ZERO STEPS -- the scanner never evaluated. This row is "
                      "degenerate,\n         not clean; do not read its zero as a pass.")

        best = max(r[3] for r in rows) if rows else 0
        for ct, stops, steps, rate, minNoise, want, squelch in rows:
            lost = (best - rate) * args.seconds
            print("  %-10s %8s %8d %9.1f %8.2f %7s %8s %7s%s"
                  % ("%d%s" % (ct, " *" if ct == stock else ""),
                     stops, steps, rate, (1000.0 / rate) if rate else 0,
                     "%.0f" % lost if lost >= 1 else "-",
                     minNoise if minNoise is not None else "-", "%04X" % want,
                     "" if squelch < 100 else "  !sq%d" % squelch))
    finally:
        settle.restoreCounts(ser)
        settle.setReject(ser, ticks=0)
        if args.dwell:
            setWord(ser, CMD_SCAN_DWELL, 0)
        print("\n  0x5A restored to %s, reject off, dwell back to stock"
              % settle.formatCounts(settle.readReg(ser, settle.REG_DETECT_COUNTS)))

    print("  * = stock. 'lost' is steps not taken relative to the fastest row: with no")
    print("  carrier every row should step at the same rate, so a deficit is time the")
    print("  scanner spent parked on nothing. ~14 steps = one 500 ms false pause.")
    print("  'min' is the lowest noise the host happened to catch while scanning -- a")
    print("  coarse sample of a rule evaluated ~1000x faster than it is polled, so it is")
    print("  a hint about headroom, not the minimum.")


def main():
    import argparse
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=sorted(settle.DETECT_NAMES))
    ap.add_argument("--margin", type=int, default=0)
    ap.add_argument("--shift", type=int, default=0)
    ap.add_argument("--threshold", type=int, default=0)
    ap.add_argument("--dwell", type=int, default=0, help="analog scan dwell, ms (0 = stock)")
    ap.add_argument("--settle-ticks", type=int, default=0,
                    help="main-loop ticks after the retune before testing (0 = stock 1)")
    ap.add_argument("--seconds", type=float, default=12.0)
    ap.add_argument("--interval", type=float, default=0.3)
    ap.add_argument("--tx-external", action="store_true",
                    help="the carrier is already running and this run must not manage it")
    ap.add_argument("--sens", metavar="TICKS:MARGIN,...",
                    help="walk the carrier down and report the lowest level at which the "
                         "scan still stops, for stock and for each reject setting. This "
                         "is the sensitivity check. TRANSMITS.")
    ap.add_argument("--reject", metavar="TICKS:MARGIN",
                    help="enable the fast reject and report the step rate and the "
                         "fraction of steps thrown away early, e.g. 3:8 . Detection is "
                         "left on the stock rule, so sensitivity is unchanged by "
                         "construction; what this measures is the speed.")
    ap.add_argument("--counts", metavar="NOISE_CT,...",
                    help="sweep 0x5A's noise_ct_u over a real scan and count the false "
                         "stops at each setting, e.g. 3,2,1,0 . Run it with NO carrier. "
                         "Shortening that average makes the stop rule fire ~2.9x sooner; "
                         "the vendor says the price is jitter, and a rule re-evaluated "
                         "every tick for a whole dwell turns jitter into false stops.")
    ap.add_argument("--counts-sens", action="store_true",
                    help="with --counts and a carrier already running: how often each "
                         "noise_ct_u setting detects it. The sensitivity gate. Use "
                         "--trials 4 at a marginal level.")
    ap.add_argument("--trials", type=int, default=4)
    ap.add_argument("--dwells", metavar="MS,...",
                    help="with --counts-sens: interleave over these dwells as well as "
                         "over noise_ct_u. This is what finds the dwell floor, which is "
                         "the only place the register buys end-to-end speed.")
    ap.add_argument("--sweep", metavar="TICKS:MARGINS",
                    help="characterise instead of watching: comma-separated settle ticks, "
                         "a colon, comma-separated margins. e.g. 1,2,3,4:6,12,18 . Counts "
                         "stops per cell. Run it with NO carrier and every cell should be "
                         "zero -- a cell that is not is the false-alarm rate at those "
                         "settings, which is the number that decides whether the rule is "
                         "usable at all.")
    args = ap.parse_args()

    with serial.Serial(fbmirror.find_port(), 115200, timeout=2.0) as ser:
        # Never resolve a symbol without checking the .elf is the firmware on the radio:
        # a stale one returns plausible garbage instead of failing.
        fwsym.assertMatchesRadio(lambda a, n: readmem(ser, a, n))

        if not intoVfo(ser):
            sys.exit("could not get the radio into VFO mode")
        print("  in VFO mode")

        if args.counts:
            return (countsSensRun if args.counts_sens else countsRun)(ser, args)
        if args.sweep:
            return paramSweep(ser, args)
        if args.sens:
            return thresholdRun(ser, args)
        if args.reject:
            return rejectRun(ser, args)

        if args.dwell:
            setWord(ser, CMD_SCAN_DWELL, args.dwell)
            print("  analog scan dwell forced to %d ms" % args.dwell)
        if args.settle_ticks:
            setWord(ser, CMD_SETTLE_TICKS, args.settle_ticks)
            print("  settling interval forced to %d ticks" % args.settle_ticks)

        got = settle.setDetect(ser, settle.DETECT_NAMES[args.mode],
                               threshold=args.threshold, margin=args.margin,
                               shift=args.shift)
        print("  detection rule: %s (margin %d, shift %d)"
              % (args.mode, got["margin"], got["shift"]))

        injectFunc(ser, FUNC_START_SCANNING)
        time.sleep(1.0)
        print("  screenOperationMode=%d (1 = SCAN)\n" % opMode(ser))

        # ★ Report the radio's own scan state, never a rule recomputed here. A host poll
        # sees a different sample than the one trxCarrierDetected() decided on, so a
        # host-side "would this have detected?" column is a guess wearing the costume of
        # a measurement. Scan.state is the radio saying what it actually did.
        print("  %8s %6s %6s %6s  %-13s %s"
              % ("t", "rssi", "noise", "floor", "scan", "note"))
        t0 = time.time()
        stops = 0
        wasScanning = True
        while (time.time() - t0) < args.seconds:
            _sq, rssi, noise, floor, _m, state, active = settle.readState(ser)
            scanning = active and (state == 0)
            if wasScanning and not scanning:
                stops += 1
            wasScanning = scanning
            note = ""
            if active and (state != 0):
                note = "STOPPED on a signal"
            print("  %7.1fs %6d %6d %6d  %-13s %s"
                  % (time.time() - t0, rssi, noise, floor,
                     settle.SCAN_STATE_NAMES.get(state, "?") if active else "idle", note))
            time.sleep(args.interval)

        print("\n  %d stop(s) in %.0f s" % (stops, args.seconds))

        key(ser, KEY_RED)
        time.sleep(0.6)
        settle.setDetect(ser, settle.DETECT_NAMES["stock"])
        if args.dwell:
            setWord(ser, CMD_SCAN_DWELL, 0)
        if args.settle_ticks:
            setWord(ser, CMD_SETTLE_TICKS, 0)
        print("\n  stopped; detection rule and overrides returned to stock")


if __name__ == "__main__":
    main()
