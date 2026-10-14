#!/usr/bin/env python3
"""Run SORIX-LDOM in FS-UAE as an Amiga 3000, serial port on a pseudo-terminal.

  uae.py run  [--cpu 030|040|060] [--adf FILE]
      Interactive console in this terminal. Ctrl-] quits (and stops FS-UAE).
  uae.py test [--cpu 030 040 060] [--adf FILE]
      M1 acceptance test: boot to the shell, exercise commands, warm reset,
      check the previous-boot log; with --hdf also SCSI, partitions, ls and
      the AmigaOS boot request. Exit status 0 if all CPUs pass.

Environment: KICKSTART (A3000 ROM; default
~/Documents/FS-UAE/Kickstarts/A3000.47.111.rom), UAE_EXTRA (more FS-UAE
options, e.g. --zorro_iii_memory=65536 for accelerator-like RAM).
Serial output is also written to build/serial[-CPU].log.
"""
import argparse
import os
import pty
import re
import select
import subprocess
import sys
import termios
import time
import tty

HERE = os.path.dirname(os.path.abspath(__file__))
CPU_OPTS = {
    "030": ["--cpu=68030", "--fpu=68882", "--uae_mmu_model=68030"],
    "040": ["--cpu=68040", "--fpu=68040", "--uae_mmu_model=68040"],
    "060": ["--cpu=68060", "--fpu=68060", "--uae_mmu_model=68060"],
}
PROMPT = b"SORIX-LDOM> "


