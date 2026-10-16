# SORIX-LDOM — paravirtualizing microkernel for the Amiga 3000

Working/source name: `pvmk`.

A small supervisor-mode microkernel for the Amiga 3000 (68030, 68040, 68060)
that boots from floppy (`pvmk.adf`) or an RDB hard disk partition, offers a
serial shell, and loads *personalities* — AMIX (SVR4) and AmigaOS — from hard
disk partitions, running them deprivileged with a paravirtual interface.
Boot and personality loading are traced over the serial port; a GDB remote
stub is included.

See [PLAN.md](PLAN.md) for the architecture and milestones.

## Building

Requirements: a host C compiler, GNU make, Python 3, and an m68k cross
toolchain. Without root, `make toolchain` unpacks GCC 14 + binutils +
gdb-multiarch from the host's Debian/Ubuntu apt repositories into
`~/opt/m68k-cross`. (With root: `apt install gcc-m68k-linux-gnu` and
`make CROSS=m68k-linux-gnu-`.)

```sh
make            # build/pvmk.adf, verified
make check      # 68060-safe instruction check, size budget, ADF verify
make test       # FS-UAE A3000 with 68030/040/060: boot, shell commands, reset, logs,
                #   and the GDB stub (if gdb-multiarch is found; GDB=...)
make run        # boot in FS-UAE with the serial console in this terminal (Ctrl-] quits)
make run UAE_CPU=060
make run UAE_HDF=                # without the test hard disk
make run-amix                    # with a writable copy of your AMIX disk:
                                 #   SORIX-LDOM> boot unix_root:
host/pvterm /dev/ttyUSB0          # real A3000, 115200 8N1
```

`make run`/`make test` need FS-UAE and an A3000 Kickstart ROM (`KICKSTART=`,
default `~/Documents/FS-UAE/Kickstarts/A3000.47.111.rom`). The emulator's
serial port is a pseudo-terminal driven by `host/uae.py`; the session is also
written to `build/serial.log`.

### Writing the boot floppy

`build/pvmk.adf` is a standard 880 KB Amiga DD image (FFS volume `PVMK`).
A PC floppy drive cannot write the Amiga format; use a Greaseweazle
(`gw write --format amiga.amigados build/pvmk.adf`), KryoFlux or SuperCard
Pro, write it from the Amiga with an ADF tool, or copy it to a Gotek.

## Boot sequence

1. Kickstart runs the boot block from DF0:. It loads the `pvmk` image with
   one read, checks its CRC and jumps to it (serial: `BLCJ`).
2. stage0 (Kickstart still alive) captures the memory list, expansion
   boards, Kickstart version and CPU flags, reserves 256 KB at the top of
   fast RAM, starts the boot log there, reads `DF0:pvmk.conf` and the
   floppy directory, and stops the drive motor.
3. Takeover: interrupts and DMA off, MMU off, data cache off; the image
   moves itself to the reserved memory and re-applies its relocations.
4. stage1 installs its own exception vectors, starts the text console on
   the Amiga display (80 columns, topaz 8 copied from Kickstart at stage0)
   and replays the log so far onto it, probes the CPU (68030, 68040, 68060
   incl. EC/LC and FPU state) and starts the shell.

All console output goes to the serial port and the screen; input comes
from the serial port or the Amiga keyboard (cursor up/down for history).
In `pvmk.conf`, `display=off` disables the screen and `keymap=` selects the
keyboard layout: `us`, `de` (Amiga German keyboard) or `de-pc` (German PC
keyboard under FS-UAE/WinUAE).

Hold the left mouse button at power-on to skip SORIX-LDOM and boot
AmigaOS from the same disk.

## Shell

