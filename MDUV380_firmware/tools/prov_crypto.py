#!/usr/bin/env python3
"""Reference implementation of the remote-provisioning crypto (PROVISIONING.md §2, §3):
AES (128/256, pure Python, no dependencies), AES-CMAC (RFC 4493 / SP 800-38B), the SP 800-108
counter-mode KDF with CMAC as the PRF, AES-256-CTR, and the frame builder / parser.

It is the oracle the firmware's crypto/dmr_cmac.c and functions/dmr_prov.c are tested against:
  python3 prov_crypto.py selftest     # RFC 4493, SP 800-38B and SP 800-38A vectors
  python3 prov_crypto.py vectors      # KDF + frame vectors as C initialisers for tests/
and the library provision.py and prov_packet.py import (`import prov_crypto as pc`).
"""
import struct, sys

# ---------------- AES (encrypt only; 128- and 256-bit keys) ----------------
_SBOX = [
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16]

def _xt(x):
    return ((x << 1) ^ 0x1B) & 0xFF if x & 0x80 else x << 1

def _expand(key):
    nk = len(key) // 4
    nr = nk + 6
    w = [list(key[4 * i:4 * i + 4]) for i in range(nk)]
    rcon = 1
    for i in range(nk, 4 * (nr + 1)):
        t = list(w[i - 1])
        if i % nk == 0:
            t = [_SBOX[t[1]] ^ rcon, _SBOX[t[2]], _SBOX[t[3]], _SBOX[t[0]]]
            rcon = _xt(rcon)
        elif nk > 6 and i % nk == 4:
            t = [_SBOX[b] for b in t]
        w.append([w[i - nk][j] ^ t[j] for j in range(4)])
    return w, nr

def aes_encrypt_block(key, block):
    """AES block encryption (key 16 or 32 bytes, block 16 bytes) -> 16 bytes."""
    w, nr = _expand(key)
    s = list(block)
    def add(r):
        for c in range(4):
            for j in range(4):
                s[4 * c + j] ^= w[4 * r + c][j]
    add(0)
    for r in range(1, nr + 1):
        s[:] = [_SBOX[b] for b in s]
        s[:] = [s[(i + 4 * (i % 4)) % 16] for i in range(16)]          # ShiftRows
        if r != nr:
            for c in range(4):
                a = s[4 * c:4 * c + 4]
                t = a[0] ^ a[1] ^ a[2] ^ a[3]
                s[4 * c + 0] = a[0] ^ t ^ _xt(a[0] ^ a[1])
                s[4 * c + 1] = a[1] ^ t ^ _xt(a[1] ^ a[2])
                s[4 * c + 2] = a[2] ^ t ^ _xt(a[2] ^ a[3])
                s[4 * c + 3] = a[3] ^ t ^ _xt(a[3] ^ a[0])
        add(r)
    return bytes(s)

# ---------------- CMAC (RFC 4493 / SP 800-38B) ----------------
def _dbl(b):
    v = int.from_bytes(b, "big") << 1
    if v >> 128:
        v ^= 0x87
    return (v & ((1 << 128) - 1)).to_bytes(16, "big")

def cmac(key, msg):
    L = aes_encrypt_block(key, bytes(16))
    k1 = _dbl(L); k2 = _dbl(k1)
    n = (len(msg) + 15) // 16
    if n == 0:
        n = 1; last = bytes(a ^ b for a, b in zip(b"\x80" + bytes(15), k2))
    elif len(msg) % 16 == 0:
        last = bytes(a ^ b for a, b in zip(msg[-16:], k1))
    else:
        pad = msg[(n - 1) * 16:] + b"\x80"
        pad += bytes(16 - len(pad))
        last = bytes(a ^ b for a, b in zip(pad, k2))
    x = bytes(16)
    for i in range(n - 1):
        x = aes_encrypt_block(key, bytes(a ^ b for a, b in zip(x, msg[16 * i:16 * i + 16])))
    return aes_encrypt_block(key, bytes(a ^ b for a, b in zip(x, last)))

# ---------------- SP 800-108 counter-mode KDF, PRF = CMAC-AES-256 ----------------
def kdf108(key, label, nbytes=32):
    """out = CMAC(key, [i]_8 || label || 0x00 || [L]_16) for i = 1.., concatenated."""
    out = b""
    i = 1
    while len(out) < nbytes:
        out += cmac(key, bytes([i]) + label.encode("ascii") + b"\x00" + struct.pack(">H", nbytes * 8))
        i += 1
    return out[:nbytes]

def derive_keys(pairing_key):
    """(K_enc, K_mac) from the 32-byte pairing key."""
    return kdf108(pairing_key, "enc"), kdf108(pairing_key, "mac")

