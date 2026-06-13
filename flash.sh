#!/usr/bin/env bash
# End-to-end: build the OpenGD77 MD-UV390 10W firmware, merge the AMBE codec from the
# official MD-9600 V5 donor, and flash the radio (radio must be in DFU / bootloader mode).
#
# DONOR (codec source - you must supply it, it cannot be redistributed):
#   MD9600-CSV(2571V5)-V26.45.bin   official TYT MD-9600 V5 firmware.
#   The MD-9600 shares the STM32F405 CPU and the AMBE codec with the MD-UV380/390, so OpenGD77
#   uses it as the universal codec donor. Your MD-UV390 AES firmwares are NOT valid donors.
#
# Requires: official ARM GNU toolchain on PATH; python3 with pyusb
#   (pip install -r MDUV380_firmware/tools/requirements.txt).
#
# Usage:  ./flash.sh /path/to/MD9600-CSV2571V5-V26.45.bin [aes]
#         second arg "aes" builds with ENABLE_AES=1 (experimental DMRA AES-256 RX decrypt).
set -euo pipefail
DONOR="${1:-}"
AES=0; [ "${2:-}" = "aes" ] && AES=1
if [ -z "$DONOR" ] || [ ! -f "$DONOR" ]; then
  echo "ERROR: supply the MD-9600 V5 donor firmware (codec source)."
  echo "usage: $0 /path/to/MD9600-CSV2571V5-V26.45.bin [aes]"
  exit 1
fi
cd "$(dirname "$0")/MDUV380_firmware"
echo ">> Validating donor ..."
python3 tools/verify_codec_donor.py "$DONOR" || { echo "Refusing to flash with an invalid donor."; exit 1; }
echo ">> Building firmware (ENABLE_AES=$AES) ..."
make -j"$(nproc)" ENABLE_AES="$AES"
echo ">> Flashing (radio must be in DFU/bootloader mode) ..."
python3 tools/opengd77_stm32_firmware_loader.py -s "$DONOR" -f build/openuv380-10w.bin -m MD-UV380
echo ">> Done. The loader prints 'Patching for DMR' on success."
