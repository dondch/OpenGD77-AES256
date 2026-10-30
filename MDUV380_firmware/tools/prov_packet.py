#!/usr/bin/env python3
"""Remote-provisioning packet builder / decoder and the one-radio bench paths (PROVISIONING.md).

Library (imported by provision.py): codeplug record encoders, job items, the on-air framing
(CSBK preamble, confirmed data header, rate-1/2 blocks with DBSN + CRC-9, DMR CRC32) and the
inverse, over the crypto in prov_crypto.py. Command line:

  # (a) replay a transaction into a SLAVE's RX path over USB, no RF (0x9B/2 feeds the bursts to
  #     dmrSmsRxBurst as the HR-C6000 ISR does; the radio then stages, commits and replies):
  python3 prov_packet.py replay --key <64hex> --master 1234567 --slave 7654321 --seq 1 --txn 7 \\
        --channel 5 chan5.bin --zone 2 "Fleet" 5,6,7 --setting txPowerLevel 3 --commit
  #     --seq is the first sequence number to use (one per PDU); read the slave's stored value
  #     with `provision.py show` and use a higher one. --dry-run prints the bursts instead.

  # (b) decode what a MASTER sent, from the data bursts DSD-FME shows for a HackRF capture:
  python3 prov_packet.py decode --key <64hex> --master 1234567 --slave 7654321 \\
        --block 0053...(12 B: DBSN/CRC-9 + 10 payload) --block ... | --blocks capture.txt | --pdu <hex>
  #     prints seq, txn, direction, the operation and its arguments; says why when it fails.

  # (c) build the bursts for the raw data-TX harness (dmr_data_tx.py --burst form) or a test:
  python3 prov_packet.py build --key ... --master M --slave S --seq N --txn T --status

Everything here runs with -I safe: no third-party imports beyond pyserial for the USB paths.
"""
import argparse, struct, sys, time, zlib, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import prov_crypto as pc

# ---------------- codeplug records ----------------
CHANNEL_LEN = 56
ZONE_NAME_LEN = 16

def bcd(v):
    """int -> 8-digit packed BCD as the codeplug stores frequencies (little-endian uint32)."""
    out = 0
    for i in range(8):
        out |= (v % 10) << (4 * i)
        v //= 10
    return out

def css_encode(tone):
    """CTCSS in Hz (float or 'none') -> the 16-bit codeplug CSS field (BCD of Hz*10, 0xFFFF = none).
    DCS as 'D023N' / 'D023I' -> BCD of the code with the DCS flag bits."""
    if tone in (None, "", "none", "None", 0):
        return 0xFFFF
    if isinstance(tone, str) and tone.upper().startswith("D"):
        code = int(tone[1:4], 8 if False else 10)
        inv = tone.upper().endswith("I")
        return (bcd(code) & 0x0FFF) | (0xC000 if inv else 0x8000)
    return bcd(int(round(float(tone) * 10))) & 0xFFFF

