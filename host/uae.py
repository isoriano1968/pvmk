#!/usr/bin/env python3
"""Run SORIX-LDOM in FS-UAE as an Amiga 3000, serial port on a pseudo-terminal.

  uae.py run  [--cpu 030|040|060] [--adf FILE]
      Interactive console in this terminal. Ctrl-] quits (and stops FS-UAE).
  uae.py test [--cpu 030 040 060] [--adf FILE]
      M1 acceptance test: boot to the shell, exercise commands, warm reset,
      check the previous-boot log; with --hdf also SCSI, partitions, ls and
      the AmigaOS boot request. Exit status 0 if all CPUs pass.
  uae.py test-gdb --gdb GDB [--cpu 030 040 060] [--adf FILE]
      GDB stub test: gdb-multiarch attaches to the stopped pvtest guest,
      sets a breakpoint, continues, steps and detaches; pvtest completes.

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
import socket
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


def to_shell(amiga, timeout):
    """From the boot menu (no default configured) to the shell."""
    amiga.expect(rb"boot menu", timeout)
    amiga.expect(rb"Select: ", 15)
    amiga.send(b"s")
    amiga.expect(re.escape(PROMPT), 15)


class Amiga:
    """FS-UAE process with its serial port attached to a pty we own."""

    def __init__(self, cpu, adf, log_path, hdf=None, hdf_ctrl="scsi",
                 amix_hdf=None, amix_ro=True):
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
            # an AMIX disk: read-only, on SCSI ID 6 as AMIX was installed
            ([f"--hard_drive_1={os.path.abspath(amix_hdf)}",
              "--hard_drive_1_type=rdb", "--hard_drive_1_controller=scsi6",
              f"--hard_drive_1_read_only={int(amix_ro)}"] if amix_hdf else []) +
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


def bridge(amiga, port, until=None, timeout=None, log=None):
    """Connect the serial pty to a TCP port for GDB until the client closes
    (or until() returns true). Returns the bytes seen on the serial side.
    log: file to record the traffic in both directions."""
    trace = open(log, "wb") if log else None
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(1)
    srv.settimeout(timeout)
    cli, _ = srv.accept()
    seen = b""
    try:
        while amiga.proc.poll() is None:
            r, _, _ = select.select([amiga.master, cli], [], [], 0.5)
            if amiga.master in r:
                data = amiga.read(0)
                seen += data
                try:
                    cli.sendall(data)
                except OSError:		# GDB went away (detach, kill)
                    break
                if trace:
                    trace.write(b"\n<< " + data)
            if cli in r:
                data = cli.recv(4096)
                if not data:
                    break
                os.write(amiga.master, data)
                if trace:
                    trace.write(b"\n>> " + data)
            if until and until():
                break
    finally:
        cli.close()
        srv.close()
        if trace:
            trace.close()
    return seen


def cmd_run(args):
    amiga = Amiga(args.cpu, args.adf, "build/serial.log", args.hdf,
                  amix_hdf=args.amix_hdf, amix_ro=not args.amix_rw)
    print(f"uae.py: A3000/{args.cpu}, {args.adf}, serial on {amiga.slave_name}"
          " (log: build/serial.log). Ctrl-] quits.\r", flush=True)
    stdin = sys.stdin.fileno()
    saved = termios.tcgetattr(stdin) if os.isatty(stdin) else None
    srv = None
    if args.gdb_port:
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(("127.0.0.1", args.gdb_port))
        srv.listen(1)
        print(f"uae.py: GDB: type 'gdb' in the shell, then in GDB: "
              f"target remote localhost:{args.gdb_port}\r", flush=True)
    cli = None
    try:
        if saved:
            tty.setraw(stdin)
        while amiga.proc.poll() is None:
            fds = [amiga.master, stdin] + ([srv] if srv and not cli else []) \
                + ([cli] if cli else [])
            r, _, _ = select.select(fds, [], [], 0.5)
            if srv in r:
                cli, _ = srv.accept()
                print("\r\nuae.py: GDB connected (serial goes to GDB)\r",
                      flush=True)
                continue
            if cli and cli in r:
                data = cli.recv(4096)
                if not data:
                    cli.close()
                    cli = None
                    print("\r\nuae.py: GDB disconnected\r", flush=True)
                else:
                    os.write(amiga.master, data)
                continue
            if cli and amiga.master in r:
                cli.sendall(amiga.read(0))
                continue
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
        if cli:
            cli.close()
        if srv:
            srv.close()
        amiga.stop()
        print("\nuae.py: stopped")
    return 0


def test_one(cpu, adf, hdf, ctrl="scsi", amix_hdf=None):
    log = f"build/serial-{cpu}{'' if ctrl == 'scsi' else '-' + ctrl}.log"
    amiga = Amiga(cpu, adf, log, hdf, ctrl, amix_hdf)
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
        amiga.expect(rb"cpu: MMU self-test passed", 10)
        step = "boot menu"
        amiga.expect(rb"boot menu", 15)
        amiga.expect(rb"Select: ", 15)
        if amix_hdf:
            amiga.send(b"k")
            amiga.expect(rb"UNIX_Root:/stand/unix +\d+ bytes", 30)
            amiga.expect(rb"Kernel: ", 10)
            amiga.send(b"b")
            amiga.expect(rb"Select: ", 15)
        amiga.send(b"s")
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
        # virtual CPU: the conformance guest, then break-in and resume
        shell("pvtest", rb"pvtest: done, 0 failed", timeout=60)
        step = "pvtest loop"
        amiga.send(b"pvtest loop\r")
        amiga.expect(rb"done, 0 failed", 60)
        amiga.expect(rb"\.\.\.", 30)
        step = "Ctrl-C"
        amiga.send(b"\x03")
        amiga.expect(rb"stopped \(Ctrl-C\)", 10)
        amiga.expect(re.escape(PROMPT), 10)
        shell("guest", rb"guest pvtest: stopped")
        step = "resume"
        amiga.send(b"resume\r")
        amiga.expect(rb"\.\.\.", 30)
        amiga.send(b"\x03")
        amiga.expect(rb"stopped \(Ctrl-C\)", 10)
        amiga.expect(re.escape(PROMPT), 10)
        step = "reset"
        amiga.send(b"reset\r")
        amiga.expect(rb"BLCJ", 60)
        amiga.expect(rb"previous boot log found", 30)
        to_shell(amiga, 30)
        shell("ver", rb"boot 2,")
        shell("dmesg -p", rb"illegal instruction")
        if hdf:
            dev = {"scsi": rb"scsi\.device", "uae": rb"uaehf\.device"}[ctrl]
            shell("part", rb"DH0 +" + dev + rb" +0 DOS\\1 +4MB +0 +AmigaOS +TestDH0")
            shell("part", rb"UNIX +" + dev + rb" +0 UNI\\1 +1MB")
            if amix_hdf:
                shell("part", rb"UNIX_Root +scsi\.device +6 UNI\\1 +500MB")
                shell("load unix_root:", rb"relocations, entry [0-9a-f]{8}",
                      timeout=120)
                shell("ls unix_root:/stand", rb"-rw-r--r-- +1475144 +unix")
                shell("ls unix_root:/", rb"unix -> stand/unix")
                shell("cat unix_root:/etc/vfstab", rb"c6d0s1\t/\tufs")
            if ctrl == "scsi":
                shell("ls dh0:s", rb"startup-sequence +70")
            else:
                shell("ls dh0:s", rb"which has no native driver")
            if amix_hdf:
                # UNIX_Boot (priority 2) comes before DH0 in strap's order
                shell("boot dh0", rb"Kickstart boots UNIX_Boot first")
            else:
                step = "boot dh0"
                amiga.send(b"boot dh0\r")
                amiga.expect(rb"rebooting; AmigaOS will start from DH0", 10)
                amiga.expect(rb"boot request: AmigaOS from DH0", 60)
                amiga.expect(rb"declining to boot from the floppy", 10)
            # last: AMIX deprivileged takes over SCSI and chip RAM
            if amix_hdf and cpu == "030":
                # AMIX deprivileged (shadow MMU) through the boot menu:
                # nextboot, reset, countdown; up to AMIX's banner
                step = "nextboot"
                shell("nextboot -pv unix_root:/stand/unix", rb"once")
                amiga.send(b"reset\r")
                amiga.expect(rb"menu: nextboot unix_root:/stand/unix", 120)
                amiga.expect(rb"Booting \d+ in", 20)
                step = "boot -pv"
                amiga.expect(rb"starting AMIX deprivileged", 120)
                time.sleep(25)
                amiga.send(b"\x03")
                amiga.expect(rb"amix stopped \(Ctrl-C\)", 20)
                amiga.expect(re.escape(PROMPT), 10)
                shell("guest log", rb"Available Unix  memory = \d+")
                shell("guest", rb"shadow MMU: \d+ fills")
                shell("kill", rb"kill: amix discarded")
                shell("guest", rb"guest amix: exited")
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


def gdb_one(cpu, adf, gdb, elf="build/pvtest.elf", port=2345):
    log = f"build/serial-gdb-{cpu}.log"
    amiga = Amiga(cpu, adf, log)
    step = "boot"
    try:
        to_shell(amiga, 120)
        step = "pvtest stop"
        amiga.send(b"pvtest stop\r")
        amiga.expect(rb"ready, stopped", 20)
        amiga.expect(re.escape(PROMPT), 10)
        step = "gdb"
        amiga.send(b"gdb\r")
        amiga.expect(rb"serial port handed to GDB", 10)
        import threading
        t = threading.Thread(target=bridge, args=(amiga, port), daemon=True,
                             kwargs={"timeout": 30,
                                     "log": f"build/gdb-traffic-{cpu}.log"})
        t.start()
        time.sleep(0.5)
        step = "gdb session"
        g = subprocess.run(
            [gdb, "-nx", "-batch", "-ex", "set remotetimeout 10",
             "-ex", f"file {elf}", "-ex", f"target remote localhost:{port}",
             "-ex", "info registers pc ps", "-ex", "break pass",
             "-ex", "continue", "-ex", "stepi", "-ex", "info registers pc",
             "-ex", "x/xw 0x60000", "-ex", "delete", "-ex", "detach"],
            capture_output=True, text=True, timeout=60)
        out = g.stdout
        for want in (r"pc\s+0x60000\s", r"ps\s+0x2700",
                     r"Breakpoint 1, 0x[0-9a-f]+ in pass ",
                     r"pc\s+0x[0-9a-f]+\s+0x[0-9a-f]+ <pass\+2>",
                     r"0x60000 <_start>:\s+0x4ff90006", r"detached"):
            if not re.search(want, out):
                print(f"uae.py test-gdb: {cpu}: FAIL: no /{want}/ in GDB "
                      f"output (log: {log})")
                print("    | " + (out + g.stderr)[-1500:].replace(
                    "\n", "\n    | "))
                return False
        t.join(10)
        step = "after detach"
        amiga.expect(rb"pvtest: done, 0 failed", 60)
    except (TimeoutError, subprocess.TimeoutExpired) as e:
        tail = (amiga.buf or b"")[-600:].decode("latin-1")
        print(f"uae.py test-gdb: {cpu}: FAIL at '{step}' waiting for {e} "
              f"(log: {log})")
        print("    | " + tail.replace("\r", "").replace("\n", "\n    | "))
        return False
    finally:
        amiga.stop()
    print(f"uae.py test-gdb: {cpu}: PASS (log: {log})")
    return True


def cmd_test_gdb(args):
    ok = all([gdb_one(cpu, args.adf, args.gdb) for cpu in args.cpu])
    return 0 if ok else 1


def cmd_test(args):
    ok = all([test_one(cpu, args.adf, args.hdf, args.hdf_ctrl, args.amix_hdf)
              for cpu in args.cpu])
    return 0 if ok else 1


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--cpu", choices=CPU_OPTS, default="030")
    r.add_argument("--adf", default="build/pvmk.adf")
    r.add_argument("--hdf", help="RDB hard disk image on the internal SCSI")
    r.add_argument("--amix-hdf", help="AMIX disk image (SCSI ID 6)")
    r.add_argument("--gdb-port", type=int,
                   help="TCP port for GDB (target remote localhost:PORT)")
    r.add_argument("--amix-rw", action="store_true",
                   help="attach the AMIX disk writable (use a copy!)")
    t = sub.add_parser("test")
    t.add_argument("--cpu", nargs="+", choices=CPU_OPTS,
                   default=["030", "040", "060"])
    t.add_argument("--adf", default="build/pvmk.adf")
    t.add_argument("--hdf", help="RDB hard disk image on the internal SCSI")
    t.add_argument("--amix-hdf", help="AMIX disk image (read-only, SCSI ID "
                   "6): also test UFS on its UNIX_Root")
    t.add_argument("--hdf-ctrl", choices=["scsi", "uae"], default="scsi",
                   help="scsi: A3000 internal SCSI; uae: UAE's own driver "
                   "(uaehf.device), like accelerator/Zorro controllers")
    g = sub.add_parser("test-gdb")
    g.add_argument("--cpu", nargs="+", choices=CPU_OPTS,
                   default=["030", "040", "060"])
    g.add_argument("--adf", default="build/pvmk.adf")
    g.add_argument("--gdb", default="gdb-multiarch", help="GDB with m68k")
    args = p.parse_args()
    return {"run": cmd_run, "test": cmd_test,
            "test-gdb": cmd_test_gdb}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
