#!/usr/bin/env python3
"""Read the AES RX diagnostics from an OpenGD77-AES radio over USB (CPS cmd 0x84).

Use this to debug on-air encrypted RX without guess-and-flash: it shows whether the
firmware is parsing PI headers (and what MI), and whether the per-burst / per-codec-frame
decrypt hooks are firing. Requires an ENABLE_AES firmware build with the 0x84 diag command.

  python3 aes_diag.py [--port COM4] [--watch]

The radio enumerates as USB CDC VID:PID 1fc9:0094. Run it, then key up the stock TYT on the
encrypted channel and re-read (or use --watch) to see the counters move.
"""
import argparse, struct, sys, time
try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial required: pip install -r requirements.txt")

AES_VID, AES_PID = 0x1FC9, 0x0094
CMD = ord('C')
SUB_SHOW_CPS = 0
SUB_CLOSE    = 5
SUB_DIAG     = 0x84

def find_port():
    for p in list_ports.comports():
        if (p.vid == AES_VID) and (p.pid == AES_PID):
            return p.device
    return None

def send(ser, payload, read=64, wait=0.1):
    ser.reset_input_buffer()
    ser.write(payload); ser.flush()
    time.sleep(wait)
    return ser.read(read)

def read_diag(ser):
    r = send(ser, bytes([CMD, SUB_DIAG]), read=64)
    if not r or len(r) < 1 + 36:
        return None, r
    d = r[1:1+36]   # skip echoed cmd byte
    (lcCrcOk, piValid, rxInit, lastMi, burstCnt, codecCnt) = struct.unpack_from("<6I", d, 0)
    lastLc = d[24:32]
    rxActive, lastAlg, lastKeyId = d[32], d[33], d[34]
    return {
        "lcCrcOk": lcCrcOk, "piValid": piValid, "rxInit": rxInit,
        "lastMi": lastMi, "burstCnt": burstCnt, "codecCnt": codecCnt,
        "lastLc": lastLc.hex(), "rxActive": rxActive,
        "lastAlg": lastAlg, "lastKeyId": lastKeyId,
    }, r

def show(x):
    print("  CRC-valid LCs offered : %d" % x["lcCrcOk"])
    print("  PIs parsed valid      : %d   (alg=%02X keyId=%02X)" % (x["piValid"], x["lastAlg"], x["lastKeyId"]))
    print("  rx_init (stream armed): %d   rxActive=%d" % (x["rxInit"], x["rxActive"]))
    print("  last MI (32-bit)      : %08X" % x["lastMi"])
    print("  voice bursts seen     : %d" % x["burstCnt"])
    print("  codec frames decoded  : %d" % x["codecCnt"])
    print("  last LC bytes [0..7]  : %s" % x["lastLc"])

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=None, help="serial port (auto-detect 1fc9:0094 if omitted)")
    ap.add_argument("--watch", action="store_true", help="poll every second until Ctrl-C")
    a = ap.parse_args()
    port = a.port or find_port()
    if not port:
        sys.exit("radio not found (USB 1fc9:0094). Specify --port.")
    print("port:", port)
    with serial.Serial(port, 115200, timeout=0.5) as ser:
        send(ser, bytes([CMD, SUB_SHOW_CPS]))
        try:
            while True:
                x, raw = read_diag(ser)
                if x is None:
                    print("no/short reply:", raw.hex() if raw else "(none)")
                else:
                    print("--- AES RX diag ---"); show(x)
                if not a.watch:
                    break
                time.sleep(1.0)
        finally:
            send(ser, bytes([CMD, SUB_CLOSE]))

if __name__ == "__main__":
    main()
