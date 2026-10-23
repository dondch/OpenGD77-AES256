#!/usr/bin/env python3
"""Drive the REAL on-radio menu SMS path (dmrSmsSend) over USB — CPS command C / 0x97.

Bug #3 A/B harness: the menu path (dmrSmsSend = store_add flash write + burst build +
dmrDataTxLoad) fails for every send after the first, while the raw 0x91 burst path
(dmr_data_tx.py) works repeatedly. 0x97 calls dmrSmsSend() itself from the host, with a
flag to skip the store_add() Sent-folder flash write, so the two halves can be isolated
without pushing radio buttons.

  python3 menu_sms_test.py "text" [--dst 9661] [--private] [--skip-store] [--port COMx]

Reply byte 2 is dmrSmsSend's int8 result: 0 ok, -1 bad text, -2 TX busy, -3 no key,
-4/-5 too long.
"""
import argparse
import sys
import time

import serial
from serial.tools import list_ports

CMD = ord('C')
SUB_MENU_SMS = 0x97


def find_port():
    for p in list_ports.comports():
        if p.vid == 0x1FC9 and p.pid == 0x0094:
            return p.device
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("text", help="SMS text (ASCII, <= 54 chars: single USB packet)")
    ap.add_argument("--dst", type=int, default=9661, help="destination TG / DMR ID (default 9661)")
    ap.add_argument("--private", action="store_true", help="private call addressing (default group)")
    ap.add_argument("--skip-store", action="store_true",
                    help="skip the store_add() Sent-folder flash write inside dmrSmsSend")
    ap.add_argument("--port", default=None, help="serial port (auto-detect 1FC9:0094 if omitted)")
    args = ap.parse_args()

    text = args.text.encode("ascii")
    if len(text) > 54:
        sys.exit("text too long for a single USB packet (<= 54 chars)")

    port = args.port or find_port()
    if port is None:
        sys.exit("radio not found (USB 1FC9:0094). Specify --port. Is it on and in OpenGD77 mode?")

    frame = bytes([CMD, SUB_MENU_SMS,
                   0x01 if args.skip_store else 0x00,
                   args.dst & 0xFF, (args.dst >> 8) & 0xFF,
                   (args.dst >> 16) & 0xFF, (args.dst >> 24) & 0xFF,
                   0x00 if args.private else 0x01]) + text + b"\x00"

    print(f"port: {port}  dst: {args.dst}  group: {not args.private}  "
          f"skip_store: {args.skip_store}  text: {args.text!r}")
    with serial.Serial(port, 115200, timeout=2) as ser:
        ser.write(frame)
        ser.flush()
        time.sleep(0.5)
        reply = ser.read(8)
    print("reply:", reply.hex() if reply else "(none)")
    if len(reply) >= 2 and reply[0] == CMD:
        r = reply[1] if reply[1] < 128 else reply[1] - 256
        names = {0: "OK (keyed TX)", -1: "bad text", -2: "TX busy", -3: "no key",
                 -4: "too long", -5: "too long"}
        print(f"dmrSmsSend result: {r}  ({names.get(r, 'unknown')})")
    else:
        print("no/short 0x97 reply (old firmware without the 0x97 diag?)")


if __name__ == "__main__":
    main()
