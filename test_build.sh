#!/usr/bin/env bash
# Build test (no radio / no donor needed): builds every build profile, asserts each .bin is
# large enough to receive the AMBE codec merge (the Makefile's own size report enforces the
# RAM floor), and asserts that the flag combinations the Makefile forbids are refused.
# Optionally validates a donor firmware offline.
#
# Usage:  ./test_build.sh [/path/to/MD9600-CSV2571V5-V26.45.bin]
set -euo pipefail
cd "$(dirname "$0")/MDUV380_firmware"
MERGE_MIN=$(( 0x6937C + 0x48BB0 ))   # 728876 = codec write offset + codec size
ok=1
for p in $(make -s profiles); do
  make -j"$(nproc)" BUILD="build-$p" PROFILE="$p" > "build-$p.log" 2>&1 || { echo "PROFILE=$p -> BUILD FAILED (see MDUV380_firmware/build-$p.log)"; tail -5 "build-$p.log"; ok=0; continue; }
  sz=$(stat -c%s "build-$p/openuv380-10w.bin")
  printf "PROFILE=%-11s -> %s bytes  " "$p" "$sz"
  if [ "$sz" -ge "$MERGE_MIN" ]; then echo "[merge-capable]"; else echo "[FAIL: too small for codec merge]"; ok=0; fi
  grep -E '^  (SRAM|CCM) ' "build-$p.log" | sed 's/^/    /'
done
echo ">> Forbidden combinations must be refused by the Makefile:"
for bad in "ENABLE_SCAN_PROFILER=1" "DMR_AES_DIAG_PATTERN=1 DMR_AES_DIAG_ENCPAT=1" "ENABLE_REMOTE_PROVISION=1" "PROFILE=no-such-profile"; do
  # shellcheck disable=SC2086
  if make -s flags $bad > /dev/null 2>&1; then echo "   FAIL: 'make $bad' was accepted"; ok=0; else echo "   ok: 'make $bad' refused"; fi
done
[ "$ok" = 1 ] && echo "PASS: all profiles build and are codec-merge-capable." || { echo "BUILD TEST FAILED"; exit 1; }
DONOR="${1:-}"
if [ -n "$DONOR" ] && [ -f "$DONOR" ]; then
  echo ">> Validating donor offline ..."
  python3 tools/verify_codec_donor.py "$DONOR"
fi