```
SORIX-LDOM> help
  ver  cpu  mem  conf            system information
  scsi                           rescan the A3000 internal SCSI bus
  part                           list RDB partitions (all disks, all controllers)
  ls [dh0:path]                  floppy directory, or an AmigaDOS partition's
                                 (partitions on the A3000 internal SCSI)
  md <addr> [len]                memory dump
  mw[.b|.w|.l] <addr> <value>    memory write
  sload <addr> <len>             receive raw bytes over serial
  go <addr>                      call code, print D0
  regs                           registers of the last exception
  dmesg [-p]                     boot log, or the previous boot's
  boot <partition>               AmigaOS partition: reboot into it (below)
  load unix_root:[path]          load and relocate an AMIX kernel (/stand/unix)
  boot unix_root:[path]          ... and start it natively (AMIX gets the machine)
  pvtest [loop]                  run the virtual CPU test guest, deprivileged
  guest                          state of the stopped guest (virtual registers)
  resume                         continue the stopped guest
  Ctrl-C (serial)                while a guest runs: stop it, enter the shell
  reset                          reboot into Kickstart
```

`boot dh0` leaves a one-shot request in the RAM that survives a reset and
reboots. stage0 picks it up while Kickstart is alive, frees its memory and
makes the floppy's boot block decline; strap then goes on with the next
boot device in priority order, so Kickstart boots DH0 its normal way even
with the SORIX-LDOM floppy in DF0. Strap keeps its own order at that point,
so only the highest-priority bootable device after the floppy can be chosen
this way; `boot` says so for any other partition.

Numbers are hexadecimal; prefix `#` for decimal. Faults (bus error,
illegal instruction, ...) print a register dump and return to the shell.
The boot log survives a warm reset: `dmesg -p` shows the previous boot.

## Status

- M0 (toolchain, boot floppy, serial output): done.
- M1 boot monitor, floppy part: done and tested in FS-UAE on 68030, 68040
  and 68060. Not yet tried on real hardware.
- Disks and partitions: stage0 reads the RDB of every disk through
  Kickstart's own drivers (whatever controller Kickstart boots from:
  A3000 SCSI, accelerator or Zorro controllers, emulated HDFs), so `part`
  and `boot` work for all of them. After takeover, a native driver for the
  A3000 internal SCSI (WD33C93A/SDMAC, polled, DMA through a chip RAM
  bounce buffer) also gives `ls`. Tested in FS-UAE on the internal SCSI and
  on UAE's own controller (`make test`, `build/test.hdf` from
  `host/mkhdf`).
- Native Z3660 disk driver (piscsi mailbox), next to the A3000 SCSI driver:
  written, not yet tested on real hardware.
- M2 (in progress): read-only UFS reader; AMIX kernel loaded from its UFS
  root (`boot unix_root:` = `/stand/unix`, any path accepted), relocated and
  started natively the way AMIX's own boot2/copyit do. AMIX 2.1 boots to
  multi-user login in FS-UAE (68030) from a real AMIX disk. Not yet: loading
  through Kickstart's drivers for disks without a native driver, tests on
  the real A3000.
- M3 (in progress): virtual CPU. A guest runs in user mode while
  SORIX-LDOM keeps supervisor mode, its vectors and the MMU: privileged
  instructions (SR access, MOVE USP, MOVEC, RTE, STOP, RESET, MOVES,
  PMOVE/PFLUSH/PTEST, CINV/CPUSH, FSAVE/FRESTORE) are emulated, exceptions
  and interrupts are reflected through the guest's own VBR, TRAP #15 is a
  hypercall. Ctrl-C stops a running guest and opens the shell; `resume`
  continues it. The pvtest guest passes on 68030, 68040 and 68060 in
  FS-UAE. A boot-time MMU self-test (translation, write, fault; 68030 PMMU
  and 68040/68060 MMU) must pass before a guest starts: CPUs or accelerator
  modes without a working MMU are refused. STOP (and the idle hypercall)
  really idles the CPU until an interrupt the guest has not masked, then
  delivers it. GDB stub: `gdb` in the shell hands the serial port to GDB
  for the stopped guest (registers, memory, breakpoints, single step,
  continue, detach; guest console output arrives as GDB output). M3 is
  complete in FS-UAE; to be confirmed on the real A3000.

