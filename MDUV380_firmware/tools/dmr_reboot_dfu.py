#!/usr/bin/env python3
"""Reboot an OpenGD77-AES radio into the STM32 ROM DFU bootloader over USB — no buttons.

Sends CPS command C / 0x90, which the ENABLE_DMR_DATA firmware (cpsHandleCommand)
defers ~500 ms and then jumps to the STM32F405 system bootloader. The radio drops off
USB and re-enumerates as DFU (VID:PID 0483:DF11), ready for the firmware loader — so a
flash cycle no longer needs the PTT+power-on button dance.

  python3 dmr_reboot_dfu.py [--port COMx]

Requires an ENABLE_DMR_DATA firmware build + pyserial. The radio normally enumerates as
USB CDC VID:PID 1FC9:0094. Recoverable: if it doesn't reach DFU, a normal power-cycle
returns to the app (the jump erases nothing).
"""
import argparse, sys, time
try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial required: pip install -r requirements.txt")

APP_VID, APP_PID = 0x1FC9, 0x0094
CMD = ord('C')
SUB_REBOOT_DFU = 0x90

def find_port():
    for p in list_ports.comports():
        if (p.vid == APP_VID) and (p.pid == APP_PID):
            return p.device
    return None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=None, help="serial port (auto-detect 1FC9:0094 if omitted)")
    a = ap.parse_args()

    port = a.port or find_port()
    if not port:
        sys.exit("radio not found (USB 1FC9:0094). Specify --port. Is it on and in OpenGD77 mode?")
    print("port:", port)

    try:
        with serial.Serial(port, 115200, timeout=0.5) as ser:
            ser.reset_input_buffer()
            ser.write(bytes([CMD, SUB_REBOOT_DFU]))
            ser.flush()
            time.sleep(0.1)
            ack = ser.read(8)
            print("reply:", ack.hex() if ack else "(none)")
    except serial.SerialException as e:
        # The port may vanish as the radio reboots; that's expected/benign.
        print("serial closed (radio likely rebooting):", e)

    print("sent reboot-to-DFU. Radio should re-enumerate as DFU (0483:DF11) within ~1-2 s.")

if __name__ == "__main__":
    main()
