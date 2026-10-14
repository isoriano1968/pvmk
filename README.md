# pvmk — paravirtualizing microkernel for the Amiga 3000

A small supervisor-mode microkernel for the Amiga 3000 (68030, 68040, 68060)
that boots from floppy (`pvmk.adf`) or an RDB hard disk partition, offers a
serial shell, and loads *personalities* — AMIX (SVR4) and AmigaOS — from hard
disk partitions, running them deprivileged with a paravirtual interface.
Boot and personality loading are traced over the serial port; a GDB remote
stub is included.

Status: **design phase**. See [PLAN.md](PLAN.md) for the architecture and
milestones.

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
| `host/` | `mkadf`, `pvterm`, `pvpack`, emulator configs |
| `tools/` | AmigaOS-side tools (`pvinstall`) |
| `conf/` | Default `pvmk.conf` placed on the boot floppy |
| `include/pvmk/` | Public headers (hypercall ABI) shared with guests |

## Redistribution notice

This repository must not contain AMIX sources, headers, libraries or kernel
objects, nor Kickstart ROM images. Users supply their own licensed copies.
AMIX-side paravirtualization changes are kept as patches against a user's
own AMIX tree.
