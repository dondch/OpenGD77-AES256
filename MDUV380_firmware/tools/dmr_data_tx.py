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

DT_CSBK        = 0x30
DT_DATA_HEADER = 0x60
DT_RATE12_DATA = 0x70
DT_TERMINATOR  = 0x20

# Verbatim frames captured off-air from a stock TYT sending an encrypted SMS on PMR01
# (src 12341, tgt 9661, KEY1, MI AB5C8269), DSD-FME -Z dump. Replaying these proves the
# harness emits real-radio-compatible encrypted-SMS framing; a TYT w/ KEY1 should decrypt
# to the original text. Each entry = (data-type byte, 12-byte payload incl. its real CRC).
REPLAY_REAL = [
    (DT_CSBK,        "bd00c00c0025bd0030354223"),  # preamble CSBK, countdown 0x0C
    (DT_CSBK,        "bd00c00b0025bd0030355b67"),  #                          0x0B
    (DT_CSBK,        "bd00c00a0025bd003035e306"),  #                          0x0A
    (DT_CSBK,        "bd00c0090025bd0030353b84"),  #                          0x09
    (DT_CSBK,        "bd00c0080025bd00303583e5"),  #                          0x08
    (DT_CSBK,        "bd00c0070025bd003035090c"),  #                          0x07
    (DT_DATA_HEADER, "82980025bd0030358600dd02"),  # Unconfirmed Delivery hdr, SAP09 EXTD
    (DT_DATA_HEADER, "4f1051010000ab5c82692f03"),  # Extended DMRA ENC hdr (ALG05, MI AB5C8269)
    (DT_RATE12_DATA, "98e55068bc73a980dee68105"),  # encrypted block 1
    (DT_RATE12_DATA, "679e8d245cdaeb689ead9994"),  #                 2
    (DT_RATE12_DATA, "ab43a67f673a45acf03d11c4"),  #                 3
    (DT_RATE12_DATA, "305a0fa97d4a9f9267d309fa"),  #                 4
    (DT_RATE12_DATA, "00000000000000003c11fb83"),  # final block (pad + msg CRC32)
]

def find_port():
    for p in list_ports.comports():
        if (p.vid == APP_VID) and (p.pid == APP_PID):
            return p.device
    return None

def crc_ccitt(data, init=0xFFFF):  # CRC16-CCITT (0x1021); DMR headers use init 0xFFFF
    crc = init
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if (crc & 0x8000) else (crc << 1) & 0xFFFF
    return crc

def _reflect(x, bits):
    r = 0
    for i in range(bits):
        if x & (1 << i):
            r |= 1 << (bits - 1 - i)
    return r

def crc16(data, poly=0x1021, init=0x0000, refin=False, refout=False, xorout=0x0000):
    """General CRC-16 so we can sweep variants against DSD-FME's data-header check."""
    crc = init
    for b in data:
        if refin:
            b = _reflect(b, 8)
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ poly) & 0xFFFF if (crc & 0x8000) else (crc << 1) & 0xFFFF
    if refout:
        crc = _reflect(crc, 16)
    return crc ^ xorout

# Candidate CRC-16 variants for the DMR data-header bring-up. The chip TXes our 12 header
# bytes verbatim and the BPTC bit-mapping is proven correct (the AES PI header validates),
# so the only unknown is which CRC value DSD-FME / real radios expect. Sweep them in one TX.
_CRC_ALGOS = {
    "ccitt_false": dict(init=0xFFFF),
    "xmodem":      dict(init=0x0000),
    "x25":         dict(init=0xFFFF, refin=True, refout=True, xorout=0xFFFF),
    "kermit":      dict(init=0x0000, refin=True, refout=True),
}
_CRC_MASKS = {"none": 0x0000, "cccc": 0xCCCC}

