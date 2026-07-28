#!/usr/bin/env python3
"""Resolve firmware symbol addresses from the .elf that is actually on the radio.

Hardcoding RAM addresses in host tools does not work on this project: the build is not
reproducible (__TIME__ is compiled in, and adding a source file reorders the link), so
.data/.bss addresses move between builds. A stale address does not fail loudly -- it
returns plausible-looking values from whatever now lives there, which is exactly how a
populated zone reads back as "0 channels".

    from fwsym import sym
    addr = sym("uiDataGlobal")

★ Looking the address up at runtime is necessary but NOT sufficient, and this bit up:
build/openuv380-10w.elf is whatever was built LAST, which is not necessarily what was
flashed. Verifying a stock build for its size triple, right after flashing a dev build,
leaves an .elf that disagrees with the radio. Symbols present in both builds then resolve
to the wrong address and return plausible garbage -- screenOperationMode read 0 (NORMAL)
while the radio was visibly sweeping.

Two defences, in order of how much you should rely on them:

  1. VERIFY. `assertMatchesRadio(readmem)` reads a block of code from the radio and
     compares it against the same address in the .elf. Any host tool that reads firmware
     memory should call it before trusting an address. This catches a stale .elf however
     it got that way, including the case where nobody remembered to stamp anything.

  2. STAMP. `python fwsym.py --stamp` copies the current build to build/flashed.elf, and
     that copy is preferred over build/openuv380-10w.elf from then on. Run it straight
     after a successful flash. This is a convenience so a later stock build cannot clobber
     the reference -- it is not a substitute for (1), because nothing forces you to run it.

FW_ELF overrides the path outright. FW_NM / FW_OBJDUMP override the tools. On Windows the
default runs the toolchain inside WSL, matching how this tree is built.
"""
import os
import re
import subprocess
import sys

_FW_DIR = "~/repo/MDUV380_firmware"
_BUILT_ELF = _FW_DIR + "/build/openuv380-10w.elf"
# Deliberately NOT under build/: `make clean` is `rm -rf $(BUILD)`, so a stamp kept there
# is destroyed by the very stock-build-and-clean cycle it exists to survive. (*.elf is
# already gitignored, so this does not need adding.)
_FLASHED_ELF = _FW_DIR + "/.flashed.elf"

# Verification probes: how many places across .text to compare, and how much at each.
#
# One block is not enough, and the obvious choice is actively misleading: main() sits near
# the start of .text and is byte-identical between a stock and a dev build, so comparing it
# passes while every later address is wrong. That false pass was the first version of this
# check. Spread the probes across the whole section instead -- two builds that differ at
# all will differ somewhere in it, and the tail is where a size difference guarantees it.
_VERIFY_PROBES = 6
_VERIFY_BYTES = 32

_cache = None
_elfPath = None
_verified = False


def _run(argv_posix, elf=None):
    """Run a toolchain command, inside WSL when we are on Windows."""
    if sys.platform == "win32":
        cmd = ["wsl", "-d", os.environ.get("FW_WSL_DISTRO", "Ubuntu-24.04"), "bash", "-lc",
               'export PATH="$HOME/arm-tc/bin:$PATH"; ' + argv_posix]
    else:
        cmd = ["bash", "-lc", argv_posix]
    return subprocess.run(cmd, capture_output=True, text=True, timeout=120)


def _exists(path):
    r = _run("test -f %s && echo yes" % path)
    return r.stdout.strip() == "yes"


def elfPath():
    """The .elf to resolve against: an explicit override, else the stamped copy of what
    was flashed, else whatever was built last (and hope)."""
    global _elfPath
    if _elfPath is None:
        override = os.environ.get("FW_ELF")
        if override:
            _elfPath = override
        elif _exists(_FLASHED_ELF):
            _elfPath = _FLASHED_ELF
        else:
            _elfPath = _BUILT_ELF
    return _elfPath


def _load():
    global _cache
    if _cache is None:
        nm = os.environ.get("FW_NM", "arm-none-eabi-nm")
        r = _run("%s %s" % (nm, elfPath()))
        if r.returncode != 0:
            raise RuntimeError("could not run nm on %s: %s" % (elfPath(), r.stderr.strip()))
        _cache = {}
        for line in r.stdout.splitlines():
            m = re.match(r"^([0-9a-fA-F]{8})\s+\S\s+(\S+)$", line.strip())
            if m:
                _cache[m.group(2)] = int(m.group(1), 16)
    return _cache


def sym(name):
    """Address of `name`, or raise if the .elf does not define it."""
    table = _load()
    if name not in table:
        raise KeyError("symbol %r not in %s -- wrong build, or the feature is not "
                       "compiled in" % (name, elfPath()))
    return table[name]


def symOrNone(name):
    try:
        return sym(name)
    except (KeyError, RuntimeError):
        return None


# Kept for callers written against the old name.
sym_or_none = symOrNone


