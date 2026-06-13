#!/usr/bin/env python3
"""Load a DMRA AES-256 key into an OpenGD77-AES radio over USB (the CPS serial protocol).

Sends a custom CPS command (C / 0x80 / keyId / 32-byte key) which the AES firmware stores
safely in the codeplug custom-data region via codeplugSetOpenGD77CustomData() and reloads.
Requires an ENABLE_AES firmware build. Needs pyserial (pip install -r requirements.txt).

  python3 aes_key_tool.py --key <64-hex-chars> [--keyid 1] [--port /dev/ttyACM0]

NOTE: the exact CPS serial framing/ack is taken from the firmware source; validate against a
real radio before relying on it. The radio enumerates as USB CDC VID:PID 1fc9:0094.
"""
import argparse, sys, time
try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial required: pip install -r requirements.txt")

AES_VID, AES_PID = 0x1FC9, 0x0094
CMD = ord('C')
SUB_SHOW_CPS = 0
SUB_CLOSE    = 5
SUB_SET_AES_KEY = 0x80   # added by the ENABLE_AES firmware (cpsHandleCommand)

def find_port():
    for p in list_ports.comports():
        if (p.vid == AES_VID) and (p.pid == AES_PID):
            return p.device
    return None

def send(ser, payload, read=64, wait=0.1):
    ser.reset_input_buffer()
    ser.write(payload)
    ser.flush()
    time.sleep(wait)
    return ser.read(read)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--key", required=True, help="AES-256 key, 64 hex chars")
    ap.add_argument("--keyid", type=int, default=1, help="key id / slot (default 1)")
    ap.add_argument("--port", default=None, help="serial port (auto-detect 1fc9:0094 if omitted)")
    a = ap.parse_args()

    key = bytes.fromhex(a.key.strip())
    if len(key) != 32:
        sys.exit("key must be 64 hex chars (32 bytes), got %d bytes" % len(key))
    if not (0 <= a.keyid <= 15):
        sys.exit("keyid must be 0..15")

    port = a.port or find_port()
    if not port:
        sys.exit("radio not found (USB 1fc9:0094). Specify --port. Is it connected and in OpenGD77 mode?")
    print("port:", port)

    with serial.Serial(port, 115200, timeout=0.5) as ser:
        send(ser, bytes([CMD, SUB_SHOW_CPS]))                       # show CPS screen
        ack = send(ser, bytes([CMD, SUB_SET_AES_KEY, a.keyid]) + key)  # set key
        print("set-key reply:", ack.hex() if ack else "(none)")
        send(ser, bytes([CMD, SUB_CLOSE]))                          # close CPS
    print("done. Key %d written; verify on the radio / by an encrypted RX test." % a.keyid)

if __name__ == "__main__":
    main()
