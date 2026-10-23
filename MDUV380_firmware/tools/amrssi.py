#!/usr/bin/env python3
"""Measure the AT1846S RSSI *tracking* bandwidth -- the number route 3 lives or dies on.

WHY THIS IS NOT THE MEASUREMENT ALREADY IN THE NOTES. The often-quoted "RSSI reaches 50%
of a step in 3.3 ms" is receiver-RESTART RECOVERY: AGC convergence and DSP re-init after
the 30H[5] rx_on edge, which NEXT_SESSION_AT1846S_SETTLE.md proved is 100% of the settle.
That says nothing about how fast rssi_db follows an amplitude change on a receiver that is
already running -- which is the only thing an AM envelope detector cares about.

So: park the receiver and DO NOT RETUNE IT. The probe (CPS 0xA1) always performs its
"step", so we hand it fA == fB with retune method `fast`, which is the project's proven
NO-OP (measured +0.0 counts of separation against a real retune). The receiver is set up
properly by the probe's radioSetFrequency(fA) park, settles for 30 ms, and then the only
thing that changes during the 200 samples is the transmitter's AM envelope.

METHOD. For each modulation tone fm, fit a sinusoid at exactly fm to the rssi(t) samples
by least squares, using the firmware's own DWT timestamps rather than assuming a uniform
rate (the reads are ~142 us but not perfectly even). The fitted amplitude, in RSSI counts
= dB, is the recovered envelope. Compare against:

  * the ideal      -- 20*log10(1+m) - 20*log10(1-m) peak-to-peak, i.e. 10.9 counts of
                      amplitude at m=0.85, if RSSI tracked the envelope perfectly;
  * the CONTROL    -- the same fit run against an UNMODULATED carrier (am0.iq). Whatever
                      that returns is the measurement's own noise floor. A "response"
                      at or below the control row means nothing was detected, and
                      without this row the rest of the table is not interpretable.

NOTHING HERE TRANSMITS UNTIL --go IS PASSED. Dry-run prints the plan and exits.
"""
import argparse, math, os, struct, subprocess, sys, time

import numpy as np
import serial

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import settle

CARRIER_HZ  = 433_502_500          # dummy load / attenuator. NEVER aviation spectrum.
TONE_OFFSET = 250_000              # must match genam.py OFFSET
DEPTH       = 0.85
TONES       = [100, 200, 300, 500, 700, 1000, 1500, 2000, 2500]


def wsl(cmd, timeout=120):
    return subprocess.run(["wsl", "-d", "Ubuntu-24.04", "bash", "-lc", cmd],
                          capture_output=True, text=True, timeout=timeout)


def txStart(iq, txvga):
    """Start the transmitter and CONFIRM it came up.

    hackrf_transfer silently fails to restart after a kill often enough to matter, and a
    dropped transmitter reads exactly like 'no response at this frequency' -- which is
    the result being measured. Check, don't sleep and hope."""
    txStop()
    for attempt in range(4):
        wsl("cd ~ && setsid nohup hackrf_transfer -t %s -f %d -s 2000000 -x %d -a 0 -R "
            "> /tmp/hrf.log 2>&1 < /dev/null &" % (iq, CARRIER_HZ - TONE_OFFSET, txvga))
        time.sleep(3.5)
        r = wsl("pgrep -x hackrf_transfer >/dev/null && grep -c dBfs /tmp/hrf.log")
        if r.stdout.strip().isdigit() and int(r.stdout.strip()) > 0:
            return
        print("    (transmitter did not come up, retry %d)" % (attempt + 1))
        txStop()
    sys.exit("could not start the HackRF with %s" % iq)


def txStop():
    wsl("pkill -x hackrf_transfer")     # -x: -f would match the shell issuing it
    time.sleep(1.5)


def fitTone(tUs, y, fm):
    """Least-squares amplitude of a sinusoid at fm, on non-uniform timestamps.

    Returns (amplitude in counts, residual RMS). Solving for a and b in
    y ~ c + a*cos(wt) + b*sin(wt) is exact for known w and copes with the uneven
    sample spacing that an FFT would smear."""
    t = np.asarray(tUs, dtype=np.float64) * 1e-6
    y = np.asarray(y, dtype=np.float64)
    w = 2.0 * math.pi * fm
    A = np.column_stack([np.ones_like(t), np.cos(w * t), np.sin(w * t)])
    coef, *_ = np.linalg.lstsq(A, y, rcond=None)
    resid = y - A @ coef
    return math.hypot(coef[1], coef[2]), float(np.sqrt((resid ** 2).mean()))