def elfBytes(addr, length):
    """`length` bytes of the .elf image at virtual address `addr`."""
    objdump = os.environ.get("FW_OBJDUMP", "arm-none-eabi-objdump")
    r = _run("%s -s --start-address=0x%X --stop-address=0x%X %s"
             % (objdump, addr, addr + length, elfPath()))
    if r.returncode != 0:
        raise RuntimeError("could not run objdump on %s: %s" % (elfPath(), r.stderr.strip()))

    # Lines look like: " 0800c000 12345678 9abcdef0 ...  ........"
    out = bytearray()
    for line in r.stdout.splitlines():
        m = re.match(r"^\s*([0-9a-fA-F]{4,8})\s+((?:[0-9a-fA-F]{2,8}\s+)+)", line)
        if not m:
            continue
        for word in m.group(2).split():
            if re.fullmatch(r"[0-9a-fA-F]+", word) and (len(word) % 2 == 0):
                out += bytes.fromhex(word)
    return bytes(out[:length])


def textRange():
    """(address, size) of the .text section in the .elf."""
    readelf = os.environ.get("FW_READELF", "arm-none-eabi-readelf")
    r = _run("%s -S %s" % (readelf, elfPath()))
    if r.returncode != 0:
        raise RuntimeError("could not run readelf on %s: %s" % (elfPath(), r.stderr.strip()))
    for line in r.stdout.splitlines():
        m = re.search(r"\.text\s+PROGBITS\s+([0-9a-fA-F]+)\s+[0-9a-fA-F]+\s+([0-9a-fA-F]+)",
                      line)
        if m:
            return int(m.group(1), 16), int(m.group(2), 16)
    raise RuntimeError("no .text section in %s" % elfPath())


def verifyAgainstRadio(readmem, probes=_VERIFY_PROBES, length=_VERIFY_BYTES):
    """Compare code on the radio with the same addresses in the .elf.

    `readmem(addr, n)` must return n bytes of the radio's address space -- over CPS that
    is area 5, which adds 0x08000000 to whatever it is given, so callers pass
    `addr - 0x08000000`. Returns (ok, detail).
    """
    base, size = textRange()
    if size < (probes * length):
        probes, length = 1, min(length, size)

    # Evenly spaced, with the last probe hard against the end of .text: that is where two
    # builds of different sizes are guaranteed to disagree.
    step = (size - length) // max(probes - 1, 1)

    for i in range(probes):
        addr = base + (i * step)
        want = elfBytes(addr, length)
        got = bytes(readmem(addr, length))

        if len(want) < length:
            return False, ("could only read %d of %d bytes from %s at 0x%08X"
                           % (len(want), length, elfPath(), addr))
        if len(got) < length:
            return False, ("radio returned %d of %d bytes at 0x%08X -- is it in normal "
                           "mode, not DFU?" % (len(got), length, addr))
        if want != got:
            return False, (
                "%s does NOT match the firmware on the radio.\n"
                "  differs at 0x%08X (probe %d of %d, %d bytes into .text)\n"
                "    elf:   %s\n"
                "    radio: %s\n"
                "  Every address from this .elf is therefore suspect, and reads will\n"
                "  return plausible garbage rather than failing. Rebuild the image that\n"
                "  is actually flashed, or reflash, then: python fwsym.py --stamp"
                % (elfPath(), addr, i + 1, probes, addr - base,
                   want[:16].hex(), got[:16].hex()))

    return True, ("%s matches the radio (%d probes x %d bytes across .text)"
                  % (elfPath(), probes, length))


def assertMatchesRadio(readmem, **kwargs):
    """verifyAgainstRadio(), but raise on mismatch. Checks once per process."""
    global _verified
    if _verified:
        return
    ok, detail = verifyAgainstRadio(readmem, **kwargs)
    if not ok:
        raise RuntimeError(detail)
    _verified = True


def stampFlashed():
    """Record the current build as the one on the radio. Run right after flashing."""
    r = _run("cp %s %s && echo ok" % (_BUILT_ELF, _FLASHED_ELF))
    if r.stdout.strip() != "ok":
        raise RuntimeError("could not stamp %s: %s" % (_FLASHED_ELF, r.stderr.strip()))
    return _FLASHED_ELF


def _cpsReader(ser):
    import struct

    def readmem(addr, n):
        # CPS 'R' area 5 computes (uint8_t*)address + 0x08000000.
        ser.reset_input_buffer()
        ser.write(struct.pack(">BBIH", ord("R"), 5, (addr - 0x08000000) & 0xFFFFFFFF, n))
        ser.flush()
        return ser.read(n + 3)[3:3 + n]

    return readmem


def _openRadio():
    import serial
    from serial.tools import list_ports
    for p in list_ports.comports():
        if (p.vid == 0x1FC9) and (p.pid == 0x0094):
            return serial.Serial(p.device, 115200, timeout=2.0)
    sys.exit("radio not found (USB 1FC9:0094) -- is it in normal mode, not DFU?")


def main():
    args = sys.argv[1:]

    if "--stamp" in args:
        print("stamped %s" % stampFlashed())
        return 0

    if "--check" in args:
        with _openRadio() as ser:
            ok, detail = verifyAgainstRadio(_cpsReader(ser))
        print(detail)
        return 0 if ok else 1

    for n in (args or ["uiDataGlobal", "currentZone", "currentChannelData"]):
        try:
            print("%-24s 0x%08X" % (n, sym(n)))
        except KeyError as exc:
            print("%-24s %s" % (n, exc))
    return 0


if __name__ == "__main__":
    sys.exit(main())
