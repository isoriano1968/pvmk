#!/usr/bin/env python3
"""Reject instructions that pvmk must not contain.

pvmk runs on 68030, 68040 and 68060 before any software support package is
installed, so it must avoid what the 68060 does not implement in hardware:
64-bit MULx.L/DIVx.L, MOVEP, CAS2, CHK2/CMP2 - and any FPU instruction
(pvmk is built soft-float; the FPU belongs to the guest).

usage: check-insns.py OBJDUMP file.elf|file.o ...
"""
import re
import subprocess
import sys

SYMBOL = re.compile(r"^[0-9a-f]+ <([^>]+)>:$")
# Functions that deliberately execute such instructions under a trap guard.
# fpu_save/fpu_restore: FSAVE/FRESTORE on behalf of a guest (its FPU state).
ALLOWED = {"probe_fpu", "fpu_save", "fpu_restore"}
LINE = re.compile(r"^\s*([0-9a-f]+):\s+((?:[0-9a-f]{4} )+)\s*(\S+)\s*(.*)$")
BANNED_PREFIX = ("movep", "cas2", "chk2", "cmp2")


def check(objdump, path):
    out = subprocess.run([objdump, "-d", path], capture_output=True, text=True,
                         check=True).stdout
    bad = []
    sym = None
    for line in out.splitlines():
        s = SYMBOL.match(line)
        if s:
            sym = s.group(1)
            continue
        if sym in ALLOWED:
            continue
        m = LINE.match(line)
        if not m:
            continue
        addr, words, mnem, ops = m.groups()
        words = words.split()
        op = int(words[0], 16)
        reason = None
        if mnem.startswith(BANNED_PREFIX):
            reason = "not implemented on 68060"
        elif (op & 0xff80) == 0x4c00 and len(words) > 1:
            # MULU/MULS.L (0x4c00-0x4c3f) and DIVU/DIVS.L (0x4c40-0x4c7f):
            # extension word bit 10 selects the 64-bit form.
            if int(words[1], 16) & 0x0400:
                reason = "64-bit MUL/DIV not implemented on 68060"
        elif (op & 0xf000) == 0xf000 and mnem.startswith("f"):
            reason = "FPU instruction in soft-float pvmk"
        if reason:
            bad.append(f"{path}:{addr}: {mnem} {ops}  <- {reason}")
    return bad


def main():
    if len(sys.argv) < 3:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    bad = []
    for path in sys.argv[2:]:
        bad += check(sys.argv[1], path)
    for b in bad:
        print(b, file=sys.stderr)
    if bad:
        return 1
    print("check-insns: OK (" + ", ".join(sys.argv[2:]) + ")")
    return 0


if __name__ == "__main__":
    sys.exit(main())