def measure(ser, fm, reps, nSamples):
    """Park without retuning and fit. Magnitude is averaged across reps, so the runs need
    no phase relationship to each other -- which they cannot have, since each probe call
    re-parks."""
    f = settle.mhz(CARRIER_HZ / 1e6)
    mode = settle.modeByte("fast", fm=True, wide=False)   # `fast` == proven no-op
    amps, resids, levels = [], [], []
    for _ in range(reps):
        samples, _retuneUs, readUs = settle.probe(ser, f, f, mode,
                                                  nSamples=nSamples, intervalUs=0)
        tUs = [s[0] for s in samples]
        rssi = [s[1] for s in samples]
        a, r = fitTone(tUs, rssi, fm)
        amps.append(a); resids.append(r); levels.append(float(np.mean(rssi)))
    span = (max(tUs) - min(tUs)) / 1e3
    rate = (len(tUs) - 1) / ((max(tUs) - min(tUs)) * 1e-6) if len(tUs) > 1 else 0
    return dict(amp=float(np.mean(amps)), ampSd=float(np.std(amps)),
                resid=float(np.mean(resids)), level=float(np.mean(levels)),
                spanMs=span, rateHz=rate, n=len(tUs), readUs=readUs)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--go", action="store_true",
                    help="actually transmit (into a dummy load). Without this: dry run.")
    ap.add_argument("--txvga", type=int, default=20)
    ap.add_argument("--reps", type=int, default=6)
    ap.add_argument("--samples", type=int, default=settle.MAX_SAMPLES)
    ap.add_argument("--port")
    ap.add_argument("--csv")
    args = ap.parse_args()

    ideal = 0.5 * (20 * math.log10(1 + DEPTH) - 20 * math.log10(1 - DEPTH))
    print("carrier      : %.4f MHz  (LO %.4f + %d kHz tone offset)"
          % (CARRIER_HZ / 1e6, (CARRIER_HZ - TONE_OFFSET) / 1e6, TONE_OFFSET // 1000))
    print("modulation   : m = %.2f  -> ideal recovered amplitude %.2f counts (dB)"
          % (DEPTH, ideal))
    print("probe        : fA == fB, retune `fast` (PROVEN no-op) -- receiver never restarts")
    print("reps         : %d x %d samples\n" % (args.reps, args.samples))

    if not args.go:
        print("DRY RUN -- nothing transmitted. Re-run with --go when the bench is ready.")
        print("Files expected in WSL ~ : " +
              ", ".join("am%d.iq" % t for t in TONES) + ", am0.iq (control)")
        return

    ser = serial.Serial(settle.findPort(args.port), 115200, timeout=2)
    rows = []
    try:
        # CONTROL FIRST. If the unmodulated carrier already "responds", the rig is wrong
        # and no later row can be believed.
        print("  %-8s %8s %8s %8s %9s %8s" %
              ("tone", "amp", "+-sd", "resid", "level", "vs ideal"))
        txStart("am0.iq", args.txvga)
        ctl = {}
        for fm in TONES:
            ctl[fm] = measure(ser, fm, args.reps, args.samples)["amp"]
        print("  %-8s %8.3f %8s %8s %9s %8s   <-- CONTROL, unmodulated" %
              ("(none)", float(np.mean(list(ctl.values()))), "", "", "", ""))

        for fm in TONES:
            txStart("am%d.iq" % fm, args.txvga)
            r = measure(ser, fm, args.reps, args.samples)
            r["fm"] = fm; r["control"] = ctl[fm]
            rows.append(r)
            print("  %-8s %8.3f %8.3f %8.2f %9.1f %7.1f dB" %
                  ("%d Hz" % fm, r["amp"], r["ampSd"], r["resid"], r["level"],
                   20 * math.log10(max(r["amp"], 1e-6) / ideal)))
    finally:
        txStop()
        ser.close()

    if rows:
        print("\nsample rate  : %.0f Hz over %.1f ms (%d samples, %d us/read)"
              % (rows[0]["rateHz"], rows[0]["spanMs"], rows[0]["n"], rows[0]["readUs"]))
    if args.csv and rows:
        import csv
        with open(args.csv, "w", newline="") as fh:
            w = csv.DictWriter(fh, fieldnames=sorted(rows[0]))
            w.writeheader(); w.writerows(rows)
        print("wrote %s" % args.csv)


if __name__ == "__main__":
    main()
