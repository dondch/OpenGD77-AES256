#!/usr/bin/env python3
"""Drive the radio into VFO sweep mode, verifying each step from firmware RAM.

Injected keypresses are not reliable right after a flash -- the first one often lands
while the splash screen is still up and is swallowed. Rather than sleep-and-hope, this
reads screenOperationMode out of RAM (resolved from the flashed .elf, never hardcoded)
and retries until the radio is actually where it should be.

screenOperationMode: 0 = NORMAL, 1 = SCAN, 2 = DUAL_SCAN, 3 = SWEEP. It is VFO-only, so
it reads NORMAL in channel mode too -- the frame buffer is what distinguishes those.
"""
import struct
import sys
import time

sys.path.insert(0, "/home/dondch/repo/MDUV380_firmware/tools")
sys.path.insert(0, r"\\wsl.localhost\Ubuntu-24.04\home\dondch\repo\MDUV380_firmware\tools")
sys.path.insert(0, r"C:\Users\ddona\OneDrive\Desktop\HAM_Radio\TYT UV390Plus\chirp-opengd77-aes")

import serial
from fwsym import sym
import fbmirror

NORMAL, SCAN, DUAL_SCAN, SWEEP = 0, 1, 2, 3
KEY_RED, KEY_HASH = 27, ord("#")
FLAG_LONG_HOLD = 0x03          # bit0 = long, bit1 = hold (no release)


def readmem(ser, addr, n):
    # CPS 'R' area 5 adds 0x08000000 to the address it is given, so subtract it first.
    ser.reset_input_buffer()
    ser.write(struct.pack(">BBIH", ord("R"), 5, (addr - 0x08000000) & 0xFFFFFFFF, n))
    ser.flush()
    return ser.read(n + 3)[3:3 + n]


def key(ser, code, flags=0):
    ser.reset_input_buffer()
    ser.write(bytes([ord("C"), 0x96, code & 0xFF, flags]))
    ser.flush()
    ser.read(8)


def op_mode(ser):
    # currentVFONumber picks the element; VFO A is 0 and that is what we drive.
    return readmem(ser, sym("screenOperationMode"), 2)[0]


def in_channel_mode(ser):
    """Channel mode draws "Ch:" where VFO draws the Rx/Tx frequency pair."""
    fb = fbmirror.read_fb(ser)
    rgb = fbmirror.fb_to_rgb(fb)
    # Count ink below the RSSI bar. VFO shows two frequency lines there, channel shows
    # a single short "Ch:n". Crude, but it only has to separate two known screens.
    ink = 0
    for y in range(88, 120):
        for x in range(0, 160):
            if rgb[(y * 160 + x) * 3] < 128:
                ink += 1
    return ink < 200


def main():
    port = fbmirror.find_port()
    with serial.Serial(port, 115200, timeout=2.0) as ser:
        for attempt in range(8):
            mode = op_mode(ser)
            if mode == SWEEP:
                print("in sweep (screenOperationMode=%d)" % mode)
                return 0

            if in_channel_mode(ser):
                print("attempt %d: channel mode -> RED" % attempt)
                key(ser, KEY_RED)
                time.sleep(1.2)
                continue

            print("attempt %d: VFO, screenOperationMode=%d -> long-# (hold)" % (attempt, mode))
            key(ser, KEY_HASH, FLAG_LONG_HOLD)
            time.sleep(3.0)

        print("gave up; screenOperationMode=%d" % op_mode(ser))
        return 1


if __name__ == "__main__":
    sys.exit(main())
