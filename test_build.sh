#!/usr/bin/env bash
# Build test (no radio / no donor needed): builds both variants and asserts each .bin is large
# enough to receive the AMBE codec merge. Optionally validates a donor firmware offline.
#
# Usage:  ./test_build.sh [/path/to/MD9600-CSV2571V5-V26.45.bin]
set -euo pipefail
cd "$(dirname "$0")/MDUV380_firmware"
MERGE_MIN=$(( 0x6937C + 0x48BB0 ))   # 728876 = codec write offset + codec size
ok=1
for AES in 0 1; do
  make clean >/dev/null
  make -j"$(nproc)" ENABLE_AES="$AES" >/dev/null
  sz=$(stat -c%s build/openuv380-10w.bin)
  printf "ENABLE_AES=%s -> %s bytes  " "$AES" "$sz"
  if [ "$sz" -ge "$MERGE_MIN" ]; then echo "[merge-capable]"; else echo "[FAIL: too small for codec merge]"; ok=0; fi
done
[ "$ok" = 1 ] && echo "PASS: builds are codec-merge-capable." || { echo "BUILD TEST FAILED"; exit 1; }
DONOR="${1:-}"
if [ -n "$DONOR" ] && [ -f "$DONOR" ]; then
  echo ">> Validating donor offline ..."
  python3 tools/verify_codec_donor.py "$DONOR"
fi
