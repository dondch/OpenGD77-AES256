#!/usr/bin/env python3
"""Persistently store DMRA AES-256 keys on an OpenGD77-AES radio using the SAME
flash mechanism the OpenGD77 CPS uses (CPS_ACCESS_FLASH 'X' write commands).

The keys live in a standard OpenGD77 custom-data block (CODEPLUG_CUSTOM_DATA_TYPE_AES_KEYS
= 6) in the SPI-flash custom-data region (FLASH_ADDRESS_OFFSET = 0x20000 on MDUV380).
This is the same region/format the CPS manages themes, boot screens, DMR-ID data, etc.,
so a future CPS GUI can read/write keys the same way. The firmware reads this block at
boot via codeplugGetOpenGD77CustomData() + dmrAesLoadKeys() — no custom firmware store.

Usage:
  python3 aes_key_store.py --key <64hex> [--keyid 1] [--tx-key N] [--port COM4]
  python3 aes_key_store.py --tx-key N            # set active TX key only
  python3 aes_key_store.py --show                # dump the stored block

The block survives reboots (it's in flash). The radio enumerates as USB CDC 1fc9:0094.
"""
import argparse, struct, sys, time
try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial required: pip install pyserial")

AES_VID, AES_PID = 0x1FC9, 0x0094
FLASH_BASE = 0x20000              # FLASH_ADDRESS_OFFSET (MDUV380); custom-data region start
SECTOR = FLASH_BASE // 4096       # = 32
CUSTOM_MAGIC = b"OpenGD77"
HDR_LEN = 12                      # 8-byte magic + 4 reserved, then blocks
TYPE_AES_KEYS = 6
AESK_BLOCK_LEN = 8 + 16 * 36      # 584: "AESK" + ver + txkey + rsvd2 + 16*(valid,keyid,rsvd2,key32)

def find_port():
    for p in list_ports.comports():
        if (p.vid == AES_VID) and (p.pid == AES_PID):
            return p.device
    return None

def show_cps(ser):
    ser.reset_input_buffer(); ser.write(bytes([ord("C"), 0])); ser.flush(); time.sleep(0.1); ser.read(64)

def flash_read(ser, addr, length):
    """CPS 'R' CPS_ACCESS_FLASH read -> bytes (raw SPI-flash address)."""
    out = b""
    while length > 0:
        n = min(length, 1024)
        req = bytes([ord("R"), 1, (addr >> 24) & 0xFF, (addr >> 16) & 0xFF, (addr >> 8) & 0xFF, addr & 0xFF,
                     (n >> 8) & 0xFF, n & 0xFF])
        ser.reset_input_buffer(); ser.write(req); ser.flush(); time.sleep(0.15)
        r = ser.read(n + 8)
        if len(r) < 3 or r[0] != ord("R"):
            raise RuntimeError("flash read failed @%06X: %r" % (addr, r[:8]))
        out += r[3:3 + n]
        addr += n; length -= n
    return out

def flash_write_block(ser, addr, data):
    """Write `data` at raw SPI-flash `addr` via CPS 'X' prepare/send/commit.
    Only the touched bytes change; the rest of the 4 KB sector is preserved
    (the firmware reads the sector first, patches it, then erases+writes)."""
    sector = addr // 4096
    # 1) prepare sector (firmware reads it into its RAM buffer)
    req = bytes([ord("X"), 1, (sector >> 16) & 0xFF, (sector >> 8) & 0xFF, sector & 0xFF])
    ser.reset_input_buffer(); ser.write(req); ser.flush(); time.sleep(0.2)
    r = ser.read(8)
    if not r or r[0] == ord("-"):
        raise RuntimeError("prepare sector failed: %r" % r)
    # 2) send data (<=1528 per chunk; our block is 604 -> one chunk)
    off = 0
    while off < len(data):
        chunk = data[off:off + 1024]
        a = addr + off
        req = bytes([ord("X"), 2, (a >> 24) & 0xFF, (a >> 16) & 0xFF, (a >> 8) & 0xFF, a & 0xFF,
                     (len(chunk) >> 8) & 0xFF, len(chunk) & 0xFF]) + chunk
        ser.reset_input_buffer(); ser.write(req); ser.flush(); time.sleep(0.2)
        r = ser.read(8)
        if not r or r[0] == ord("-"):
            raise RuntimeError("send data failed @%06X: %r" % (a, r))
        off += len(chunk)
    # 3) commit (erase sector + write back)
    ser.reset_input_buffer(); ser.write(bytes([ord("X"), 3])); ser.flush(); time.sleep(0.5)
    r = ser.read(8)
    if not r or r[0] == ord("-"):
        raise RuntimeError("flash write/commit failed: %r" % r)

