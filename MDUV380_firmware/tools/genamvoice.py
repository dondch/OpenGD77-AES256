#!/usr/bin/env python3
"""Build an AM-modulated IQ file from a real speech recording, for the listening test.

Same convention as genam.py: 2 Msps, int8 I/Q, carrier parked 250 kHz above the LO so LO
leakage lands outside the radio's IF passband.

The file is padded to a whole number of seconds with UNMODULATED carrier, so that
`hackrf_transfer -R` loops through a silent seam instead of a click -- a click is a
broadband amplitude step, i.e. exactly the thing being measured.

Generated in chunks: a 6 s file is 12M complex samples, which is 200+ MB if built in one
complex128 array.
"""
import numpy as np, sys, wave

FS_IQ  = 2_000_000
OFFSET = 250_000
DEPTH  = 0.85
PEAK   = 120.0
DUR    = 6.0            # whole seconds -> 250 kHz completes 1.5M cycles, loops cleanly


def loadMono(path, skipS=0.0):
    """Mono, DC-removed, and normalised so the speech actually MODULATES.

    ⚠ Peak-normalising is wrong for these recordings. dectx_voice_NOKEY.wav has
    rms/peak = 0.025 -- one transient at the start and then silence -- so dividing by the
    peak put the speech at ~2% depth and the captured envelope came back with a standard
    deviation of 0.08 RSSI counts, i.e. nothing. Normalise on a high percentile and clip
    the few samples above it instead."""
    w = wave.open(path)
    n, ch, sw, sr = w.getnframes(), w.getnchannels(), w.getsampwidth(), w.getframerate()
    raw = np.frombuffer(w.readframes(n), dtype=np.int16 if sw == 2 else np.int8)
    w.close()
    if ch > 1:
        raw = raw.reshape(-1, ch).mean(axis=1)
    a = raw.astype(np.float64)
    a -= a.mean()
    if skipS > 0:
        a = a[int(skipS * sr):]
    ref = np.percentile(np.abs(a), 99.0)
    if ref <= 0:
        ref = np.abs(a).max() or 1.0
    return np.clip(a / ref, -1.0, 1.0), sr


def main():
    src = sys.argv[1]
    out = sys.argv[2]
    skip = float(sys.argv[3]) if len(sys.argv) > 3 else 0.0
    speech, sr = loadMono(src, skip)
    nOut = int(FS_IQ * DUR)

    # Speech occupies the front of the file; the tail is unmodulated carrier.
    print("source %s: %.2f s @ %d Hz (skip %.1f s), rms %.3f of full scale -> %.1f s IQ (%.0f MB)"
          % (src, len(speech) / sr, sr, skip, speech.std(), DUR, nOut * 2 / 1e6))

    a = PEAK / (1.0 + DEPTH)
    chunk = 1_000_000
    with open(out, "wb") as fh:
        for start in range(0, nOut, chunk):
            n = min(chunk, nOut - start)
            idx = np.arange(start, start + n, dtype=np.float64)
            t = idx / FS_IQ
            # linear-interpolate the speech onto the IQ timebase; zero (silence) past its end
            s = np.interp(t * sr, np.arange(len(speech), dtype=np.float64), speech,
                          left=0.0, right=0.0)
            env = 1.0 + DEPTH * s
            ph = 2.0 * np.pi * OFFSET * t
            iq = np.empty(2 * n, dtype=np.int8)
            iq[0::2] = np.clip(np.round(a * env * np.cos(ph)), -127, 127)
            iq[1::2] = np.clip(np.round(a * env * np.sin(ph)), -127, 127)
            iq.tofile(fh)
    print("wrote %s" % out)


if __name__ == "__main__":
    main()
