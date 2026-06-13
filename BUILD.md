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
