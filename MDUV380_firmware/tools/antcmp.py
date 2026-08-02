#!/usr/bin/env python3
"""Compare the two antenna sweeps at the frequencies they share.

The question this answers: is the ~131 MHz knee the RADIO's front-end filter, or was part
of it antenna/coupling? The antenna changes one term in the chain and leaves the filter
alone, so:
  knee stays put  -> front-end filter, 131 MHz is the hard floor
  knee moves      -> some of the measured rolloff was coupling

⚠ `lift` alone can mislead. Where a spur (or broadcast breakthrough) raises the NOISE
FLOOR, lift under-reads sensitivity and looks like a dip. So the floor is printed for both
runs and any point whose floor moved a lot is flagged rather than compared silently.
"""
import re, sys

import numpy as np

ROW = re.compile(r"^\s*([\d.]+) MHz\s+floor\s+([\d.]+)\s+carrier\s+([\d.]+)\s+lift\s+([+-][\d.]+)")


def load(path):
    out = {}
    for line in open(path, encoding="utf-8", errors="replace"):
        m = ROW.match(line)
        if m:
            f, fl, ca, lf = (float(m.group(i)) for i in (1, 2, 3, 4))
            out[round(f, 2)] = (fl, ca, lf)
    return out


def main():
    old = load(sys.argv[1])
    new = load(sys.argv[2])
    common = sorted(set(old) & set(new))
    if not common:
        sys.exit("no shared frequencies")

    print("%-9s | %-22s | %-22s | %s" % ("", "ORIGINAL antenna", "NEW antenna", "delta"))
    print("%-9s | %6s %7s %6s | %6s %7s %6s | %6s %6s" %
          ("MHz", "floor", "carrier", "lift", "floor", "carrier", "lift", "lift", "floor"))
    for f in common:
        of, oc, ol = old[f]
        nf, nc, nl = new[f]
        flag = ""
        if abs(nf - of) >= 8:
            flag = "  <- FLOOR MOVED, lift not comparable"
        elif 118 <= f < 137:
            flag = "  <- airband"
        print("%-9.2f | %6.1f %7.1f %+6.1f | %6.1f %7.1f %+6.1f | %+6.1f %+6.1f%s"
              % (f, of, oc, ol, nf, nc, nl, nl - ol, nf - of, flag))

    def knee(d):
        """Lowest frequency at which lift is >= 10 dB and stays >= 8 dB for 2 more points."""
        fs = sorted(d)
        for i, f in enumerate(fs[:-2]):
            if d[f][2] >= 10 and d[fs[i + 1]][2] >= 8 and d[fs[i + 2]][2] >= 8:
                return f
        return None

    print()
    print("knee (first freq with lift >=10 dB, sustained):")
    print("  ORIGINAL antenna : %s" % knee(old))
    print("  NEW antenna      : %s" % knee(new))


if __name__ == "__main__":
    main()
