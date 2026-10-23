#!/usr/bin/env python3
"""Validate an AMBE codec donor for the OpenGD77 MD-UV380/390 loader WITHOUT flashing.

Replicates the loader's codec extraction (seek 0xC2C7C, length 0x48BB0, MD9600 cipher, merge
offset 0x6937C) and checks that the three AMBE codec entry points decrypt to valid Thumb-2
function prologues (PUSH.W {..,lr} == bytes 2D E9). This is the definitive signature; entropy
alone is unreliable because the codec contains large high-entropy vocoder tables (~7.5 bits/byte).

Expected donor: MD-9600 V5  ->  MD9600-CSV(2571V5)-V26.45.bin
Usage:  python3 verify_codec_donor.py <donor.bin>
"""
import sys, re, math, os
from collections import Counter

CODEC_OFFSET = 0x0C2C7C   # codec location in the donor file
CODEC_LEN    = 0x048BB0   # codec size
MERGE_OFFSET = 0x06937C   # where it is written in the open firmware (flash 0x0807537C)
BASE         = 0x0800C000

# AMBE entry points (codec.h); after merge each must be a PUSH.W {..,lr} (0x2D 0xE9) prologue.
ENTRYPOINTS = {"AMBE_ENCODE": 0x080754AC, "AMBE_ENCODE_ECC": 0x08075864, "AMBE_DECODE": 0x08075954}
PUSHW_LR = b"\x2d\xe9"

here = os.path.dirname(os.path.abspath(__file__))
loader = open(os.path.join(here, "opengd77_stm32_firmware_loader.py")).read()
m = re.search(r"MD9600_ENCODE_CIPHER:Final = \[(.*?)\]", loader, re.S)
cipher = [int(x, 0) for x in re.findall(r"0x[0-9A-Fa-f]+|\d+", m.group(1))]
assert len(cipher) == 1024, "could not read MD9600 cipher from loader"

if len(sys.argv) < 2:
    sys.exit("usage: verify_codec_donor.py <donor.bin>")
data = open(sys.argv[1], "rb").read()
if len(data) < CODEC_OFFSET + CODEC_LEN:
    sys.exit("donor too small / wrong file")

enc = bytearray(data[CODEC_OFFSET:CODEC_OFFSET + CODEC_LEN])
ofs = MERGE_OFFSET % 1024
dec = bytearray(enc[j] ^ cipher[(j + ofs) % 1024] for j in range(len(enc)))

c = Counter(dec); n = len(dec)
ent = -sum((v / n) * math.log2(v / n) for v in c.values())
print("codec region entropy: %.2f bits/byte (info only)" % ent)

ok = True
for name, addr in ENTRYPOINTS.items():
    idx = addr - BASE - MERGE_OFFSET
    sig = bytes(dec[idx:idx + 2])
    good = (sig == PUSHW_LR)
    print("  %-16s @%08x: %s  %s" % (name, addr, sig.hex(), "OK (PUSH.W prologue)" if good else "BAD"))
    ok = ok and good

if ok:
    print("VALID donor: AMBE codec entry points present and correct.")
else:
    print("INVALID donor: not the expected file.")
    print("Use MD-9600 V5: MD9600-CSV(2571V5)-V26.45.bin")
    sys.exit(1)