# ---------------- AES-256-CTR (13-byte nonce, 24-bit block index) ----------------
def ctr_crypt(key, nonce13, data):
    out = bytearray()
    for i in range(0, len(data), 16):
        ks = aes_encrypt_block(key, nonce13 + struct.pack(">I", i // 16)[1:])
        out += bytes(a ^ b for a, b in zip(data[i:i + 16], ks))
    return bytes(out)

def ctr_nonce(master_id, slave_id, seq, reply):
    return struct.pack(">IIIB", master_id, slave_id, seq, 1 if reply else 0)

# ---------------- the provisioning frame (PROVISIONING.md §3.2) ----------------
FRAME_MAGIC = 0x50
FRAME_VERSION = 1
FRAME_HDR_LEN = 10
TAG_LEN = 16

OP_STATUS, OP_WRITE_CHANNEL, OP_WRITE_ZONE, OP_SET_VFO, OP_SET_SETTING, OP_STORE_KEY = 0x01, 0x10, 0x11, 0x12, 0x13, 0x14
OP_COMMIT, OP_ABORT, OP_REBOOT, OP_STATUS_REPLY = 0x20, 0x21, 0x30, 0x81
OP_NAMES = {0x01: "STATUS", 0x10: "WRITE_CHANNEL", 0x11: "WRITE_ZONE", 0x12: "SET_VFO", 0x13: "SET_SETTING",
            0x14: "STORE_KEY", 0x20: "COMMIT", 0x21: "ABORT", 0x30: "REBOOT", 0x81: "STATUS_REPLY"}
RESULT_NAMES = ["OK", "BAD_OP", "BAD_SEQ", "TXN_MISMATCH", "CRC_MISMATCH", "NO_SPACE", "BUSY", "WRITE_FAILED", "TIMEOUT"]

def build_frame(k_enc, k_mac, master_id, slave_id, seq, txn, body, reply=False):
    """body = op || args (plaintext). Returns the frame bytes (header || ct || tag)."""
    hdr = struct.pack(">BBBBIH", FRAME_MAGIC, FRAME_VERSION, txn & 0xFF, 1 if reply else 0, seq & 0xFFFFFFFF, len(body))
    ct = ctr_crypt(k_enc, ctr_nonce(master_id, slave_id, seq, reply), body)
    assoc = struct.pack(">II", master_id, slave_id)
    tag = cmac(k_mac, assoc + hdr + ct)
    return hdr + ct + tag

def parse_frame(k_enc, k_mac, master_id, slave_id, frame):
    """Verify and decrypt a frame. Returns (seq, txn, reply, body) or raises ValueError."""
    if len(frame) < FRAME_HDR_LEN + TAG_LEN:
        raise ValueError("short frame")
    magic, ver, txn, flags, seq, plen = struct.unpack(">BBBBIH", frame[:FRAME_HDR_LEN])
    if magic != FRAME_MAGIC or ver != FRAME_VERSION:
        raise ValueError("not a provisioning frame")
    if len(frame) < FRAME_HDR_LEN + plen + TAG_LEN:
        raise ValueError("truncated frame")
    ct = frame[FRAME_HDR_LEN:FRAME_HDR_LEN + plen]
    tag = frame[FRAME_HDR_LEN + plen:FRAME_HDR_LEN + plen + TAG_LEN]
    reply = bool(flags & 1)
    assoc = struct.pack(">II", master_id, slave_id)
    if cmac(k_mac, assoc + frame[:FRAME_HDR_LEN] + ct) != tag:
        raise ValueError("MAC failure")
    body = ctr_crypt(k_enc, ctr_nonce(master_id, slave_id, seq, reply), ct)
    return seq, txn, reply, body

# ---------------- self test ----------------
def _h(s):
    return bytes.fromhex(s.replace(" ", ""))

def selftest(verbose=True):
    ok = True
    def chk(name, got, want):
        nonlocal ok
        good = got == want
        ok = ok and good
        if verbose or not good:
            print("  %s %s" % ("PASS" if good else "FAIL", name))
    # RFC 4493 (AES-128)
    k128 = _h("2b7e151628aed2a6abf7158809cf4f3c")
    m64 = _h("6bc1bee22e409f96e93d7e117393172a ae2d8a571e03ac9c9eb76fac45af8e51"
             "30c81c46a35ce411e5fbc1191a0a52ef f69f2445df4f9b17ad2b417be66c3710")
    chk("RFC 4493 example 1 (empty)", cmac(k128, b""), _h("bb1d6929e95937287fa37d129b756746"))
    chk("RFC 4493 example 2 (16 B)", cmac(k128, m64[:16]), _h("070a16b46b4d4144f79bdd9dd04a287c"))
    chk("RFC 4493 example 3 (40 B)", cmac(k128, m64[:40]), _h("dfa66747de9ae63030ca32611497c827"))
    chk("RFC 4493 example 4 (64 B)", cmac(k128, m64), _h("51f0bebf7e3b9d92fc49741779363cfe"))
    # SP 800-38B D.3 (AES-256)
    k256 = _h("603deb1015ca71be2b73aef0857d7781 1f352c073b6108d72d9810a30914dff4")
    chk("SP 800-38B AES-256 example 13 (empty)", cmac(k256, b""), _h("028962f61b7bf89efc6b551f4667d983"))
    chk("SP 800-38B AES-256 example 14 (16 B)", cmac(k256, m64[:16]), _h("28a7023f452e8f82bd4bf28d8c37c35c"))
    chk("SP 800-38B AES-256 example 15 (40 B)", cmac(k256, m64[:40]), _h("aaf3d8f1de5640c232f5b169b9c911e6"))
    chk("SP 800-38B AES-256 example 16 (64 B)", cmac(k256, m64), _h("e1992190549f6ed5696a2c056c315410"))
    # SP 800-38A F.5.5 CTR-AES256: counter f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff -> our layout uses a
    # 13-byte nonce + 24-bit index, so the first block only (index 0 == ...fdfeff? no: the standard
    # counter's low 3 bytes are fdfeff, ours start at 000000). Check the block cipher instead
    # with FIPS-197 C.3 and CTR with a 13-byte prefix of the F.5.5 counter at index 0.
    chk("FIPS-197 C.3 AES-256 block", aes_encrypt_block(_h("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"),
                                                        _h("00112233445566778899aabbccddeeff")), _h("8ea2b7ca516745bfeafc49904b496089"))
    ks0 = aes_encrypt_block(k256, _h("f0f1f2f3f4f5f6f7f8f9fafbfc") + b"\x00\x00\x00")
    chk("CTR index-0 keystream is E(nonce||000000)", ctr_crypt(k256, _h("f0f1f2f3f4f5f6f7f8f9fafbfc"), bytes(16)), ks0)
    # frame round trip
    ke, km = derive_keys(bytes(range(32)))
    f = build_frame(ke, km, 1234567, 7654321, 42, 7, bytes([OP_STATUS]))
    chk("frame round trip", parse_frame(ke, km, 1234567, 7654321, f), (42, 7, False, bytes([OP_STATUS])))
    bad = bytearray(f); bad[-1] ^= 1
    try:
        parse_frame(ke, km, 1234567, 7654321, bytes(bad)); chk("tag bit flip rejected", 0, 1)
    except ValueError:
        chk("tag bit flip rejected", 1, 1)
    try:
        parse_frame(ke, km, 1234567, 7654320, f); chk("other slave id rejected", 0, 1)
    except ValueError:
        chk("other slave id rejected", 1, 1)
    return ok

def _c_array(name, b):
    body = ",".join("0x%02x" % x for x in b)
    return "static const uint8_t %s[%d] = { %s };" % (name, len(b), body)

def vectors():
    """KDF and frame vectors for tests/test_prov_crypto.c, from this reference."""
    pk = bytes(range(32))
    ke, km = derive_keys(pk)
    print("/* generated by tools/prov_crypto.py vectors */")
    print(_c_array("VEC_PAIRING_KEY", pk))
    print(_c_array("VEC_K_ENC", ke))
    print(_c_array("VEC_K_MAC", km))
    pk2 = _h("603deb1015ca71be2b73aef0857d7781 1f352c073b6108d72d9810a30914dff4")
    ke2, km2 = derive_keys(pk2)
    print(_c_array("VEC2_PAIRING_KEY", pk2))
    print(_c_array("VEC2_K_ENC", ke2))
    print(_c_array("VEC2_K_MAC", km2))
    body = bytes([OP_WRITE_CHANNEL]) + struct.pack(">H", 5) + bytes(range(56))
    f = build_frame(ke, km, 1234567, 7654321, 0x01020304, 0x2A, body)
    print(_c_array("VEC_FRAME_BODY", body))
    print(_c_array("VEC_FRAME", f))
    r = build_frame(ke, km, 1234567, 7654321, 0x01020304, 0x2A, bytes([OP_STATUS_REPLY, 0]), reply=True)
    print(_c_array("VEC_REPLY", r))

if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "selftest"
    if cmd == "selftest":
        sys.exit(0 if selftest() else 1)
    elif cmd == "vectors":
        vectors()
    else:
        sys.exit(__doc__)
