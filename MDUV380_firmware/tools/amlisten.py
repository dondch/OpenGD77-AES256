#!/usr/bin/env python3
"""The listening test: record the RSSI envelope on the radio, bring it back, make a WAV.

Capture (CPS 0xB0) leaves 40960 RSSI bytes in the display framebuffer; they come back over
the CPS 'R' area 6 read that fbmirror.py already uses. Nothing must repaint the screen in
between, so the readout happens immediately after the capture returns.

Reconstruction:
  rssi counts ARE dB, so amplitude = 10**(rssi/20). Remove the carrier (the mean) and what
  is left is the modulation.

  --eq applies the inverse of the measured rssi_ct_u single pole (f-3dB = 1145/2**ct Hz):
  y[n] = x[n] + k*(x[n]-x[n-1]) with k = fs/(2*pi*fc). This flattens the response but does
  NOT improve SNR -- it lifts signal and quantisation noise together.
"""
import argparse, math, struct, subprocess, sys, time, wave

import numpy as np
import serial

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])
import settle

CARRIER_HZ, TONE_OFFSET = 433_502_500, 250_000
FB_BYTES, CHUNK, AREA_DISPLAY = 40960, 1024, 6


def wsl(cmd, timeout=180):
    return subprocess.run(["wsl", "-d", "Ubuntu-24.04", "bash", "-lc", cmd],
                          capture_output=True, text=True, timeout=timeout)


def txStart(iq, txvga):
    wsl("pkill -x hackrf_transfer"); time.sleep(1.5)
    for attempt in range(4):
        wsl("cd ~ && setsid nohup hackrf_transfer -t %s -f %d -s 2000000 -x %d -a 0 -R "
            "> /tmp/hrf.log 2>&1 < /dev/null &" % (iq, CARRIER_HZ - TONE_OFFSET, txvga))
        time.sleep(3.5)
        r = wsl("pgrep -x hackrf_transfer >/dev/null && grep -c dBfs /tmp/hrf.log")
        if r.stdout.strip().isdigit() and int(r.stdout.strip()) > 0:
            return
        print("    (transmitter did not come up, retry %d)" % (attempt + 1))
    sys.exit("could not start the HackRF")


def cpsScreen(ser, sub):
    """C,0 pushes the static UI_CPS screen; C,5 closes it.

    ★ This is what makes the capture survive. The samples live in the display
    framebuffer, and the normal VFO/channel UI repaints that buffer on its own schedule --
    the first attempt read back RGB565 pixel data (values 0..255) and gave byte-identical
    results for two different rssi_ct_u settings, which is what being handed the screen
    instead of the capture looks like. UI_CPS only draws when explicitly told to, so with
    it on top nothing overwrites the buffer between capture and readout."""
    ser.reset_input_buffer()
    ser.write(bytes([ord("C"), sub]) + bytes(6))
    ser.flush()
    time.sleep(0.3)
    ser.reset_input_buffer()


def capture(ser, nSamples, rssiCt):
    req = struct.pack(">BBIHB", ord("C"), 0xB0, settle.mhz(CARRIER_HZ / 1e6),
                      nSamples, rssiCt)
    ser.reset_input_buffer(); ser.write(req); ser.flush()
    r = settle.readExact(ser, 10)
    if len(r) < 10 or r[1] != 0xB0:
        sys.exit("bad capture reply: %s" % r.hex())
    count, rate = struct.unpack_from(">HH", r, 2)
    elapsedUs = struct.unpack_from(">I", r, 6)[0]
    return count, rate, elapsedUs


def readFb(ser, nBytes):
    out = bytearray(); addr = 0
    while addr < nBytes:
        n = min(CHUNK, nBytes - addr)
        ser.reset_input_buffer()
        ser.write(bytes([ord("R"), AREA_DISPLAY, (addr >> 24) & 0xFF, (addr >> 16) & 0xFF,
                         (addr >> 8) & 0xFF, addr & 0xFF, (n >> 8) & 0xFF, n & 0xFF]))
        ser.flush()
        r = settle.readExact(ser, n + 3)
        if len(r) < n + 3 or r[0] != ord("R"):
            raise IOError("bad framebuffer read @%d" % addr)
        out += r[3:3 + n]; addr += n
    return bytes(out)


def toWav(path, samples, fsIn, fsOut, ct, eq):
    x = 10.0 ** (np.asarray(samples, dtype=np.float64) / 20.0)   # dB -> linear envelope
    x -= x.mean()
    if eq:
        fc = 1145.0 / (2 ** ct)
        k = fsIn / (2.0 * math.pi * fc)
        x = np.concatenate([[x[0]], x[1:] + k * np.diff(x)])
    n = int(len(x) * fsOut / fsIn)
    x = np.interp(np.arange(n) / fsOut, np.arange(len(x)) / fsIn, x)
    peak = np.abs(x).max()
    pcm = (x / peak * 32000).astype("<i2") if peak else x.astype("<i2")
    w = wave.open(path, "wb")
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(int(fsOut))
    w.writeframes(pcm.tobytes()); w.close()
    return peak


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--go", action="store_true")
    ap.add_argument("--iq", default="amvoice.iq")
    ap.add_argument("--txvga", type=int, default=20)
    ap.add_argument("--samples", type=int, default=FB_BYTES)
    ap.add_argument("--ct", type=int, action="append",
                    help="rssi_ct_u values to capture (repeatable). Default 0 and 3.")
    ap.add_argument("--out", default="amcap")
    args = ap.parse_args()
    cts = args.ct if args.ct else [0, 3]

    if not args.go:
        print("DRY RUN. Would transmit %s at txvga %d and capture %d samples at ct=%s"
              % (args.iq, args.txvga, args.samples, cts))
        return

    ser = serial.Serial(settle.findPort(None), 115200, timeout=30)
    try:
        txStart(args.iq, args.txvga)
        cpsScreen(ser, 0)          # freeze the UI so it cannot repaint over the samples
        for ct in cts:
            count, rate, elapsedUs = capture(ser, args.samples, ct)
            fs = count / (elapsedUs / 1e6)
            raw = readFb(ser, count)
            arr = np.frombuffer(raw, dtype=np.uint8)
            fcHz = 1145.0 / (2 ** ct)
            print("ct=%d  %d samples @ %.1f Hz (%.2f s), rssi %d..%d mean %.1f sd %.2f"
                  " | RSSI filter f-3dB %.0f Hz"
                  % (ct, count, fs, elapsedUs / 1e6, arr.min(), arr.max(),
                     arr.mean(), arr.std(), fcHz))
            for eq in (False, True):
                p = "%s_ct%d%s.wav" % (args.out, ct, "_eq" if eq else "")
                toWav(p, arr, fs, 8000.0, ct, eq)
                print("   wrote %s" % p)
    finally:
        try:
            cpsScreen(ser, 5)      # close UI_CPS, give the radio its normal screen back
        except Exception:
            pass
        wsl("pkill -x hackrf_transfer")
        ser.close()


if __name__ == "__main__":
    main()