def build_crc_sweep():
    """One data header per CRC recipe, each with src = its recipe index. DSD-FME only
    validates (and prints 'Source: N') the burst whose CRC recipe is correct -- so the
    Source number on the green line names the winning algo+mask+endian directly."""
    recipes = []
    for an, ap in _CRC_ALGOS.items():
        for mn, mv in _CRC_MASKS.items():
            for end in ("be", "le"):
                recipes.append((an, mn, end, ap, mv))
    bursts, legend = [], []
    for i, (an, mn, end, ap, mv) in enumerate(recipes):
        dst, src = 9661, i  # src encodes the recipe index for unambiguous identification
        data = bytes([0x8D, 0x00,
                      (dst >> 16) & 0xFF, (dst >> 8) & 0xFF, dst & 0xFF,
                      (src >> 16) & 0xFF, (src >> 8) & 0xFF, src & 0xFF,
                      0x06, 0x00])
        v = crc16(data, **ap) ^ mv
        tail = bytes([(v >> 8) & 0xFF, v & 0xFF]) if end == "be" else bytes([v & 0xFF, (v >> 8) & 0xFF])
        legend.append((i, "src=%d" % src, "%s+%s+%s" % (an, mn, end)))
        bursts.append(bytes([DT_DATA_HEADER]) + data + tail)
    return bursts, legend

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
    # DMR data-header CRC: CRC-CCITT over the 10 header octets, XORed with the
    # 0xCCCC header mask (ETSI TS 102 361-1 B.3.11). The chip TXes our 12 bytes
    # verbatim (a chip-computed CRC would have validated), so we own this.
    hdr = hdr + struct.pack(">H", crc_ccitt(hdr) ^ 0xCCCC)  # 12 bytes

    # Send the data header TWICE: the first burst of a keyed TX is the weak PTT/sync
    # ramp-up (the receiver hasn't locked yet), so a lone leading header is usually lost
    # -- the firmware sends the AES PI header twice for the same reason. The second copy
    # lands after sync. (Real data calls use CSBK preamble bursts for this; TODO.)
    hdr_burst = bytes([DT_DATA_HEADER]) + hdr
    bursts = [hdr_burst, hdr_burst]
    for b in blocks:
        bursts.append(bytes([DT_RATE12_DATA]) + b)
    return bursts

# ---- DMR UDT (Unified Data Transport) UTF-16BE text SMS ----------------------
# RE'd byte-exact from DSD-FME (dmr_block.c dmr_udt_decoder + dmr_utils.c CRCs). A UDT
# message = a DPF-0 data header burst (0x60) + UAB appended rate-1/2 blocks (0x70).
# Header/CSBK CRC = CCITT16(init=0,poly=0x1021,MSB-first) ^ 0xFFFF ^ crcmask, big-endian
# (crcmask: data header 0xCCCC, CSBK 0xA5A5). Appended-blocks CRC = same CCITT16 ^ 0xFFFF
# (no mask) over all appended bits except its own trailing 16, stored as the last 2 bytes.

def _crc16d(data):  # DSD ComputeCrcCCITT(16d): bit-wise init 0, poly 0x1021, then ^0xFFFF
    crc = 0
    for byte in data:
        for k in range(7, -1, -1):
            bit = (byte >> k) & 1
            crc = (((crc << 1) ^ 0x1021) & 0xFFFF) if (((crc >> 15) & 1) ^ bit) else ((crc << 1) & 0xFFFF)
    return crc ^ 0xFFFF

def _hdr_crc(hdr10, mask):
    v = _crc16d(hdr10) ^ mask
    return bytes([(v >> 8) & 0xFF, v & 0xFF])

def build_csbk_preamble(dst, src, group, count):
    """N preamble CSBKs (Group/Indiv Data), countdown in byte3. CSBKO 0x3D (=0xBD w/ LB)."""
    bursts = []
    for i in range(count):
        body = bytes([
            0xBD, 0x00, 0xC0 if group else 0x80, (count - 1 - i) & 0xFF,
            (dst >> 16) & 0xFF, (dst >> 8) & 0xFF, dst & 0xFF,
            (src >> 16) & 0xFF, (src >> 8) & 0xFF, src & 0xFF,
        ])
        bursts.append(bytes([DT_CSBK]) + body + _hdr_crc(body, 0xA5A5))
    return bursts