def find_aes_block(region):
    """Return (block_offset, payload) of the AES_KEYS block within the region bytes,
    or (None, None). region[0:] starts at the custom-data region base."""
    if region[:8] != CUSTOM_MAGIC:
        return None, None
    off = HDR_LEN
    while off + 8 <= len(region):
        dtype, dlen = struct.unpack_from("<ii", region, off)
        if dtype == TYPE_AES_KEYS and 0 < dlen <= AESK_BLOCK_LEN:
            return off, region[off + 8: off + 8 + dlen]
        if dlen == 0 or dlen == -1 or (dtype & 0xFFFFFFFF) == 0xFFFFFFFF:
            return None, None  # hit empty/end without finding it
        off += 8 + dlen
    return None, None

def fresh_payload():
    p = bytearray(AESK_BLOCK_LEN)
    p[0:4] = b"AESK"; p[4] = 1; p[5] = 0  # magic, version, txKeyId=0
    return p

def set_key(payload, keyid, key32):
    p = bytearray(payload)
    slot = -1; freeslot = -1
    for i in range(16):
        e = 8 + i * 36
        if p[e] == 1 and p[e + 1] == keyid: slot = i; break
        if freeslot < 0 and p[e] == 0: freeslot = i
    if slot < 0: slot = freeslot
    if slot < 0: raise RuntimeError("no free key slot")
    e = 8 + slot * 36
    p[e] = 1; p[e + 1] = keyid; p[e + 2] = p[e + 3] = 0; p[e + 4:e + 36] = key32
    return bytes(p), slot

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--key", help="AES-256 key, 64 hex chars")
    ap.add_argument("--keyid", type=int, default=1)
    ap.add_argument("--tx-key", dest="txkey", type=int, default=None, help="active TX key id (0=off)")
    ap.add_argument("--show", action="store_true", help="dump the stored AES_KEYS block")
    ap.add_argument("--port", default=None)
    a = ap.parse_args()
    port = a.port or find_port()
    if not port: sys.exit("radio not found (1fc9:0094); pass --port")

    key = None
    if a.key:
        key = bytes.fromhex(a.key.strip())
        if len(key) != 32: sys.exit("key must be 64 hex chars")
        if not (0 <= a.keyid <= 15): sys.exit("keyid 0..15")

    with serial.Serial(port, 115200, timeout=0.6) as ser:
        show_cps(ser)
        region = flash_read(ser, FLASH_BASE, 1024)
        has_magic = region[:8] == CUSTOM_MAGIC
        boff, payload = find_aes_block(region)
        print("region magic:", "OpenGD77" if has_magic else region[:8].hex(),
              "| AES block:", ("@+%d" % boff) if boff is not None else "none")

        if a.show:
            pl = payload if payload else b""
            print("AESK payload present:", bool(pl), "len:", len(pl))
            if pl[:4] == b"AESK":
                print("  version=%d txKeyId=%d" % (pl[4], pl[5]))
                for i in range(16):
                    e = 8 + i * 36
                    if pl[e] == 1:
                        print("  slot %d: keyId=%d key=%s..." % (i, pl[e + 1], pl[e + 4:e + 8].hex()))
            return

        if (key is None) and (a.txkey is None):
            sys.exit("nothing to do: pass --key and/or --tx-key (or --show)")

        payload = bytearray(payload) if payload else fresh_payload()
        if payload[:4] != b"AESK":
            payload = fresh_payload()
        if key is not None:
            payload, slot = set_key(payload, a.keyid, key)
            payload = bytearray(payload)
            print("set key id %d in slot %d" % (a.keyid, slot))
        if a.txkey is not None:
            payload[5] = a.txkey & 0xFF
            print("set TX key id = %d" % a.txkey)

        # Build the region image: magic(12) + block header(8) + payload(584)
        img = bytearray()
        img += CUSTOM_MAGIC + b"\xFF\xFF\xFF\xFF"
        img += struct.pack("<ii", TYPE_AES_KEYS, AESK_BLOCK_LEN)
        img += bytes(payload)
        flash_write_block(ser, FLASH_BASE, bytes(img))
        print("wrote %d bytes to flash region @0x%X" % (len(img), FLASH_BASE))

        # verify read-back
        rb = flash_read(ser, FLASH_BASE, len(img))
        ok = rb == bytes(img)
        print("read-back verify:", "OK" if ok else "MISMATCH")
        if not ok:
            print("  wrote:", bytes(img)[:24].hex())
            print("  read :", rb[:24].hex())

if __name__ == "__main__":
    main()
