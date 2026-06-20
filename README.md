# OpenGD77-AES256 (TYT MD-UV390 Plus, 10W)

An experimental fork of [OpenGD77](https://opengd77.com) for the **TYT MD-UV390 Plus (10W)** that adds
**TYT-compatible DMRA AES-256 encrypted voice** — interoperable with the stock TYT "Universal" AES256
firmware. Stock OpenGD77 has no encryption; this is a reverse-engineering / interop project, not an
upstream feature.

It is built on the official OpenGD77 source release **R20260131**, target `MDUV380_10W_PLUS_FW`
(`PLATFORM_MDUV380` + `PLATFORM_VARIANT_UV380_PLUS_10W`). All the normal OpenGD77 functionality
(DMR/FM transmit + receive, hotspot, etc.) is unchanged — the AES code is compiled in behind
`ENABLE_AES`, and the default `make` build is byte-for-byte stock OpenGD77.

## ⚠️ Legal

AES-256 encrypted voice is **illegal on amateur radio bands in most countries**; it is only valid on
licensed commercial / PMR allocations. Use this only where you are licensed to transmit encrypted
voice. The source and binaries are also **non-commercial use only** — see [`license.txt`](license.txt).

## What it adds

Implements the standardized **DMR Association AES-256** scheme (Motorola/Hytera-compatible, ETSI TS
102 361) that the MD-UV390 Plus "Universal" mode uses, so this firmware and a stock TYT radio can
encrypt/decrypt each other:

- **AES-256 in OFB mode**, the keystream XORed onto the decoded 49-bit AMBE voice parameters.
- A 32-bit **Message Indicator (MI)** carried in the PI header, expanded to a 128-bit IV by an LFSR
  (`dmr_lfsr128d`) and advanced one step per superframe.
- The MI also conveyed **in-band in the AMBE codeword bits** (Late-Entry MI, Golay(24,12)+CRC4), with
  the call announced over the air via the encrypted-call LC (FID `0x10` / SO `0x40`) and the burst-F
  EMB **Late-Entry Single Block** (alg/key).

### Status — working on hardware

Validated on real MD-UV390 10W Plus radios, cross-checked with HackRF captures + DSD-FME.

- **TX:** a bone-stock TYT MD-UV390 decodes this firmware's encrypted transmissions to clear voice —
  full interoperable signalling (keystream + Late-Entry MI + encrypted-call LC + EMB Single Block +
  PI-Header preamble).
- **RX:** decrypts a stock TYT's AES-256 voice to clear audio, including **rapid back-to-back calls**
  (the receiver locks the new call's MI directly from the in-band Late-Entry, like a stock receiver).
- **Keys:** stored persistently in the SPI-flash custom-data region (survive reboots and firmware
  flashes), loaded via a host tool over the OpenGD77 CPS USB protocol.

## Build & flash

See **[BUILD.md](BUILD.md)** for the full toolchain, the required AMBE codec donor (MD-9600 V5), and
DFU flashing. In short:

```sh
cd MDUV380_firmware
make ENABLE_AES=1 -j$(nproc)        # or plain `make` for a stock (no-AES) build
python3 tools/opengd77_stm32_firmware_loader.py -s <MD9600-V5-donor.bin> \
        -f build/openuv380-10w.bin -m MD-UV380
```

DMR needs the proprietary AMBE+2 vocoder, which is **not** present in this (or any OpenGD77)
source/binary; the loader merges it from an **MD-9600 V5** donor at flash time (see BUILD.md).
**Do not flash a 5W build to a 10W radio.**

### Optional diagnostics

AES decrypt/keystream diagnostics are compiled in behind off-by-default flags (`DMR_AES_DIAG_RX` /
`DMR_AES_DIAG_PATTERN` / `DMR_AES_DIAG_ENCPAT`) — they add nothing to the default build. Build e.g.
`make ENABLE_AES=1 DMR_AES_DIAG_RX=1` and read over USB with `tools/rxdiag.py` / `tools/fragcap.py`.

## License

This fork inherits the OpenGD77 license — **BSD-3-clause style with a non-commercial clause**. The full
text is in [`license.txt`](license.txt); commercial use of the source or binaries is forbidden. All
upstream source files and copyright headers are preserved.

## Credits

OpenGD77 was conceived by Kai DG4KLU and developed by Roger Clark VK3KYY, latterly assisted by Daniel
F1RMB, Alex DL4LEX, Colin G4EML and many others (lead developer / source gatekeeper: Roger VK3KYY) —
see the upstream project for the full contributor list. This fork only adds the DMRA AES-256 layer on
top of their work. The DMRA AES scheme reference implementation is
[DSD-FME](https://github.com/lwvmobile/dsd-fme) by lwvmobile.

User guide (general OpenGD77 operation): https://github.com/LibreDMR/OpenGD77_UserGuide
