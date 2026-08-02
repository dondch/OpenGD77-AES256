#!/usr/bin/env python3
"""Gate 2 part 2: sensitivity versus frequency across and below airband.

⚠ THIS TRANSMITS INTO AVIATION SPECTRUM (108-137 MHz). Run only inside the shielded
enclosure and only with an explicit go-ahead. Authorised 2026-07-29.

⚠⚠ WHAT THIS CANNOT SEPARATE. The measured lift is the product of FOUR frequency
responses: HackRF output level, coupling inside the box, the radio's antenna, and the
radio's front-end filter. Only the last one is the question. Nothing here calibrates the
other three, so an absolute sensitivity number would be fiction. What IS interpretable is
a sharp EDGE -- a front-end band-pass rolls off far faster than a HackRF output curve or a
box-coupling curve, so a distinct knee is attributable, a gentle slope is not.

Grid is offset by 0.30 MHz to avoid landing on the exact-MHz bench spurs, which would
inflate the TX-off floor and compress the measured lift.
"""
import subprocess, sys, time

import numpy as np
import serial
import settle

TXVGA = 20                 # 145.3 MHz: floor 55, +26 dB at this level, compressing by 40
STEP, F0, F1 = 1.0, 125.3, 160.3
MODE = settle.modeByte("latch", fm=True)


def wsl(c):
    return subprocess.run(["wsl", "-d", "Ubuntu-24.04", "bash", "-lc", c],
                          capture_output=True, text=True)


def txStop():
    wsl("pkill -x hackrf_transfer")      # -x: -f matches the issuing shell
    time.sleep(1.2)


def txStart(mhz, g):
    """Start and CONFIRM. A silent failure to restart reads exactly like 'no response at
    this frequency', which is the result being measured."""
    for _ in range(4):
        txStop()
        wsl("cd ~ && setsid nohup hackrf_transfer -t cw_250k.iq -f %d -s 2000000 -x %d "
            "-a 0 -R > /tmp/hrf.log 2>&1 < /dev/null &" % (int(mhz * 1e6) - 250000, g))
        time.sleep(3.2)
        r = wsl("pgrep -x hackrf_transfer >/dev/null && grep -c dBfs /tmp/hrf.log")
        if r.stdout.strip().isdigit() and int(r.stdout.strip()) > 0:
            return True
    return False


def read(ser, mhz):
    """Settled tail only. The first ~20 ms after a latch is the receiver-restart
    transient -- sampling it returns a flat dead-looking floor at every frequency."""
    s, _, _ = settle.probe(ser, settle.mhz(mhz), settle.mhz(mhz), MODE,
                           nSamples=200, intervalUs=0)
    tail = [x[1] for x in s if x[0] >= 21000]
    return float(np.median(tail))


def main():
    freqs = [round(F0 + i * STEP, 2) for i in range(int((F1 - F0) / STEP) + 1)]
    ser = serial.Serial(settle.findPort(None), 115200, timeout=15)
    try:
        txStop()
        floor = {f: read(ser, f) for f in freqs}          # pass 1: no transmitter
        print("pass 1 (TX off) done")
        rows = []
        for f in freqs:                                    # pass 2: carrier at each point
            ok = txStart(f, TXVGA)
            on = read(ser, f) if ok else float("nan")
            rows.append((f, floor[f], on, on - floor[f], ok))
            print("  %7.2f MHz  floor %5.1f  carrier %5.1f  lift %+6.1f dB%s"
                  % (f, floor[f], on, on - floor[f], "" if ok else "   TX FAILED"))
    finally:
        txStop()
        ser.close()

    a = np.array([[r[0], r[1], r[2], r[3]] for r in rows])
    np.savetxt("gate2sens_fine.csv", a, delimiter=",", header="mhz,floor,carrier,lift")
    sel = a[(a[:, 0] >= 148)]; ref = np.median(sel[:, 3]) if len(sel) else float("nan")
    print("\nin-band reference lift (150-175 MHz) = %.1f dB" % ref)
    print(" %-10s %8s %10s" % ("MHz", "lift dB", "vs in-band"))
    for f, fl, on, lf in a:
        tag = "  <-- AIRBAND" if 118 <= f < 137 else ""
        print(" %-10.2f %8.1f %10.1f%s" % (f, lf, lf - ref, tag))


if __name__ == "__main__":
    main()
