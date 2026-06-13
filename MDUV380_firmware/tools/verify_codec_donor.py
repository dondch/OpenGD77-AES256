#!/usr/bin/env python3
"""Validate an AMBE codec donor for the OpenGD77 MD-UV380/390 loader WITHOUT flashing.

Replicates the loader's codec extraction (seek 0xC2C7C, length 0x48BB0, MD9600 cipher, merge
offset 0x6937C) and checks that the result decodes to real ARM code (low entropy). The expected
donor is MD-9600 V5:  MD9600-CSV(2571V5)-V26.45.bin  (same CPU + AMBE codec as the MD-UV380/390).

Usage:  python3 verify_codec_donor.py <donor.bin>
"""
import sys, re, math, os
from collections import Counter

CODEC_OFFSET = 0x0C2C7C
CODEC_LEN    = 0x048BB0
MERGE_OFFSET = 0x06937C

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
print("codec region entropy: %.2f bits/byte" % ent)
if ent < 7.0:
    print("VALID donor: codec decodes to real code.")
else:
    print("INVALID donor: not the expected file.")
    print("Use MD-9600 V5: MD9600-CSV(2571V5)-V26.45.bin")
    sys.exit(1)
