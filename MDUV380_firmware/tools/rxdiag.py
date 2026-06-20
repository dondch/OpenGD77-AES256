#!/usr/bin/env python3
"""Read the AES-RX state-machine diagnostic ring from an OpenGD77-AES radio built
with DMR_AES_DIAG_RX. Runs over the USB CDC (CPS) port. NOT committed — bench tool
for diagnosing the rapid-re-PTT RX garble.

Firmware records, in the HR-C6000 ISR, a small CCM ring of decrypt events:
  PI      chip-LC PI parse (the MI the chip surfaced) + whether it (re)seeded
  IVGEN   first IV generated for a (re)seed (the MI the new call STARTS decrypting with)
  WRAP    superframe wrap: late-entry-decode MI (le=ok/fail) vs LFSR self-advance fallback
  LCRESET Voice-LC-Header new-call reset (dmrAesRxEnd)
  RXEND   CRC-valid Terminator (dmrAesRxEnd)

Usage (Windows, the CDC enumerates as COM4):
  python rxdiag.py --reset            # clear the ring before a test run
  python rxdiag.py                    # dump + decode the ring
  python rxdiag.py --port COM4
"""
import argparse, struct, sys, time
try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial required: pip install pyserial")

AES_VID, AES_PID = 0x1FC9, 0x0094
TYPE = {1: "PI    ", 2: "WRAP  ", 3: "LCRSET", 4: "RXEND ", 5: "IVGEN ", 6: "BOOT  ", 7: "RESEED"}


def find_port():
    for p in list_ports.comports():
        if (p.vid == AES_VID) and (p.pid == AES_PID):
            return p.device
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=None)
    ap.add_argument("--reset", action="store_true", help="clear/arm the ring, then exit")
    args = ap.parse_args()

    port = args.port or find_port()
    if not port:
        sys.exit("no OpenGD77 CDC port found (1fc9:0094). Pass --port COMx")
    ser = serial.Serial(port, 115200, timeout=0.5)

    if args.reset:
        ser.reset_input_buffer()
        ser.write(bytes([ord("C"), 0x85])); ser.flush(); time.sleep(0.15)
        print("ring cleared:", ser.read(8))
        return

    ser.reset_input_buffer()
    ser.write(bytes([ord("C"), 0x84])); ser.flush(); time.sleep(0.25)
    r = ser.read(2048)
    if len(r) < 3 or r[0] != ord("C"):
        sys.exit("bad reply: %r" % r[:16])
    n = (r[1] << 8) | r[2]
    data = r[3:3 + n]
    if len(data) < 24 or data[0] != 0xA5:
        sys.exit("bad diag header (len=%d): %r" % (len(data), data[:8]))

    ring = data[1]; rec = data[2]
    w = data[4] | (data[5] << 8)
    cnt = struct.unpack_from("<7H", data, 6)   # PI WRAP LCRESET RXEND IVGEN BOOTSTRAP RESEED
    leOk, leFail = struct.unpack_from("<2H", data, 20)
    lcReads, piValid, burstCalls = struct.unpack_from("<3H", data, 24)
    body = data[32:32 + ring * rec]

    print(f"port {port}  ring={ring} rec={rec} writeIdx={w}")
    print(f"counts: PI={cnt[0]} WRAP={cnt[1]} LCRESET={cnt[2]} RXEND={cnt[3]} IVGEN={cnt[4]} "
          f"BOOTSTRAP={cnt[5]} RESEED={cnt[6]}   late-entry: ok={leOk} fail={leFail}")
    print(f"        LC-reads={lcReads}  valid-PI={piValid}  RxBurst-calls={burstCalls}  "
          f"(non-PI LCs = {lcReads-piValid})")
    print("-" * 90)
    print(" # |   ts   dT | type   | seq last | mi        leMi      | flags")
    print("-" * 90)

    nvalid = min(w, ring)
    start = (w - nvalid) % ring if w >= nvalid else 0
    prev_ts = None
    for i in range(nvalid):
        idx = (start + i) % ring
        o = idx * rec
        t = body[o]; fl = body[o + 1]; seq = body[o + 2]
        ls = body[o + 3] - 256 if body[o + 3] >= 128 else body[o + 3]
        ts = body[o + 4] | (body[o + 5] << 8)
        mi = struct.unpack_from("<I", body, o + 8)[0]
        aux = struct.unpack_from("<I", body, o + 12)[0]
        if t == 0:
            continue
        dt = "" if prev_ts is None else f"{(ts - prev_ts) & 0xFFFF:5d}"
        prev_ts = ts
        name = TYPE.get(t, f"?{t}")
        if t == 1:
            f = []
            if fl & 1: f.append("valid")
            f.append("SEEDED" if (fl & 2) else "no-seed")
            f.append("wasActive" if (fl & 4) else "wasIdle")
            fs = " ".join(f)
        elif t == 2:   # WRAP (active self-advance)
            fs = ("LE-OK" if (fl & 1) else "LE-FAIL") + (" confirmed" if (fl & 8) else "")
        elif t == 6:   # BOOTSTRAP (idle -> active from late entry)
            fs = ("LE-OK" if (fl & 1) else "LE-FAIL") + (" confirmed" if (fl & 8) else "") + (" ACTIVATED" if (fl & 0x10) else " (no-seed)")
        elif t == 7:   # RESEED (active track switch on diverging confirmed late entry)
            fs = "SWITCHED-TRACK" + (" confirmed" if (fl & 8) else "")
        elif t in (3, 4):
            fs = "wasActive" if (fl & 1) else "wasIdle"
        else:
            fs = ""
        print(f"{i:2d} | {ts:5d} {dt:>5} | {name} | {seq:3d} {ls:4d} | {mi:08X}  {aux:08X} | {fs}")


if __name__ == "__main__":
    main()
