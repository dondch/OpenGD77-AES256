#!/usr/bin/env python3
"""Crack the RX late-entry bit layout. The firmware (DMR_AES_DIAG_RX build) captures one
clean superframe's 6 raw 27-byte AMBE bursts + the MI its late-entry encodes + the
firmware-built expected fragment (dmr_le_mi_build of that MI). We know the EXPECTED 4-bit
nibble for each (voice-frame vc, codeword cw); this finds which raw OTA bit each nibble bit
actually sits at — cw=0 decodes today but cw=1/cw=2 don't, so their bits are elsewhere.

Flow (Windows, COM4):
  python fragcap.py --arm          # arm; then key up a CLEAN (spaced) encrypted call
  python fragcap.py                # dump + analyse (append this run to the accumulator)
  python fragcap.py --reset-acc    # forget accumulated runs
Repeat arm/dump over a few different calls (different MIs) for more samples.
"""
import argparse, json, os, struct, sys, tempfile, time
try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial required: pip install pyserial")

AES_VID, AES_PID = 0x1FC9, 0x0094
ACC = os.path.join(tempfile.gettempdir(), "fragcap_acc.json")


def find_port():
    for p in list_ports.comports():
        if (p.vid == AES_VID) and (p.pid == AES_PID):
            return p.device
    return None


def rawbit(buf27, p):
    """OTA bit p (0..215) of a 27-byte buffer, MSB-first."""
    return (buf27[p >> 3] >> (7 - (p & 7))) & 1


def extract_nib(buf27, cw):
    """Current firmware extraction: OTA bits 71/67/63/59 of codeword cw."""
    c = buf27[cw * 9: cw * 9 + 9]
    return (((c[8] & 0x01) << 3) | (0x04 if (c[8] & 0x10) else 0) |
            ((c[7] & 0x01) << 1) | (0x01 if (c[7] & 0x10) else 0))


def err_report(runs):
    """Per-Golay-word bit-error count (extracted vs expected) — is a fail signal or a Golay bug?
    word j = the cw=j nibbles of voice frames vc=1..6 (24 bits); Golay(24,12,8) corrects <=3."""
    print("\n=== per-superframe late-entry bit errors (extracted vs expected) ===")
    for r in runs:
        seqs, exp, bursts = r["seqs"], r["exp"], r["bursts"]
        werr = [0, 0, 0]
        for i, vc in enumerate(seqs):
            if not (1 <= vc <= 6):
                continue
            for cw in range(3):
                got = extract_nib(bursts[i], cw)
                want = exp[(vc - 1) * 3 + cw]
                werr[cw] += bin(got ^ want).count("1")
        verdict = "DECODABLE (<=3/word)" if all(e <= 3 for e in werr) else "uncorrectable (>3 in a word)"
        print(f"  MI={r['mi']:08X}  word errors cw0/1/2 = {werr[0]}/{werr[1]}/{werr[2]}   -> {verdict}")


def analyse(runs):
    # current firmware assumption: nibble bit b (3,2,1,0) of codeword cw -> OTA bit cw*72 + (71,67,63,59)
    cur = {3: 71, 2: 67, 1: 63, 0: 59}
    print(f"\n=== majority-agreement bit search over {len(runs)} run(s), "
          f"{sum(len(r['seqs']) for r in runs)} bursts ===")
    print("   for each nibble bit: the OTA position whose raw bit best agrees with the expected")
    print("   bit across all samples. ~100%% at a position == that is the true bit. The current")
    print("   firmware position's agreement is shown; <90%% there => extraction bug, not noise.\n")
    for cw in range(3):
        line = f"  cw={cw}: "
        for b in range(3, -1, -1):
            samples = []
            for r in runs:
                for i, vc in enumerate(r["seqs"]):
                    if not (1 <= vc <= 6):
                        continue
                    nib = r["exp"][(vc - 1) * 3 + cw]
                    samples.append((bytes(r["bursts"][i]), (nib >> b) & 1))
            n = len(samples)
            if not n:
                continue
            # best position by agreement (direct or inverted), and the current position's agreement
            best_p, best_agree, best_inv = 0, -1, 0
            for p in range(216):
                a = sum(1 for buf, ev in samples if rawbit(buf, p) == ev)
                ai = n - a
                if a > best_agree: best_p, best_agree, best_inv = p, a, 0
                if ai > best_agree: best_p, best_agree, best_inv = p, ai, 1
            curp = cw * 72 + cur[b]
            cura = sum(1 for buf, ev in samples if rawbit(buf, p=curp) == ev)
            line += (f"\n     bit{b}: best=pos{best_p}(B{best_p//8}.{7-(best_p%8)}){'inv' if best_inv else ''} "
                     f"{best_agree}/{n}  |  current pos{curp}={cura}/{n}"
                     f"{'  <-- OK' if cura >= 0.9*n else '  <-- WRONG?'}")
        print(line)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=None)
    ap.add_argument("--arm", action="store_true")
    ap.add_argument("--reset-acc", action="store_true")
    args = ap.parse_args()

    if args.reset_acc:
        if os.path.exists(ACC):
            os.remove(ACC)
        print("accumulator cleared")
        return

    port = args.port or find_port()
    if not port:
        sys.exit("no OpenGD77 CDC port (1fc9:0094). Pass --port COMx")
    ser = serial.Serial(port, 115200, timeout=0.5)

    if args.arm:
        ser.reset_input_buffer(); ser.write(bytes([ord("C"), 0x88])); ser.flush(); time.sleep(0.15)
        print("armed:", ser.read(8), "- now key up one CLEAN encrypted call, then run without --arm")
        return

    ser.reset_input_buffer(); ser.write(bytes([ord("C"), 0x89])); ser.flush(); time.sleep(0.3)
    r = ser.read(2048)
    if len(r) < 3 or r[0] != ord("C"):
        sys.exit("bad reply: %r" % r[:16])
    n = (r[1] << 8) | r[2]
    d = r[3:3 + n]
    if len(d) < 30:
        sys.exit("short capture: %r" % d[:8])
    state, cnt = d[0], d[1]
    mi = struct.unpack_from("<I", d, 2)[0]
    seqs = list(d[6:12])
    exp = list(d[12:30])
    bursts = [list(d[30 + i * 27: 30 + (i + 1) * 27]) for i in range(6)]
    print(f"state={state} (3=complete) N={cnt} trueMI={mi:08X} seqs={seqs}")
    print("expected frag (vc1..6 x cw0..2):", " ".join(f"{x:X}" for x in exp))
    for i in range(6):
        print(f"  burst[{i}] seq={seqs[i]}: " + " ".join(f"{x:02X}" for x in bursts[i]))
    if state != 3 or cnt < 6:
        print("** capture incomplete (need a clean call while armed); not accumulating **")
        return

    runs = []
    if os.path.exists(ACC):
        runs = json.load(open(ACC))
    runs.append({"mi": mi, "seqs": seqs, "exp": exp, "bursts": bursts})
    json.dump(runs, open(ACC, "w"))
    err_report(runs)
    analyse(runs)


if __name__ == "__main__":
    main()