Debugging pvtest with GDB in FS-UAE:

```sh
python3 host/uae.py run --gdb-port 2345    # SORIX-LDOM> pvtest stop
                                           # SORIX-LDOM> gdb
gdb-multiarch build/pvtest.elf -ex 'target remote localhost:2345'
```

On real hardware: `target remote /dev/ttyUSB0` after `set serial baud 115200`.
- M4 (in progress): `boot -pv unix_root:` starts AMIX deprivileged on a
  68030, with a shadow MMU built from AMIX's own page tables and a
  load-time PV patches (copyin/copyout, halt/reboot). In FS-UAE AMIX boots
  to multi-user and login, slowly (every spl traps; M6 makes that fast).
  A reboot goes through SORIX-LDOM, which boots the kernel again. Break in
  with Ctrl + Left Amiga + Help (FS-UAE on a PC: Left Ctrl + Left Windows
  + End) or Ctrl-C on serial; `resume` gives AMIX its screen back.
- M7: shadow MMU for the 68040 and 68060 too. The amigaux.org 040/060
  AMIX kernel boots deprivileged to the login prompt in FS-UAE on both
  (`boot -pv unix_root:/stand/unix-040`); next is the Z3660 in its real
  68060 mode. Shell aids:
  `guest log` (AMIX kernel messages), `pt` (page table walk), `md -g`,
  `watch`. Not yet on 68040/68060 (AMIX's 2 KB pages).
GDB's own m68k breakpoint (TRAP #15) is the hypercall, so breakpoints are
ILLEGAL instructions the stub tells apart by address.
- M5: a boot menu at every start (screen and serial): AMIX kernels on the
  AMIX roots (or `entry=` lines in `pvmk.conf`), deprivileged or native,
  a chooser for the kernels in `/stand`, the AmigaOS partition, the shell,
  reboot; `default=` and `timeout=` give the countdown. `nextboot` boots a
  kernel once (for testing); a non-default kernel that reboots quickly
  falls back to the default. Kernels that do not fit the CPU (stock 030
  kernel on a 68040/68060, 040/060 port kernel on a 68030) need a
  confirmation.
- Hard disk boot of SORIX-LDOM itself (`PVMK` partition, `pvinstall`) is
  the last milestone (M11); the boot floppy is the boot medium until then.

## Layout

| Directory | Contents |
|---|---|
| `boot/` | Shared floppy/RDB boot block, PVBB parameter area |
| `stage0/` | Kickstart-alive capture, read-only FFS, machine takeover |
| `core/` | vCPU, instruction emulation, exception reflection, IRQ, hypercalls, shadow MMU |
| `cpu/` | 68030 / 68040 / 68060 back-ends and CPU probing |
| `sp/` | Motorola FPSP/ISP support packages (040/060) |
| `drv/` | Paula UART, CIA timers, WD33C93A/SDMAC, RDB |
| `svc/` | Shell, log, GDB stub, ELF helpers |
| `ld/` | Personality loaders (AMIX, PVPI, Kickstart, raw) |
| `guest/pvtest/` | Bare-metal vCPU conformance test guest |
| `host/` | `mkimage`, `mkadf` (FFS volumes), `mkhdf` (RDB disks), `uae.py` (FS-UAE run/test), `pvterm`, toolchain fetch |
| `tools/` | AmigaOS-side tools (`pvinstall`) |
| `conf/` | Default `pvmk.conf` placed on the boot floppy |
| `include/pvmk/` | Public headers (hypercall ABI) shared with guests |

## Redistribution notice

This repository must not contain AMIX sources, headers, libraries or kernel
objects, nor Kickstart ROM images. Users supply their own licensed copies.
AMIX-side paravirtualization changes are kept as patches against a user's
own AMIX tree.