class Amiga:
    """FS-UAE process with its serial port attached to a pty we own."""

    def __init__(self, cpu, adf, log_path, hdf=None, hdf_ctrl="scsi"):
        kick = os.environ.get("KICKSTART", os.path.expanduser(
            "~/Documents/FS-UAE/Kickstarts/A3000.47.111.rom"))
        if not os.path.isfile(kick):
            sys.exit(f"uae.py: Kickstart ROM not found: {kick} (set KICKSTART)")
        if not os.path.isfile(adf):
            sys.exit(f"uae.py: no image: {adf} (run make)")
        self.master, slave = pty.openpty()
        tty.setraw(slave)
        self.slave_name = os.ttyname(slave)
        os.makedirs(os.path.dirname(log_path) or ".", exist_ok=True)
        self.log = open(log_path, "wb")
        self.uae_log = open(log_path[:-4] + "-uae.log" if log_path.endswith(
            ".log") else log_path + ".uae", "wb")
        self.proc = subprocess.Popen(
            ["fs-uae", os.path.join(HERE, "uae", "a3000.fs-uae"),
             f"--kickstart_file={kick}",
             f"--floppy_drive_0={os.path.abspath(adf)}",
             f"--serial_port={self.slave_name}"] + CPU_OPTS[cpu] +
            ([f"--hard_drive_0={os.path.abspath(hdf)}",
              "--hard_drive_0_type=rdb", f"--hard_drive_0_controller={hdf_ctrl}"]
             if hdf else []) +
            os.environ.get("UAE_EXTRA", "").split(),
            stdout=self.uae_log, stderr=subprocess.STDOUT)
        self.slave = slave
        self.buf = b""

    def read(self, timeout):
        r, _, _ = select.select([self.master], [], [], timeout)
        if not r:
            return b""
        try:
            data = os.read(self.master, 4096)
        except OSError:
            return b""
        self.log.write(data)
        self.log.flush()
        return data

    def expect(self, pattern, timeout):
        """Wait until regex pattern (bytes) appears; consume through it."""
        rx = re.compile(pattern)
        end = time.time() + timeout
        while True:
            m = rx.search(self.buf)
            if m:
                out, self.buf = self.buf[:m.end()], self.buf[m.end():]
                return out
            if time.time() > end or self.proc.poll() is not None:
                raise TimeoutError(pattern)
            self.buf += self.read(0.2)

    def send(self, data, delay=0.004):
        """Send slowly: Paula has a one-byte receive buffer."""
        for b in data:
            os.write(self.master, bytes([b]))
            time.sleep(delay)

    def stop(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        self.log.close()
        self.uae_log.close()


def cmd_run(args):
    amiga = Amiga(args.cpu, args.adf, "build/serial.log", args.hdf)
    print(f"uae.py: A3000/{args.cpu}, {args.adf}, serial on {amiga.slave_name}"
          " (log: build/serial.log). Ctrl-] quits.\r", flush=True)
    stdin = sys.stdin.fileno()
    saved = termios.tcgetattr(stdin) if os.isatty(stdin) else None
    try:
        if saved:
            tty.setraw(stdin)
        while amiga.proc.poll() is None:
            r, _, _ = select.select([amiga.master, stdin], [], [], 0.5)
            if amiga.master in r:
                data = amiga.read(0)
                os.write(sys.stdout.fileno(), data)
            if stdin in r:
                data = os.read(stdin, 64)
                if not data or b"\x1d" in data:
                    break
                amiga.send(data, delay=0.002)
    finally:
        if saved:
            termios.tcsetattr(stdin, termios.TCSADRAIN, saved)
        amiga.stop()
        print("\nuae.py: stopped")
    return 0


def test_one(cpu, adf, hdf, ctrl="scsi"):
    log = f"build/serial-{cpu}{'' if ctrl == 'scsi' else '-' + ctrl}.log"
    amiga = Amiga(cpu, adf, log, hdf, ctrl)
    step = "boot"

    def shell(command, expect, timeout=10):
        nonlocal step
        step = command
        amiga.send(command.encode() + b"\r")
        out = amiga.expect(expect, timeout)
        amiga.expect(re.escape(PROMPT), timeout)
        return out

    try:
        amiga.expect(rb"BLCJ", 60)
        amiga.expect(rb"takeover complete", 30)
        amiga.expect(rb"cpu: 68" + cpu.encode() + rb"[ ,]", 10)
        amiga.expect(re.escape(PROMPT), 15)
        shell("ver", rb"SORIX-LDOM \S+, boot 1,")
        shell("help", rb"sload")
        shell("conf", rb"from DF0:pvmk\.conf[\s\S]*baud=115200")
        shell("ls", rb"pvmk\.conf")
        shell("mem", rb"boot log")
        shell("mw 2000 deadbeef", rb"")
        shell("md 2000 4", rb"00002000: de ad be ef")
        step = "sload"
        amiga.send(b"sload 3000 #4\r")
        amiga.expect(rb"send 4 bytes", 5)
        amiga.send(b"\x70\x2a\x4e\x75", delay=0.02)  # moveq #42,d0; rts
        amiga.expect(rb"crc32 [0-9a-f]{8}", 15)
        amiga.expect(re.escape(PROMPT), 5)
        shell("go 3000", rb"returned 0000002a")
        shell("mw.w 3000 4afc", rb"")
        shell("go 3000", rb"illegal instruction \(vector 4")
        shell("regs", rb"illegal instruction")
        shell("dmesg", rb"takeover complete")
        step = "reset"
        amiga.send(b"reset\r")
        amiga.expect(rb"BLCJ", 60)
        amiga.expect(rb"previous boot log found", 30)
        amiga.expect(re.escape(PROMPT), 30)
        shell("ver", rb"boot 2,")
        shell("dmesg -p", rb"illegal instruction")
        if hdf:
            dev = {"scsi": rb"scsi\.device", "uae": rb"uaehf\.device"}[ctrl]
            shell("part", rb"DH0 +" + dev + rb" +0 DOS\\1 +4MB +0 +AmigaOS +TestDH0")
            shell("part", rb"UNIX +" + dev + rb" +0 UNI\\1 +1MB")
            if ctrl == "scsi":
                shell("ls dh0:s", rb"startup-sequence +70")
            else:
                shell("ls dh0:s", rb"can only read disks on the A3000")
            step = "boot dh0"
            amiga.send(b"boot dh0\r")
            amiga.expect(rb"rebooting; AmigaOS will start from DH0", 10)
            amiga.expect(rb"boot request: AmigaOS from DH0", 60)
            amiga.expect(rb"declining to boot from the floppy", 10)
    except TimeoutError as e:
        tail = (amiga.buf or b"")[-600:].decode("latin-1")
        print(f"uae.py test: {cpu} {ctrl}: FAIL at '{step}' waiting for {e} "
              f"(log: {log})")
        print("    | " + tail.replace("\r", "").replace("\n", "\n    | "))
        return False
    finally:
        amiga.stop()
    print(f"uae.py test: {cpu}{'' if ctrl == 'scsi' else ' ' + ctrl}: PASS "
          f"(log: {log})")
    return True


def cmd_test(args):
    ok = all([test_one(cpu, args.adf, args.hdf, args.hdf_ctrl)
              for cpu in args.cpu])
    return 0 if ok else 1


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--cpu", choices=CPU_OPTS, default="030")
    r.add_argument("--adf", default="build/pvmk.adf")
    r.add_argument("--hdf", help="RDB hard disk image on the internal SCSI")
    t = sub.add_parser("test")
    t.add_argument("--cpu", nargs="+", choices=CPU_OPTS,
                   default=["030", "040", "060"])
    t.add_argument("--adf", default="build/pvmk.adf")
    t.add_argument("--hdf", help="RDB hard disk image on the internal SCSI")
    t.add_argument("--hdf-ctrl", choices=["scsi", "uae"], default="scsi",
                   help="scsi: A3000 internal SCSI; uae: UAE's own driver "
                   "(uaehf.device), like accelerator/Zorro controllers")
    args = p.parse_args()
    return cmd_run(args) if args.cmd == "run" else cmd_test(args)


if __name__ == "__main__":
    sys.exit(main())
