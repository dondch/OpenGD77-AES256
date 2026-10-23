#!/usr/bin/env python3
"""Compare N antenna sweeps by SHAPE, not by absolute lift.

Absolute lift is not comparable across antennas -- they couple differently, and ANT3 also
needed a different drive level (txvga 47 vs 20) to get usable dynamic range. Normalising
each run to its OWN in-band reference (>=148 MHz) cancels both, leaving the frequency
response shape, which is the only thing the front-end filter question depends on.

  knee same across antennas -> the radio's front-end filter, nothing can move it
  knee moves                -> part of the rolloff was antenna/coupling

⚠ Where a spur or broadcast breakthrough raises the NOISE FLOOR, lift under-reads
sensitivity and mimics a dip. Points with an unusually high floor are marked, not compared.
"""
import re, sys

import numpy as np

ROW = re.compile(r"^\s*([\d.]+) MHz\s+floor\s+([\d.]+)\s+carrier\s+([\d.]+)\s+lift\s+([+-][\d.]+)")


def load(path):
    d = {}
    for line in open(path, encoding="utf-8", errors="replace"):
        m = ROW.match(line)
        if m:
            d[round(float(m.group(1)), 2)] = (float(m.group(2)), float(m.group(4)))
    return d


def knee(d, ref):
    """Lowest frequency whose normalised response is within 14 dB of in-band and stays
    there for two more points. 14 dB is well below the ~20-27 dB hole beneath the knee and
    well above the 7-11 dB shelf, so it separates the two regimes on every run so far."""
    fs = sorted(d)
    for i, f in enumerate(fs[:-2]):
        if all(d[fs[j]][1] - ref > -14 for j in (i, i + 1, i + 2)):
            return f
    return None


def main():
    runs = []
    for arg in sys.argv[1:]:
        label, _, path = arg.partition("=")
        d = load(path)
        inband = [v[1] for f, v in d.items() if f >= 148]
        runs.append((label, d, float(np.median(inband)) if inband else float("nan")))

    print("in-band reference (>=148 MHz):  " +
          "   ".join("%s %+.1f dB" % (l, r) for l, _, r in runs))
    print()
    hdr = "".join("%12s" % l for l, _, _ in runs)
    print(" %-9s%s   %s" % ("MHz", hdr, "note"))
    common = sorted(set.intersection(*[set(d) for _, d, _ in runs]))
    for f in common:
        cells = ""
        hot = False
        for _, d, ref in runs:
            fl, lf = d[f]
            cells += "%12.1f" % (lf - ref)
            if fl >= 70:
                hot = True
        note = "spur/breakthrough floor - ignore" if hot else ("airband" if 118 <= f < 137 else "")
        print(" %-9.2f%s   %s" % (f, cells, note))

    print()
    print("knee (first freq within 14 dB of in-band, sustained):")
    for l, d, r in runs:
        print("  %-12s %s MHz" % (l, knee(d, r)))


if __name__ == "__main__":
    main()
