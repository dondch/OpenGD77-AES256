#!/usr/bin/env python3
"""Drive the OpenGD77-AES DMR data-TX harness over USB (CPS command C / 0x91).

The firmware keys a DMR data call and feeds each burst we send to the HR-C6000,
which does the BPTC/Trellis FEC. So the DMR data framing is built HERE on the PC and
iterated against a HackRF capture + DSD-FME, with no firmware reflash. Requires an
ENABLE_DMR_DATA firmware build + pyserial. Radio enumerates as USB CDC 1FC9:0094.

Each burst = 1 data-type byte (HR-C6000 reg 0x50: slot-type<<4 | data | hdr | LCSS)
  0x60 = data header, 0x70 = rate-1/2 data, 0x20 = terminator
followed by 12 payload bytes (the 96 info bits the chip FEC-encodes).

  # raw bursts (the iteration vehicle) - TYPE:24-hex-payload, repeatable:
  python3 dmr_data_tx.py --burst 60:0123...  --burst 70:abcd...
  # first-cut SMS builder (framing is the bring-up target - verify via DSD-FME):
  python3 dmr_data_tx.py --sms "hello" --dst 9990 --src 12341 [--group]
"""
import argparse, sys, time
try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial required: pip install -r requirements.txt")

APP_VID, APP_PID = 0x1FC9, 0x0094
CMD = ord('C')
SUB_DATA_TX = 0x91
BURST_LEN = 12
MAX_BURSTS = 24

DT_DATA_HEADER = 0x60
DT_RATE12_DATA = 0x70
DT_TERMINATOR  = 0x20

def find_port():
    for p in list_ports.comports():
        if (p.vid == APP_VID) and (p.pid == APP_PID):
            return p.device
    return None

def crc_ccitt(data):  # CRC16-CCITT (0x1021), used by DMR data headers/blocks
    crc = 0x0000
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if (crc & 0x8000) else (crc << 1) & 0xFFFF
    return crc

def parse_burst(s):
    t, _, h = s.partition(":")
    t = int(t, 16)
    payload = bytes.fromhex(h)
    if len(payload) > BURST_LEN:
        sys.exit("burst payload > %d bytes: %s" % (BURST_LEN, s))
    return bytes([t]) + payload.ljust(BURST_LEN, b"\x00")

def build_sms(text, dst, src, group):
    """FIRST-CUT DMR 'Short Data: Defined' SMS frame. The exact header bit layout +
    whether the chip appends the CRC is the bring-up target (verify with DSD-FME and
    adjust). Text -> UTF-16BE, split into 12-byte rate-1/2 blocks, CRC32 over the
    message in the final block; a data header announces dst/src + blocks-to-follow."""
    body = text.encode("utf-16-be")
    # pad message to a whole number of rate-1/2 blocks (12 bytes), reserving 4 bytes
    # in the last block for the message CRC32 (ETSI: 32-bit CRC over the user data).
    import struct, zlib
    nblocks = max(1, (len(body) + 4 + BURST_LEN - 1) // BURST_LEN)
    buf = body.ljust(nblocks * BURST_LEN - 4, b"\x00")
    buf += struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)
    blocks = [buf[i:i + BURST_LEN] for i in range(0, len(buf), BURST_LEN)]

    gi = 0x80 if group else 0x00
    dpf = 0x0D  # Short Data: Defined
    hdr = bytes([
        gi | dpf,                                   # G/I + DPF  (layout TBD via capture)
        0x00,                                       # SAP / format hi
        (dst >> 16) & 0xFF, (dst >> 8) & 0xFF, dst & 0xFF,
        (src >> 16) & 0xFF, (src >> 8) & 0xFF, src & 0xFF,
        nblocks & 0xFF,                             # blocks to follow
        0x00,                                       # DD format (0 = ?), TBD
    ])
    hdr = hdr + struct.pack(">H", crc_ccitt(hdr))   # 12 bytes (chip may re-add CRC)

    bursts = [bytes([DT_DATA_HEADER]) + hdr]
    for b in blocks:
        bursts.append(bytes([DT_RATE12_DATA]) + b)
    return bursts

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--burst", action="append", default=[], help="raw burst TYPE:payload-hex (repeatable)")
    ap.add_argument("--sms", help="build+send a first-cut DMR SMS with this text")
    ap.add_argument("--dst", type=int, default=0, help="destination DMR ID / TG")
    ap.add_argument("--src", type=int, default=0, help="source DMR ID")
    ap.add_argument("--group", action="store_true", help="group (TG) addressing instead of individual")
    ap.add_argument("--port", default=None, help="serial port (auto-detect 1FC9:0094 if omitted)")
    a = ap.parse_args()

    if a.sms is not None:
        bursts = build_sms(a.sms, a.dst, a.src, a.group)
    elif a.burst:
        bursts = [parse_burst(s) for s in a.burst]
    else:
        sys.exit("nothing to send: pass --sms or one or more --burst")

    if len(bursts) > MAX_BURSTS:
        sys.exit("too many bursts (%d > %d)" % (len(bursts), MAX_BURSTS))

    port = a.port or find_port()
    if not port:
        sys.exit("radio not found (USB 1FC9:0094). Specify --port.")
    payload = bytes([CMD, SUB_DATA_TX, len(bursts)]) + b"".join(bursts)
    print("port: %s  bursts: %d  bytes: %d" % (port, len(bursts), len(payload)))
    for i, b in enumerate(bursts):
        print("  burst %2d  type 0x%02X  %s" % (i, b[0], b[1:].hex()))

    with serial.Serial(port, 115200, timeout=0.5) as ser:
        ser.reset_input_buffer()
        ser.write(payload)
        ser.flush()
        time.sleep(0.2)
        ack = ser.read(8)
        print("reply:", ack.hex() if ack else "(none)")
    print("sent. Capture with HackRF + decode (dsd-fme -fs) to verify the framing.")

if __name__ == "__main__":
    main()