def build_udt(text, dst, src, group=True):
    chars = [ord(c) for c in text]
    n = len(chars)
    cap = [5, 11, 17, 23]
    uab = next((i + 1 for i, c in enumerate(cap) if n <= c), 4)
    pad_chars = (6 * uab - 1) - n
    padnib = pad_chars * 4
    b0 = (0x80 if group else 0x00)              # IG | A(0) | res(0) | DPF(0=UDT)
    b1 = 0x07                                   # SAP 0 | format2 0x07 (UTF-16BE)
    hdr = bytes([
        b0, b1,
        (dst >> 16) & 0xFF, (dst >> 8) & 0xFF, dst & 0xFF,
        (src >> 16) & 0xFF, (src >> 8) & 0xFF, src & 0xFF,
        ((padnib & 0x1F) << 3) | ((uab - 1) & 0x3),
        0x00,
    ])
    header_block = hdr + _hdr_crc(hdr, 0xCCCC)
    appended = bytearray()
    for ch in chars:
        appended += bytes([(ch >> 8) & 0xFF, ch & 0xFF])
    appended += bytes(pad_chars * 2)
    bcrc = _crc16d(bytes(appended))             # over text+pad (= blocks*96-16 bits)
    appended += bytes([(bcrc >> 8) & 0xFF, bcrc & 0xFF])
    bursts = [bytes([DT_DATA_HEADER]) + header_block]
    for i in range(0, len(appended), 12):
        bursts.append(bytes([DT_RATE12_DATA]) + bytes(appended[i:i + 12]))
    # Keep the whole CPS command <= 64 bytes (one USB full-speed packet): commands that
    # span two packets get the 2nd packet truncated (the cmd is dispatched on the 1st).
    # 3 + N*13 <= 64  -> N <= 4 bursts. The firmware's own voice-LC/PI bursts already
    # absorb the TX ramp-up, so pad with CSBK preambles only up to that 4-burst budget.
    csbk_n = max(0, 4 - len(bursts))
    return build_csbk_preamble(dst, src, group, csbk_n) + bursts

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--burst", action="append", default=[], help="raw burst TYPE:payload-hex (repeatable)")
    ap.add_argument("--sms", help="build+send a first-cut DMR SMS with this text")
    ap.add_argument("--dst", type=int, default=0, help="destination DMR ID / TG")
    ap.add_argument("--src", type=int, default=0, help="source DMR ID")
    ap.add_argument("--group", action="store_true", help="group (TG) addressing instead of individual")
    ap.add_argument("--port", default=None, help="serial port (auto-detect 1FC9:0094 if omitted)")
    ap.add_argument("--crcsweep", action="store_true", help="send one data header per CRC-16 variant to find the one DSD-FME accepts")
    ap.add_argument("--replay-real", action="store_true", help="replay the verbatim captured stock-TYT encrypted SMS (CSBKs+headers+blocks)")
    ap.add_argument("--udt", help="build+send a cleartext DMR UDT UTF-16 text SMS (DSD-FME decodable)")
    a = ap.parse_args()

    legend = None
    if a.udt is not None:
        bursts = build_udt(a.udt, a.dst, a.src, a.group)
        print("UDT SMS %r -> %d bursts (3 CSBK + UDT header + appended blocks)" % (a.udt, len(bursts)))
    elif getattr(a, "replay_real", False):
        bursts = [bytes([t]) + bytes.fromhex(h) for t, h in REPLAY_REAL]
        print("replaying %d verbatim stock-TYT SMS frames" % len(bursts))
    elif a.crcsweep:
        bursts, legend = build_crc_sweep()
        print("CRC sweep legend (index -> 12-byte header -> CRC recipe):")
        for i, hexhdr, labels in legend:
            print("  [%2d] %s  %s" % (i, hexhdr, ",".join(labels)))
        print("Decode the capture: the burst index printed WITHOUT '(CRC ERR)' is the winner.")
    elif a.sms is not None:
        bursts = build_sms(a.sms, a.dst, a.src, a.group)
    elif a.burst:
        bursts = [parse_burst(s) for s in a.burst]
    else:
        sys.exit("nothing to send: pass --sms, --crcsweep, or one or more --burst")

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
