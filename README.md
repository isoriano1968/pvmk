# SORIX-ILOM-LDOM — paravirtualizing microkernel for the Amiga 3000

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
                                 #   SORIX-ILOM-LDOM> boot unix_root:
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

Hold the left mouse button at power-on to skip SORIX-ILOM-LDOM and boot
AmigaOS from the same disk.

## Shell

```
SORIX-ILOM-LDOM> help
  ver  cpu  mem  conf            system information
  scsi                           rescan the A3000 internal SCSI bus
  part                           list RDB partitions (all disks, all controllers)
  ls [dh0:path]                  floppy directory, or an AmigaDOS partition's
                                 (partitions on the A3000 internal SCSI)
  md <addr> [len]                memory dump
  mw[.b|.w|.l] <addr> <value>    memory write
  memtest <addr> <len>           RAM test that keeps the contents: a bus
                                 error is "no RAM", mirrors are found
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
with the SORIX-ILOM-LDOM floppy in DF0. Strap keeps its own order at that point,
so only the highest-priority bootable device after the floppy can be chosen
this way; `boot` says so for any other partition.

Numbers are hexadecimal; prefix `#` for decimal. Faults (bus error,
illegal instruction, ...) print a register dump and return to the shell.
The boot log survives a warm reset: `dmesg -p` shows the previous boot.

## Status

In daily use on an Amiga 3000 with a Z3660 (68060, 128 MB):

- **Boot manager.** It boots from its own `PVMK` hard disk partition or from
  the floppy, and shows the boot menu on screen and serial. It then starts the
  68040/68060 AMIX kernel natively from the AMIX disk, or boots AmigaOS.
- **The menu:**
  - kernels from `/stand`;
  - `entry=` lines in `pvmk.conf`;
  - `nextboot` to boot a kernel once, with fallback to the default when a
    test kernel reboots quickly;
  - a check that the kernel fits the CPU;
  - a kernel sent over the serial line (`boot ser:`);
  - `update ser:` to replace its own boot volume.
- **The shell and the serial boot log**, on the real machine.

Working in FS-UAE (68030, 68040 and 68060), not yet confirmed on real
hardware:

- **AMIX deprivileged** (`boot -pv`): AMIX runs in user mode under
  SORIX-ILOM-LDOM, with a shadow MMU built from its own page tables and
  load-time paravirtual patches. It reaches multi-user and login.
  - Break in with Ctrl + Left Amiga + Help, or Ctrl-C on serial; `resume`
    continues.
  - `shutdown` and `kill` stop a guest.
  - On the real Z3660 with 128 MB, AMIX's memory check takes minutes this
    way, so the boot menu boots natively by default (`mode=native`).
- **AmigaOS deprivileged** (`kick`): Kickstart 3.2 to its CLI.
- **The virtual CPU:**
  - privileged instructions emulated;
  - exceptions and interrupts reflected through the guest's VBR;
  - `TRAP #15` hypercalls;
  - idling on `STOP`;
  - a boot-time MMU self-test.

  The `pvtest` guest passes on all three CPUs.
- **GDB:** `gdb` in the shell hands the serial port to GDB for the stopped
  guest.

  ```sh
  python3 host/uae.py run --gdb-port 2345    # SORIX-ILOM-LDOM> pvtest stop
                                             # SORIX-ILOM-LDOM> gdb
  gdb-multiarch build/pvtest.elf -ex 'target remote localhost:2345'
  ```

  On real hardware: `target remote /dev/ttyUSB0` after
  `set serial baud 115200`.

Next on the plan:
- make the deprivileged boot usable on the real 68060, and measure its cost;
- performance work (M6);
- running AMIX and AmigaOS side by side (M9, a stretch goal);
- an ILOM-like web console over the Z3660's network (M10).

[PLAN.md](PLAN.md) has the architecture, the milestones and the development
log.

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

## Licence

MIT (`LICENSE`).
