# Building the MD-UV390 (10W, AES256) firmware

Based on the official OpenGD77 source release **R20260131** (OpenGD77_MDUV380_DM1701_20260130), set up to
build the **MD-UV390 Plus 10W** target (MDUV380_10W_PLUS_FW = PLATFORM_MDUV380 + PLATFORM_VARIANT_UV380_PLUS_10W)
from the command line / CI, without STM32CubeIDE.

This release includes the fixes for 10W Plus hardware: the screen/backlight and PA control signals are
transposed vs the 5W radios, and newer production batches have low-brightness LCDs - all handled by the
10W_PLUS firmware target. Do NOT flash a 5W build to a 10W radio.

## CI
.github/workflows/build.yml builds on every push with the official ARM GNU toolchain 14.2.Rel1 and uploads
openuv380-10w.{bin,hex,elf}.

## Local build
Needs the official ARM toolchain on PATH (the Ubuntu gcc-arm-none-eabi package lacks newlib-nano specs):

    cd MDUV380_firmware
    make -j$(nproc)        # -> build/openuv380-10w.bin

The Makefile mirrors the CubeIDE MDUV380_10W_PLUS_FW release config: all sources under
Core/ Drivers/ Middlewares/ USB_DEVICE/ application/ (excluding languages_builder.c), startup
Core/Startup/startup_stm32f405vgtx.s, linker STM32F405VGTX_FLASH.ld, -Os,
-mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard,
defines USE_HAL_DRIVER STM32F405xx PLATFORM_MDUV380 PLATFORM_VARIANT_UV380_PLUS_10W.

## Upstream / licence
OpenGD77 by Roger Clark VK3KYY and contributors - see license.txt (BSD-3-clause style, non-commercial use only).
This fork preserves all upstream source and copyright headers.

## AES build (experimental, RX)
`make ENABLE_AES=1` compiles in DMRA (Motorola/Hytera-compatible) AES-256 voice **decryption**. The default
build (ENABLE_AES=0) is byte-for-byte the stock firmware. The decrypt path is wired in HR-C6000.c right after
the AMBE burst is read (before the vocoder); the crypto lives in application/source/crypto/ (AES-256-OFB +
the 32-bit MI to 128-bit IV LFSR, all standalone, host-unit-tested). The exact PI-header trigger and the DMR
voice octet mapping are marked TODO(OTA) pending an over-the-air capture against a stock TYT radio.
Encrypted voice is for licensed commercial/PMR use only; not legal on amateur bands in most countries.

## Flashing - the AMBE codec is merged at flash time (important)
DMR uses the proprietary AMBE+2 vocoder (DVSI-licensed), which OpenGD77 cannot legally distribute. So neither
the source nor ANY built .bin - ours OR the official OpenGD77 releases - contains the codec. The linker
reserves an empty region (.codec_bin_section_1, ~117 KB at 0x08057F50..0x0807537C); this is verified 100%
zero in both our CI build and the official OpenMDUV380_10W_PLUS.bin. The real codec is extracted from official
TYT firmware and merged into that region at flash time.

Therefore the CI artifact is NOT a directly-flashable DMR image on its own (it would be FM-only). It must be
processed by the OpenGD77 firmware loader, exactly like any OpenGD77 source build. Steps
(tools/opengd77_stm32_firmware_loader.py ships in this repo):

1. Obtain an official TYT MD-UV390 firmware .bin (file header "OutSecurityBin") - the AMBE codec source.
2. Put the radio into DFU / bootloader mode.
3. Run:
       python3 MDUV380_firmware/tools/opengd77_stm32_firmware_loader.py          -s <official_TYT_firmware.bin>          -f MDUV380_firmware/build/openuv380-10w.bin          -m <model>      # run with --help to list model tokens (pick the MD-UV390 10W one)
   (-s/--source is remembered after the first run; -L <LANG>.gla adds a secondary language.)

The loader patches the AMBE codec into the reserved region, merges language packs, then downloads via DFU.

AES note: the ENABLE_AES decryption operates on the encrypted AMBE byte stream BEFORE the vocoder, so it is
independent of the codec merge. On a codec-merged AES build the hook decrypts the bytes and the merged codec
then decodes them to audio.
