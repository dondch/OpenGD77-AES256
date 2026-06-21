#!/usr/bin/env python3
"""Set/show per-channel AES encryption on an OpenGD77-AES radio.

Each codeplug channel has an `encrypt` byte that selects the TX encryption for that
channel (the firmware resolves it at key-up, falling back to the global TX key):
    0      = inherit the global TX key selector (set via aes_key_store.py --tx-key)
    1..15  = encrypt TX on this channel with that AES key slot
    0xFF   = force CLEAR (unencrypted) TX on this channel, overriding the global selector

RX is unaffected (it auto-decrypts any call whose key id is loaded, on any channel).

The per-channel byte is honoured only when the channel does NOT use a per-channel
"optional DMR ID" (that feature repurposes the same byte to hold the DMR ID). If a key
slot has no key loaded, the firmware transmits in the clear (no garble).

Runs over USB CDC (radio enumerates as 1fc9:0094); requires an ENABLE_AES build.

Usage:
  aes_channel_key.py --channel N --slot K     # encrypt channel N with AES key slot K (1..15)
  aes_channel_key.py --channel N --clear      # force clear TX on channel N (0xFF)
  aes_channel_key.py --channel N --inherit    # inherit the global TX key (0)
  aes_channel_key.py --channel N --show       # show channel N's current setting
  aes_channel_key.py --channel N --slot 1 --port COM4
"""
import argparse, sys, time
try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial required: pip install pyserial")

AES_VID, AES_PID = 0x1FC9, 0x0094


def find_port():
    for p in list_ports.comports():
        if (p.vid == AES_VID) and (p.pid == AES_PID):
            return p.device
    return None


def cmd_set(ser, ch, val):
    req = bytes([ord("C"), 0x82, ch & 0xFF, (ch >> 8) & 0xFF, val & 0xFF])
    ser.reset_input_buffer(); ser.write(req); ser.flush(); time.sleep(0.15)
    return ser.read(8)


def cmd_get(ser, ch):
    req = bytes([ord("C"), 0x83, ch & 0xFF, (ch >> 8) & 0xFF])
    ser.reset_input_buffer(); ser.write(req); ser.flush(); time.sleep(0.15)
    r = ser.read(8)
    if len(r) < 3 or r[0] != ord("C"):
        raise RuntimeError("bad reply (is this an ENABLE_AES build?): %r" % r[:8])
    return r[1], r[2]  # encrypt byte, flags (bit0 = optional-DMRID set)


def describe(enc):
    if enc == 0:
        return "inherit global TX key"
    if enc == 0xFF:
        return "force CLEAR (unencrypted)"
    if 1 <= enc <= 15:
        return "AES key slot %d" % enc
    return "0x%02X (out of range -> inherit global)" % enc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=None)
    ap.add_argument("--channel", type=int, required=True, help="channel index (1-based)")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--slot", type=int, help="AES key slot 1..15")
    g.add_argument("--clear", action="store_true", help="force clear TX (0xFF)")
    g.add_argument("--inherit", action="store_true", help="inherit global TX key (0)")
    g.add_argument("--show", action="store_true", help="show current setting only")
    args = ap.parse_args()

    port = args.port or find_port()
    if not port:
        sys.exit("no OpenGD77 CDC port found (1fc9:0094). Pass --port COMx")
    ser = serial.Serial(port, 115200, timeout=0.5)

    if args.show:
        enc, fl = cmd_get(ser, args.channel)
        extra = "  [optional-DMRID set -> per-channel encryption IGNORED here]" if (fl & 1) else ""
        print("channel %d: encrypt=0x%02X (%s)%s" % (args.channel, enc, describe(enc), extra))
        return

    if args.slot is not None:
        if not (1 <= args.slot <= 15):
            sys.exit("--slot must be 1..15")
        val = args.slot
    elif args.clear:
        val = 0xFF
    else:  # --inherit
        val = 0x00

    cmd_set(ser, args.channel, val)
    enc, fl = cmd_get(ser, args.channel)
    print("channel %d set: encrypt=0x%02X (%s)" % (args.channel, enc, describe(enc)))
    if fl & 1:
        print("  WARNING: this channel uses a per-channel optional DMR ID; the encrypt byte is")
        print("  interpreted as the DMR ID, so per-channel encryption will NOT take effect here.")
    if enc != val:
        print("  WARNING: readback (0x%02X) != requested (0x%02X)" % (enc, val))


if __name__ == "__main__":
    main()
