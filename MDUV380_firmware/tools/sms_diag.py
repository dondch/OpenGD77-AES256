#!/usr/bin/env python3
"""Read the fork's encrypted-SMS RX diagnostics over USB CPS.
  0x93 = counters (d/hOk/hBad/bOk/bBad/pdu/msg + txActive)
  0x94 = reset counters      (--reset)
  0x95 = dump last reassembled PDU + metadata   (--pdu)
Run from Windows or WSL; auto-detects the OpenGD77 radio (1FC9:0094), or --port COMx with
two radios attached."""
import sys, time, struct
import serial
from serial.tools import list_ports

APP_VID, APP_PID = 0x1FC9, 0x0094

def find_port():
    for p in list_ports.comports():
        if p.vid == APP_VID and p.pid == APP_PID:
            return p.device
    return None

def main():
    port = None
    if "--port" in sys.argv:
        port = sys.argv[sys.argv.index("--port") + 1]
    port = port or find_port()
    if not port:
        sys.exit("radio not found (USB 1FC9:0094); pass --port COMx")
    ser = serial.Serial(port, 115200, timeout=0.5)

    if "--reset" in sys.argv:
        ser.write(bytes([ord("C"), 0x94])); ser.flush(); time.sleep(0.2)
        print("counters reset:", ser.read(8).hex())
        return

    ser.write(bytes([ord("C"), 0x93])); ser.flush(); time.sleep(0.3)
    r = ser.read(128)
    if len(r) >= 3 + 28 + 1 and r[0] == ord("C"):
        vals = struct.unpack_from("<7I", r, 3)
        tx = r[3 + 28]
        names = ["d", "hOk", "hBad", "bOk", "bBad", "pdu", "msg"]
        print("RX diag: " + "  ".join(f"{k}={v}" for k, v in zip(names, vals)) + f"  txActive={tx}")
        # Per-burst trace (firmware with dmrSmsRxTrace): [writeIndex, 8 x (type, state, b0..b3)],
        # printed oldest first. state = blocks held before the burst | haveHeader<<6 | pduReady<<7.
        n = (r[1] << 8) | r[2]
        body = r[3:3 + n]
        if len(body) >= 29 + 49:
            w = body[29]
            ents = [body[30 + i * 6:36 + i * 6] for i in range(8)]
            print("last bursts (oldest first): type cnt hdr rdy bytes0-3")
            for k in range(8):
                e = ents[(w + k) % 8]
                if not any(e):
                    continue
                print(f"  {e[0]:2d}   {e[1] & 0x3F:2d}   {(e[1] >> 6) & 1}   {(e[1] >> 7) & 1}  "
                      f"{e[2]:02x} {e[3]:02x} {e[4]:02x} {e[5]:02x}")
        # Then (firmware with dmrDataTxDiag): the last data call as the TX ISR saw it, loaded = bursts
        # queued, sent = bursts fetched, endedBy 1 = the queue drained / 2 = the finish poll timed
        # out, ampTx = the RF audio amp was open at a transmitted burst; and ampRx = it was open at
        # a received data burst since the last reset.
        if len(body) >= 29 + 49 + 5:
            t = body[29 + 49:29 + 54]
            print(f"last data call TX: loaded={t[0]} sent={t[1]} endedBy={t[2]} ampTx={t[3]}   ampRx={t[4]}")
    else:
        print("unexpected reply (%d B): %s" % (len(r), r.hex()))

    if "--pdu" in sys.argv:
        ser.write(bytes([ord("C"), 0x95])); ser.flush(); time.sleep(0.3)
        r = ser.read(512)
        if len(r) >= 3 and r[0] == ord("C"):
            n = (r[1] << 8) | r[2]; body = r[3:3 + n]
            if len(body) >= 8:
                plen = (body[0] << 8) | body[1]
                keyid, exp = body[2], body[3]
                peer = struct.unpack_from("<I", body, 4)[0]
                print(f"last PDU: len={plen} keyId={keyid} expBlocks={exp} peer={peer}")
                print("  raw:", body[8:8 + plen].hex())
        else:
            print("no PDU reply:", r.hex())

main()
