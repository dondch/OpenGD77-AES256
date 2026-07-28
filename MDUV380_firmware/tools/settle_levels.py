#!/usr/bin/env python3
"""Detection time and detection threshold, rule by rule, against a level-controlled carrier.

The split-settle run says RSSI is there ~3.5x sooner than `noise < squelch`. That is only
useful if it does not cost sensitivity, and the honest way to ask is to walk the carrier
down in exact 1 dB steps and record, at each level, how soon each rule would have fired
and whether it fired at all.

Runs from Windows; the radio is on a COM port and the HackRF lives in WSL.
"""
import json
import os
import subprocess
import sys
import time

sys.path.insert(0, r"\\wsl.localhost\Ubuntu-24.04\home\dondch\repo\MDUV380_firmware\tools")
import serial
import settle

FA, FB = settle.mhz(430.0), settle.mhz(433.5025)
LO_HZ = 433252500 - 250000 + 250000   # tone offset handled below
TONE_OFFSET = 250000
CARRIER_HZ = 433502500

REPS = 3
SAMPLES = 200


def wsl(cmd):
    return subprocess.run(["wsl", "-d", "Ubuntu-24.04", "bash", "-lc", cmd],
                          capture_output=True, text=True, timeout=120)


def txStart(txvga, iq="cw_250k.iq"):
    """Start the carrier and CONFIRM it started.

    hackrf_transfer occasionally fails to come up after a kill, and a silent failure
    reads as 'this level was too weak to detect' -- which is indistinguishable from the
    result being measured. A run that shows 16 dB undetected while 8 dB is strong is a
    dropout, not a sensitivity curve, so check rather than sleep and hope."""
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
    sys.exit("could not start the HackRF at txvga %d" % txvga)


def txStop():
    wsl("pkill -x hackrf_transfer")
    time.sleep(1.5)


def run(ser, mode, squelch):
    """Median detect times over REPS, plus the settled levels."""
    nDet, rDet3, rDet6, rDet10, finalR, finalN = [], [], [], [], [], []
    base, _, _ = settle.probe(ser, FA, FA, mode, SAMPLES, 0)
    floorR = settle.finalValue([s[1] for s in base])
    for _ in range(REPS):
        s, _, _ = settle.probe(ser, FA, FB, mode, SAMPLES, 0)
        ts = [x[0] for x in s]
        rs = [x[1] for x in s]
        ns = [x[2] for x in s]
        nDet.append(settle.crossTime(ts, ns, squelch - 1, rising=False))
        rDet3.append(settle.crossTime(ts, rs, floorR + 3, rising=True))
        rDet6.append(settle.crossTime(ts, rs, floorR + 6, rising=True))
        rDet10.append(settle.crossTime(ts, rs, floorR + 10, rising=True))
        finalR.append(settle.finalValue(rs))
        finalN.append(settle.finalValue(ns))
    return dict(floorR=floorR,
                finalR=settle.median(finalR), finalN=settle.median(finalN),
                noise=settle.median(nDet), r3=settle.median(rDet3),
                r6=settle.median(rDet6), r10=settle.median(rDet10))


def main():
    ser = serial.Serial(settle.findPort(), 115200, timeout=2.0)
    mode = settle.modeByte("radio", fm=True)
    squelch, _, _ = settle.readSquelch(ser)
    print("squelch threshold %d; fA %.4f (clear) -> fB %.4f (carrier)"
          % (squelch, FA / 1e5, FB / 1e5))
    print("detect time = first sample after which the rule stays satisfied, "
          "median of %d reps\n" % REPS)

    rows = []
    # Each argument is "txvga" or "iqfile:txvga". The attenuated IQ files reach below
    # what TXVGA 0 can, which is where the interesting comparison is.
    levels = sys.argv[1:] or ["20", "14", "8", "4", "0"]
    print("  %-14s %7s %7s %11s %11s %11s %11s"
          % ("level", "rssi", "noise", "noise<%d" % squelch,
             "rssi>fl+3", "rssi>fl+6", "rssi>fl+10"))

    for spec in levels:
        iq, _, txs = spec.rpartition(":")
        iq = iq or "cw_250k.iq"
        txvga = int(txs)
        txStart(txvga, iq)
        r = run(ser, mode, squelch)
        rows.append(dict(txvga=txvga, iq=iq, **r))
        print("  %-14s %7.1f %7.1f %11s %11s %11s %11s"
              % ("%s %ddB" % (iq.replace("cw_", "").replace(".iq", ""), txvga),
                 r["finalR"], r["finalN"],
                 settle.fmtUs(r["noise"]), settle.fmtUs(r["r3"]),
                 settle.fmtUs(r["r6"]), settle.fmtUs(r["r10"])))

    txStop()
    r = run(ser, mode, squelch)
    rows.append(dict(txvga=None, iq=None, **r))
    print("  %-14s %7.1f %7.1f %11s %11s %11s %11s"
          % ("no tx", r["finalR"], r["finalN"],
             settle.fmtUs(r["noise"]), settle.fmtUs(r["r3"]),
             settle.fmtUs(r["r6"]), settle.fmtUs(r["r10"])))
    print("\n  'no tx' must read `never` in every column -- any rule that fires with no")
    print("  carrier is a false alarm and its detection times above mean nothing.")

    out = os.path.join(os.environ["TEMP"], "settle_levels.json")
    json.dump(rows, open(out, "w"), indent=1)
    print("\nwrote %s" % out)
    ser.close()


main()
