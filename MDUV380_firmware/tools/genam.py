#!/usr/bin/env python3
"""Generate AM-modulated IQ files for hackrf_transfer, matching this bench's convention.

Same shape as the existing cw_*.iq: 2 Msps, 1.000 s, int8 I/Q interleaved, tone parked
250 kHz above the LO so that LO leakage and DC offset land far outside the radio's
12.5/25 kHz IF passband and cannot be mistaken for carrier.

    z(t) = A * (1 + m*sin(2*pi*fm*t)) * exp(j*2*pi*250000*t)

Both the 250 kHz offset and any integer fm complete a whole number of cycles in exactly
one second, so `hackrf_transfer -R` loops seamlessly -- a discontinuity at the loop point
would be a wideband click, i.e. an amplitude event, i.e. exactly the thing being measured.
"""
import numpy as np, os, sys

FS      = 2_000_000
DUR     = 1.0
OFFSET  = 250_000          # must match settle_levels.TONE_OFFSET
DEPTH   = 0.85             # typical airband modulation index
PEAK    = 120.0            # of 127, leaves headroom so (1+m) never clips

TONES = [100, 200, 300, 500, 700, 1000, 1500, 2000, 2500]

def gen(fm, m=DEPTH):
    n = int(FS * DUR)
    t = np.arange(n, dtype=np.float64) / FS
    env = 1.0 + m * np.sin(2 * np.pi * fm * t) if fm else np.ones(n)
    z = env * np.exp(2j * np.pi * OFFSET * t)
    a = PEAK / (1.0 + m if fm else 1.0)
    iq = np.empty(2 * n, dtype=np.int8)
    iq[0::2] = np.clip(np.round(a * z.real), -127, 127)
    iq[1::2] = np.clip(np.round(a * z.imag), -127, 127)
    return iq

if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    for fm in TONES:
        p = os.path.join(out, "am%d.iq" % fm)
        gen(fm).tofile(p)
        print("%-14s %d Hz tone, m=%.2f, %d bytes" % (os.path.basename(p), fm, DEPTH,
                                                      os.path.getsize(p)))
    # m=0 control: identical generator, no modulation. Without this row the rest of the
    # table has no floor to be compared against.
    p = os.path.join(out, "am0.iq")
    gen(0, 0.0).tofile(p)
    print("%-14s unmodulated CONTROL, %d bytes" % ("am0.iq", os.path.getsize(p)))