def channel_record(name, rx_hz, tx_hz, mode="digital", color_code=1, timeslot=1, contact=1, rx_group=0,
                   power=0, tot=0, bandwidth_25k=False, rx_only=False, rx_tone=None, tx_tone=None,
                   squelch=0, encrypt=0):
    """A 56-byte CodeplugChannel_t as stored (BCD frequencies in 10 Hz units)."""
    r = bytearray(CHANNEL_LEN)
    n = name.encode("ascii", "replace")[:ZONE_NAME_LEN]
    r[0:16] = n + b"\xff" * (16 - len(n))
    struct.pack_into("<II", r, 16, bcd(rx_hz // 10), bcd(tx_hz // 10))
    r[24] = 1 if mode == "digital" else 0          # chMode
    r[25] = power & 0xFF                            # libreDMR_Power (0 = master)
    r[27] = tot & 0xFF
    struct.pack_into("<HH", r, 32, css_encode(rx_tone), css_encode(tx_tone))
    r[40] = 0                                       # rxSignaling (repurposed)
    r[42] = encrypt & 0xFF                          # per-channel AES key slot (0 = inherit, 0xFF = clear)
    r[44] = rx_group & 0xFF
    r[45] = color_code & 0x0F                       # txColor
    struct.pack_into("<H", r, 47, contact & 0xFFFF)
    r[49] = 0                                       # flag1
    r[50] = 0x40 if timeslot == 2 else 0x00         # flag2: 0x40 = TS2
    r[51] = 0x00                                    # flag3
    r[52] = (0x02 if bandwidth_25k else 0) | (0x04 if rx_only else 0)   # flag4
    r[55] = squelch & 0xFF
    return bytes(r)

def zone_record(name, channels):
    """name(16) + 80 x uint16 LE channel indexes (0 = empty); the slave keeps the first
    channelsPerZone entries."""
    n = name.encode("ascii", "replace")[:ZONE_NAME_LEN]
    r = bytearray(n + b"\xff" * (16 - len(n)))
    chans = list(channels)[:80] + [0] * (80 - min(80, len(channels)))
    for c in chans:
        r += struct.pack("<H", c)
    return bytes(r)

# the settings fields the slave accepts, by index (dmr_prov_apply.c s_fields, same order)
SETTING_FIELDS = ["txPowerLevel", "backlightMode", "backLightTimeout", "displayContrast", "scanDelay",
                  "scanModePause", "scanStepTime", "dmrDestinationFilter", "dmrCcTsFilter", "privateCalls",
                  "txTimeoutBeepX5Secs", "micGainDMR", "micGainFM", "ecoLevel", "apo", "autolockTimer",
                  "currentZone", "beepVolumeDivider", "bitfieldOptions0", "bitfieldOptions1"]

# ---------------- items and jobs ----------------
def item_channel(index, record56):
    assert len(record56) == CHANNEL_LEN
    return bytes([pc.OP_WRITE_CHANNEL]) + struct.pack(">H", index) + record56

def item_zone(index, record176):
    assert len(record176) == 176
    return bytes([pc.OP_WRITE_ZONE, index]) + record176

def item_vfo(vfo, record56):
    return bytes([pc.OP_SET_VFO, vfo]) + record56

def item_setting(field, value):
    idx = SETTING_FIELDS.index(field) if isinstance(field, str) else int(field)
    return bytes([pc.OP_SET_SETTING, idx]) + struct.pack("<I", value & 0xFFFFFFFF)

def item_key(slot, key32):
    assert len(key32) == 32 and 1 <= slot <= 15
    return bytes([pc.OP_STORE_KEY, slot]) + key32

def item_status():
    return bytes([pc.OP_STATUS])

def item_reboot():
    return bytes([pc.OP_REBOOT])

def item_abort():
    return bytes([pc.OP_ABORT])

def items_to_job_bytes(items):
    """The job item chain: op(1) len(2 LE) args, per item (also the slave's staged image)."""
    out = b""
    for it in items:
        out += bytes([it[0]]) + struct.pack("<H", len(it) - 1) + it[1:]
    return out

def commit_item(items):
    return bytes([pc.OP_COMMIT, len(items)]) + struct.pack(">I", zlib.crc32(items_to_job_bytes(items)) & 0xFFFFFFFF)

def job_bytes(slave_id, txn, items, commit=True, single=False):
    """What CPS command 0x9A carries: slaveId LE32, txn, flags, item chain."""
    flags = (0x01 if commit else 0) | (0x02 if single else 0)
    return struct.pack("<IBB", slave_id & 0xFFFFFF, txn & 0xFF, flags) + items_to_job_bytes(items)

# ---------------- on-air framing ----------------
def crc16d(data):
    crc = 0
    for b in data:
        for k in range(7, -1, -1):
            bit = (b >> k) & 1
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if ((crc >> 15) & 1) ^ bit else (crc << 1) & 0xFFFF
    return crc ^ 0xFFFF

def hdr_crc(h10, mask):
    return struct.pack(">H", crc16d(h10) ^ mask)

def crc32_dmr(pdu):
    """DMR data-PDU CRC32 (byte-pair swapped, poly 0x04C11DB7) over len*8-32 bits; wire order."""
    crc = 0
    nbits = len(pdu) * 8 - 32
    bitno = 0
    for i in range(0, len(pdu) - 1, 2):
        for byte in (pdu[i + 1], pdu[i]):
            for k in range(7, -1, -1):
                if bitno >= nbits:
                    break
                bit = (byte >> k) & 1
                crc = ((crc << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if ((crc >> 31) & 1) ^ bit else (crc << 1) & 0xFFFFFFFF
                bitno += 1
    return ((crc & 0xFF) << 24) | ((crc & 0xFF00) << 8) | ((crc >> 8) & 0xFF00) | ((crc >> 24) & 0xFF)

def crc9(data, dbsn):
    """ETSI B.3.11 CRC-9 as DSD-FME checks it: poly 0x059 over the data bits then the 7 DBSN bits,
    field = register ^ 0x10F (mask 0x0F0 and the inverted compare). Same as dmrProvCrc9()."""
    crc = 0
    bits = [(b >> (7 - k)) & 1 for b in data for k in range(8)] + [(dbsn >> (6 - k)) & 1 for k in range(7)]
    for bit in bits:
        crc = ((crc << 1) ^ 0x059) & 0x1FF if ((crc >> 8) & 1) ^ bit else (crc << 1) & 0x1FF
    return crc ^ 0x10F

BLK = 10
PREAMBLE = 6
SAP = 9

def pdu_from_frame(frame):
    total = ((len(frame) + 4 + BLK - 1) // BLK) * BLK
    pdu = bytearray(frame + bytes(total - len(frame)))
    struct.pack_into(">I", pdu, total - 4, crc32_dmr(pdu))
    return bytes(pdu)

def bursts_from_pdu(pdu, dst, src, seq):
    """[(type, 12 bytes)]: CSBK preamble, confirmed data header (SAP 9, A, N(S) = seq & 7),
    rate-1/2 blocks with DBSN + CRC-9. Identical to dmrProvBuildBursts()."""
    nblocks = len(pdu) // BLK
    plen = (pdu[8] << 8) | pdu[9]
    poc = max(0, len(pdu) - 4 - (pc.FRAME_HDR_LEN + plen + pc.TAG_LEN))
    out = []
    for i in range(PREAMBLE):
        body = bytes([0xBD, 0x00, 0x80, (PREAMBLE - 1 - i) + 1 + nblocks]) + struct.pack(">I", dst)[1:] + struct.pack(">I", src)[1:]
        out.append((0x30, body + hdr_crc(body, 0xA5A5)))
    h = bytes([0x43 | (((poc >> 4) & 3) << 4), (SAP << 4) | (poc & 0x0F)]) + struct.pack(">I", dst)[1:] + struct.pack(">I", src)[1:] + \
        bytes([0x80 | (nblocks & 0x7F), (seq & 7) << 4])
    out.append((0x60, h + hdr_crc(h, 0xCCCC)))
    for k in range(nblocks):
        payload = pdu[k * BLK:(k + 1) * BLK]
        c9 = crc9(payload, k & 0x7F)
        out.append((0x70, bytes([((k & 0x7F) << 1) | ((c9 >> 8) & 1), c9 & 0xFF]) + payload))
    return out

def request_bursts(k_enc, k_mac, master, slave, seq, txn, body, reply=False):
    frame = pc.build_frame(k_enc, k_mac, master, slave, seq, txn, body, reply)
    pdu = pdu_from_frame(frame)
    return bursts_from_pdu(pdu, master if reply else slave, slave if reply else master, seq), pdu

def pdu_from_blocks(blocks):
    """10-byte payloads out of 12-byte confirmed rate-1/2 blocks (or 10-byte payloads as given)."""
    pdu = b""
    for b in blocks:
        pdu += b[2:12] if len(b) == 12 else b[:10]
    return pdu

def describe_body(body):
    op = body[0]
    name = pc.OP_NAMES.get(op, "op 0x%02x" % op)
    a = body[1:]
    if op == pc.OP_WRITE_CHANNEL and len(a) == 58:
        idx = struct.unpack(">H", a[:2])[0]
        rec = a[2:]
        rx, tx = struct.unpack("<II", rec[16:24])
        return "%s idx %d name %r rx %s tx %s mode %s cc %d ts %d contact %d" % (
            name, idx, rec[:16].rstrip(b"\xff").decode("ascii", "replace"), "%08x" % rx, "%08x" % tx,
            "digital" if rec[24] else "analog", rec[45] & 0x0F, 2 if rec[50] & 0x40 else 1, struct.unpack("<H", rec[47:49])[0])
    if op == pc.OP_WRITE_ZONE and len(a) == 177:
        chans = [struct.unpack("<H", a[17 + 2 * i:19 + 2 * i])[0] for i in range(80)]
        return "%s zone %d name %r channels %s" % (name, a[0], a[1:17].rstrip(b"\xff").decode("ascii", "replace"), [c for c in chans if c])
    if op == pc.OP_SET_VFO and len(a) == 57:
        return "%s vfo %s" % (name, "AB"[a[0]] if a[0] < 2 else a[0])
    if op == pc.OP_SET_SETTING and len(a) == 5:
        f = SETTING_FIELDS[a[0]] if a[0] < len(SETTING_FIELDS) else "field %d" % a[0]
        return "%s %s = %d" % (name, f, struct.unpack("<I", a[1:5])[0])
    if op == pc.OP_STORE_KEY and len(a) == 33:
        return "%s slot %d (key bytes not shown)" % (name, a[0])
    if op == pc.OP_COMMIT and len(a) == 5:
        return "%s count %d crc %08x" % (name, a[0], struct.unpack(">I", a[1:5])[0])
    if op == pc.OP_STATUS_REPLY and len(a) == 23:
        res, txn = a[0], a[1]
        return "%s result %s txn %d lastSeq %d codeplugCrc %08x git %s battery %.2f V staged %d failedMacs %d" % (
            name, pc.RESULT_NAMES[res] if res < len(pc.RESULT_NAMES) else res, txn, struct.unpack(">I", a[2:6])[0],
            struct.unpack(">I", a[6:10])[0], a[10:18].rstrip(b"\x00").decode("ascii", "replace"),
            struct.unpack(">H", a[18:20])[0] / 100.0, a[20], struct.unpack(">H", a[21:23])[0])
    return "%s args %s" % (name, a.hex())

# ---------------- USB (CPS 'C' commands) ----------------
try:
    import serial
    from serial.tools import list_ports
except ImportError:
    serial = None

VID, PID = 0x1FC9, 0x0094

def find_port():
    for p in list_ports.comports():
        if (p.vid == VID) and (p.pid == PID):
            return p.device
    return None

def open_radio(port=None):
    if serial is None:
        sys.exit("pyserial required: pip install pyserial")
    port = port or find_port()
    if not port:
        sys.exit("radio not found (1fc9:0094); pass --port")
    ser = serial.Serial(port, 115200, timeout=0.6)
    ser.reset_input_buffer()
    # Never push the CPS screen ('C' 0): while it is up the radio is in RADIO_MODE_NONE and a
    # data call lights the LED but puts nothing on the air (bench 2026-10-30). The commands
    # below work from any screen. close_cps() pops a screen an older tool left behind.
    return ser

def close_cps(ser):
    cmd(ser, bytes([5]), 8, 0.1)

def cmd(ser, payload, expect, wait=0.15):
    """Send 'C' + payload, read `expect` bytes (or until timeout)."""
    ser.reset_input_buffer()
    ser.write(bytes([ord("C")]) + payload); ser.flush()
    time.sleep(wait)
    return ser.read(expect)

def inject_bursts(ser, bursts):
    """0x9B/2 per burst (header 12 B or rate-1/2 12 B), then 0x9B/3 (terminator)."""
    for t, p in bursts:
        if t == 0x30:
            continue   # CSBKs never reach the reassembler
        dtype = 6 if t == 0x60 else 7
        r = cmd(ser, bytes([0x9B, 2, dtype]) + p, 3, 0.05)
        if not r or r[0] != ord("C"):
            raise RuntimeError("burst inject failed: %r" % r)
    r = cmd(ser, bytes([0x9B, 3]), 3, 0.05)

def read_diag(ser):
    r = cmd(ser, bytes([0x9B, 0]), 3 + 256, 0.3)
    if len(r) < 3 or r[0] != ord("C"):
        raise RuntimeError("diag failed: %r" % r)
    n = (r[1] << 8) | r[2]
    return r[3:3 + n]

def decode_diag(d):
    """dmrProvDiag + dmrProvJobStatus bytes -> dict (never contains key bytes)."""
    out = {}
    if len(d) < 84:
        return {"raw": d.hex()}
    out["paired"], out["nMasters"], out["txnActive"], out["txn"], out["stagedCount"] = d[0], d[1], d[2], d[3], d[4]
    out["stagedUsed"] = (d[5] << 8) | d[6]; out["stageErr"] = d[7]
    for i, k in enumerate(["failedMacs", "rejSeq", "rejAddr", "rejFrame", "commits"]):
        out[k] = (d[8 + 2 * i] << 8) | d[9 + 2 * i]
    out["lastCommitSeq"], out["lastCodeplugCrc"] = struct.unpack(">II", d[18:26])
    out["ackInhibit"], out["replyPending"] = d[26], d[27]
    out["masters"] = []
    for i in range(4):
        mid, last, seen = struct.unpack(">III", d[28 + 12 * i:40 + 12 * i])
        if i < out["nMasters"]:
            out["masters"].append({"id": mid, "lastSeq": last, "lastSeen": seen})
    out["ownSeqNext"], out["ownSeqReserved"] = struct.unpack(">II", d[76:84])
    j = d[84:]
    if len(j) >= 36:
        phases = ["idle", "next", "sending", "wait_ack", "wait_status", "done", "failed"]
        out["job"] = {"phase": phases[j[0]] if j[0] < 7 else j[0], "item": j[1], "count": j[2], "tries": j[3],
                      "outcome": ["none", "done", "failed"][j[4]] if j[4] < 3 else j[4],
                      "failReason": pc.RESULT_NAMES[j[5]] if j[5] < len(pc.RESULT_NAMES) else j[5],
                      "statusSeen": j[6], "ack": ("N(S) %d" % (j[7] & 7)) if j[7] & 0x80 else None,
                      "seq": struct.unpack(">I", j[8:12])[0]}
        if j[6]:
            out["job"]["status"] = describe_body(bytes(j[12:12 + 24]))
    return out

# ---------------- command line ----------------
def add_item_args(ap):
    ap.add_argument("--channel", nargs=2, action="append", metavar=("INDEX", "FILE|HEX"), default=[],
                    help="write channel INDEX from a 56-byte file or 112 hex chars")
    ap.add_argument("--chan", nargs=4, action="append", metavar=("INDEX", "NAME", "RXHZ", "TXHZ"), default=[],
                    help="write a digital channel built from name and frequencies (Hz); see --cc/--ts/--contact")
    ap.add_argument("--cc", type=int, default=1); ap.add_argument("--ts", type=int, default=1); ap.add_argument("--contact", type=int, default=1)
    ap.add_argument("--zone", nargs=3, action="append", metavar=("INDEX", "NAME", "CH,CH,..."), default=[])
    ap.add_argument("--vfo", nargs=2, action="append", metavar=("A|B", "FILE|HEX"), default=[])
    ap.add_argument("--setting", nargs=2, action="append", metavar=("NAME", "VALUE"), default=[])
    ap.add_argument("--voice-key", nargs=2, action="append", metavar=("SLOT", "64HEX"), default=[])
    ap.add_argument("--status", action="store_true", help="a STATUS request")
    ap.add_argument("--reboot", action="store_true", help="a REBOOT")
    ap.add_argument("--commit", action="store_true", help="end the staged items with a COMMIT (replay/build)")

def file_or_hex(s, n):
    if os.path.exists(s):
        b = open(s, "rb").read()
    else:
        b = bytes.fromhex(s)
    if len(b) != n:
        sys.exit("%s: expected %d bytes, got %d" % (s, n, len(b)))
    return b

def items_from_args(a):
    items = []
    for idx, src in a.channel:
        items.append(item_channel(int(idx), file_or_hex(src, 56)))
    for idx, name, rx, tx in a.chan:
        items.append(item_channel(int(idx), channel_record(name, int(rx), int(tx), color_code=a.cc, timeslot=a.ts, contact=a.contact)))
    for idx, name, chans in a.zone:
        items.append(item_zone(int(idx), zone_record(name, [int(c) for c in chans.split(",") if c])))
    for vfo, src in a.vfo:
        items.append(item_vfo(0 if vfo.upper() == "A" else 1, file_or_hex(src, 56)))
    for name, value in a.setting:
        items.append(item_setting(name, int(value, 0)))
    for slot, key in a.voice_key:
        items.append(item_key(int(slot), bytes.fromhex(key)))
    single = []
    if a.status:
        single.append(item_status())
    if a.reboot:
        single.append(item_reboot())
    return items, single

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("replay", "build"):
        p = sub.add_parser(name)
        p.add_argument("--key", required=True, help="pairing key, 64 hex chars")
        p.add_argument("--master", type=int, required=True); p.add_argument("--slave", type=int, required=True)
        p.add_argument("--seq", type=int, required=True, help="first sequence number (one per PDU)")
        p.add_argument("--txn", type=int, default=1)
        add_item_args(p)
        p.add_argument("--port"); p.add_argument("--dry-run", action="store_true")
    p = sub.add_parser("decode")
    p.add_argument("--key", required=True); p.add_argument("--master", type=int, required=True); p.add_argument("--slave", type=int, required=True)
    p.add_argument("--block", action="append", default=[], help="a 12-byte (or 10-byte payload) rate-1/2 block, hex")
    p.add_argument("--blocks", help="file with one hex block per line")
    p.add_argument("--pdu", help="the whole PDU (payloads concatenated), hex")
    p.add_argument("--reply", action="store_true", help="the PDU is a slave's reply (master <- slave)")
    a = ap.parse_args()

    if a.cmd == "decode":
        blocks = [bytes.fromhex(b) for b in a.block]
        if a.blocks:
            blocks += [bytes.fromhex(l.strip().replace(" ", "")) for l in open(a.blocks) if l.strip() and not l.startswith("#")]
        pdu = bytes.fromhex(a.pdu) if a.pdu else pdu_from_blocks(blocks)
        if len(pdu) < 8:
            sys.exit("no PDU bytes")
        if crc32_dmr(pdu) != struct.unpack(">I", pdu[-4:])[0]:
            print("warning: DMR CRC32 does not verify over %d bytes (missing or corrupt block?)" % len(pdu))
        ke, km = pc.derive_keys(bytes.fromhex(a.key))
        try:
            seq, txn, reply, body = pc.parse_frame(ke, km, a.master, a.slave, pdu)
        except ValueError as e:
            sys.exit("rejected: %s" % e)
        print("seq %d txn %d %s: %s" % (seq, txn, "reply" if reply else "request", describe_body(body)))
        return

    key = bytes.fromhex(a.key)
    ke, km = pc.derive_keys(key)
    items, single = items_from_args(a)
    seq = a.seq
    pdus = []
    for it in items:
        b, pdu = request_bursts(ke, km, a.master, a.slave, seq, a.txn, it); pdus.append((seq, it, b)); seq += 1
    if items and a.commit:
        it = commit_item(items)
        b, pdu = request_bursts(ke, km, a.master, a.slave, seq, a.txn, it); pdus.append((seq, it, b)); seq += 1
    for it in single:
        b, pdu = request_bursts(ke, km, a.master, a.slave, seq, a.txn, it); pdus.append((seq, it, b)); seq += 1
    if not pdus:
        sys.exit("nothing to send: add items, --status or --reboot")
    for s, it, b in pdus:
        print("# seq %d: %s (%d bursts)" % (s, describe_body(it), len(b)))
        if a.cmd == "build" or a.dry_run:
            for t, p in b:
                print("%02x:%s" % (t, p.hex()))
    if a.cmd == "replay" and not a.dry_run:
        ser = open_radio(a.port)
        with ser:
            for s, it, b in pdus:
                inject_bursts(ser, b)
                time.sleep(0.3)
                d = decode_diag(read_diag(ser))
                print("  after seq %d: txnActive %d staged %d stageErr %d failedMacs %d rejSeq %d rejAddr %d rejFrame %d commits %d replyPending %d" % (
                    s, d["txnActive"], d["stagedCount"], d["stageErr"], d["failedMacs"], d["rejSeq"], d["rejAddr"], d["rejFrame"], d["commits"], d["replyPending"]))
            print("masters:", d["masters"])
    print("next free seq: %d" % seq)

if __name__ == "__main__":
    main()
