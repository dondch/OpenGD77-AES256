#!/usr/bin/env python3
"""Resolve firmware symbol addresses from the built .elf at runtime.

Hardcoding RAM addresses in host tools does not work on this project: the build is not
reproducible (__TIME__ is compiled in, and adding a source file reorders the link), so
.data/.bss addresses move between builds. A stale address does not fail loudly -- it
returns plausible-looking values from whatever now lives there, which is exactly how a
populated zone reads back as "0 channels".

So look the address up every time, from the same .elf that was flashed.

    from fwsym import sym
    addr = sym("uiDataGlobal")

Set FW_ELF to point elsewhere, or FW_NM if arm-none-eabi-nm is not reachable as given.
On Windows the default runs nm inside WSL, matching how this tree is built."""
import os
import re
import subprocess
import sys

_DEFAULT_ELF = "~/repo/MDUV380_firmware/build/openuv380-10w.elf"
_cache = None


def _nm_output():
    elf = os.environ.get("FW_ELF", _DEFAULT_ELF)
    if sys.platform == "win32":
        cmd = ["wsl", "-d", os.environ.get("FW_WSL_DISTRO", "Ubuntu-24.04"),
               "bash", "-lc",
               'export PATH="$HOME/arm-tc/bin:$PATH"; arm-none-eabi-nm %s' % elf]
    else:
        nm = os.environ.get("FW_NM", "arm-none-eabi-nm")
        cmd = [nm, os.path.expanduser(elf)]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    if r.returncode != 0:
        raise RuntimeError("could not run nm on %s: %s" % (elf, r.stderr.strip()))
    return r.stdout


def _load():
    global _cache
    if _cache is None:
        _cache = {}
        for line in _nm_output().splitlines():
            m = re.match(r"^([0-9a-fA-F]{8})\s+\S\s+(\S+)$", line.strip())
            if m:
                _cache[m.group(2)] = int(m.group(1), 16)
    return _cache


def sym(name):
    """Address of `name`, or raise if the .elf does not define it."""
    table = _load()
    if name not in table:
        raise KeyError("symbol %r not in the firmware .elf -- wrong build, or the "
                       "feature is not compiled in" % name)
    return table[name]


def sym_or_none(name):
    try:
        return sym(name)
    except (KeyError, RuntimeError):
        return None


if __name__ == "__main__":
    for n in (sys.argv[1:] or ["uiDataGlobal", "currentZone", "currentChannelData"]):
        try:
            print("%-24s 0x%08X" % (n, sym(n)))
        except KeyError as exc:
            print("%-24s %s" % (n, exc))
