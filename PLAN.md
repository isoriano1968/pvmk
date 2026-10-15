# SORIX-LDOM (pvmk) — Paravirtualizing Microkernel for the Amiga 3000

The microkernel identifies itself as **SORIX-LDOM** on the console;
`pvmk` remains the source/working name.

Working name: **pvmk**. A small supervisor-mode microkernel for the A3000
(68030, and 68040/68060 accelerators) that boots from a floppy disk (built
as an `.adf` image) or from an RDB partition, offers a serial/console shell, and loads *personalities* — AMIX (SVR4) first,
AmigaOS (Kickstart) later — from hard disk partitions, running them
deprivileged with a paravirtual interface. Full boot and personality-load
tracing goes over the serial port, with a GDB remote stub.

---

## 1. Goals and non-goals

### Goals
1. **Small**: resident core ≤ 32 KB code, shell + drivers + debug ≤ 32 KB,
   optional 040/060 support packages loaded only when needed. Total resident
   RAM reservation ≤ 256 KB (incl. shadow page tables, log ring, stacks).
2. **CPUs**: 68030 (+68881/68882), 68040, 68060, and the FPU-less
   68LC040/68LC060. One binary, runtime CPU dispatch. **A working MMU is a
   hard requirement**; SORIX-LDOM does not emulate one. CPUs without an MMU
   (68EC030, 68EC040, 68EC060) and accelerator modes that do not provide one
   can run the boot monitor (M1/M2) but cannot run guests (M3+). The CPU
   probe verifies the MMU with a self-test (map a page, translate it, fault
   on an unmapped one) and reports it.
3. **Boot shell** on serial (and optional minimal screen) that lists RDB
   partitions and loads a personality boot loader from any of them, with an
   autoboot default + timeout.
4. **Serial debugging** of the microkernel's own boot and of every step of
   personality loading; post-mortem log that survives warm reset; GDB RSP
   stub able to debug both pvmk and the guest.
5. **AMIX personality** runs unmodified object-only kernel parts
   (`os/exp`, `vm/exp`, `fs/exp`, …) with paravirtual changes confined to
   source-available code (`ml/`, `amiga/kernel`, `amiga/console`, `master.d`).
6. **AmigaOS personality** (Kickstart 2.x/3.x guest).
7. **Floppy boot**: the build produces `pvmk.adf`, a standard 880 KB DD
   Amiga disk image that can be written to a physical floppy and boots pvmk
   from `DF0:` with nothing installed on the hard disk. The disk is also a
   normal AmigaDOS (FFS) volume, so its config file can be edited from
   Workbench. See §8.1.

### Non-goals (initially)
- Running two personalities *concurrently*. The A3000's custom chips, CIAs
  and SDMAC have no clean sharing story; v1 runs **one active personality**
  with pvmk underneath. Concurrency is a stretch milestone (M8).
- SMP, Zorro III bus-master isolation (no IOMMU exists), 68020/68851.
- MMU emulation (see goal 2).

### What SORIX-LDOM is at each stage
- **M0–M2: a boot monitor and boot loader.** It owns the machine only until
  it hands over. `boot dh0` reboots into Kickstart; `boot -native` gives AMIX
  the whole machine (vectors, supervisor mode). After that SORIX-LDOM is no
  longer in control and there is no way back except a reset. `boot -native`
  stays as a fallback, not as the goal.
- **M3+: a microkernel with paravirtualized guests.** The guest runs in user
  mode; SORIX-LDOM keeps supervisor mode, its own vector table and the MMU.
  Like a service processor (Sun ILOM), it can always be entered while a guest
  runs: a break-in (keyboard combination, Ctrl-C on serial, level 7
  interrupt) stops the guest and opens the SORIX-LDOM shell, where its
  registers, memory and the logs can be inspected; `resume` continues the
  guest exactly where it stopped. The guest cannot change VBR, mask the
  level 7 interrupt or switch the MMU off: those instructions trap.

---

## 2. Findings from the AMIX tree that shape the design

| Fact (verified in this tree) | Consequence |
|---|---|
| Object-only kernel parts contain **~1,740 privileged instructions** (`objdump` of `os/exp` 731, `io/exp` 438, `fs/exp` 194, `vm/exp` 143, `netinet/exp` 118, `disp/exp` 74, …), mostly inlined `spl` sequences: `move.w %sr,%dN` / `move.w #0x2400,%sr`. | Pure source-level paravirtualization is impossible. Use **trap-and-emulate** as the baseline, with optional **load-time binary patching** of hot sequences. |
| The 68k HAT (`hat_*`, `vtop`) lives in object-only `vm/exp`. | Guest page tables can't be rewritten at source level → pvmk uses **shadow page tables** synchronised on guest `PFLUSH`/`PMOVE` (a "virtual TLB"). |
| `copyin/copyout/fubyte/subyte/resume` and `pstart.c` are in source `ml/`; `amiga/kernel/{support.c,servant.s}`, `amiga/console/*`, `master.d/kernel.c` (bdevsw/cdevsw) are source. | Paravirtual hooks (console, idle, MMU setup, reboot, hypercalls) go there. |
| Kernel is a relocatable ELF delivered on a boot partition: boot1 (1 KB) → boot2 info block → boot2 → kernel info block → ELF (`stand/README`, `boot/makeiblk.c`). | pvmk's AMIX loader reads the **existing partition format** directly — `make bootpart` keeps working. |
| Kernel entry contract (`boot/bootinfo.h`): `D0 = 3 (EXEC1)`, `D1 = &struct bootinfo` { ConfigDev[16], MemHeader[16], keystates, BootData }. Entry via `copyit.s`. | pvmk synthesises `struct bootinfo` itself and enters the guest at `bd_entry` with that contract. |
| Patches 0004–0009 already add 040/060 MMU/FPU/trap-frame paths to `ml/` and `copyit.s`. | Guest sees the **real CPU model**; pvmk does not have to fake a 68030 on 040/060. |

The 68030/040/060 are classically virtualizable for our purposes: since the
68010, `MOVE from SR` is privileged, so every sensitive instruction traps in
user mode. That is the foundation of the whole design.

---

## 3. Architecture

```
            ┌──────────────────────────────────────────────────────────┐
 user mode  │  Guest user processes (AMIX)   │  AmigaOS tasks          │
            ├──────────────────────────────────────────────────────────┤
 user mode  │  Guest kernel  (virtual S=1)   │  Kickstart exec (vS=1)  │
 (deprivil.)│  AMIX: os/vm/fs/io objects +   │                          │
            │  PV ml/, console, master.d     │                          │
            └───────────▲───────────────┬──────────────────────────────┘
          reflect exc./ │               │ priv-violation, A-line, TRAP #15
          virtual IRQs  │               ▼ (hypercall), bus error, IRQs
            ┌──────────────────────────────────────────────────────────┐
 supervisor │ pvmk core                                                │
            │  vcpu: decode/emulate privileged insns, exception reflect│
            │  vmmu: shadow page tables (030 PMMU / 040-060 MMU)       │
            │  virq: IPL virtualization, IRQ routing                   │
            │  hcall: hypercall ABI                                     │
            │  cpu/: 030 | 040 | 060 back-ends, FPSP/ISP (opt.)        │
            ├──────────────────────────────────────────────────────────┤
            │ pvmk services (same address space, not in fast path)     │
            │  shell · loaders (amix, kick, raw) · rdb · log · gdbstub │
            │  drivers: paula-uart, cia-timer, wd33c93/sdmac (polled)  │
            └──────────────────────────────────────────────────────────┘
                       A3000: 68030/040/060 · ECS · SDMAC · Ramsey/Gary
```

Single address space for pvmk (no internal IPC); "microkernel" here means
*minimal privileged core + policy-free mechanisms*, not a message-passing
server system. Services run in supervisor mode but are only entered from the
shell or debug paths, never from guest fast paths.

### 3.1 Physical memory map (A3000)

| Range | Use |
|---|---|
| `$00000000–$001FFFFF` | Chip RAM 2 MB — passed through to guest (custom-chip DMA must see real addresses) |
| `$00BFxxxx` | CIA A/B — passed through or trapped per device policy |
| `$00DC0000` | RTC (RP5C01) |
| `$00DD0000` | SDMAC + WD33C93A (internal SCSI) |
| `$00DE0000` | Gary/Ramsey control |
| `$00DFF000` | Custom chips |
| `$00F80000–$00FFFFFF` | Kickstart ROM (or SuperKickstart RAM image) |
| `$07000000–$07FFFFFF` | Motherboard fast RAM (≤16 MB). **pvmk reserves the top 256 KB**; hidden from guests |
| `$08000000+` | Accelerator RAM (A3640, CyberStorm…) — Zorro III / CPU-slot RAM, passed through |

**Guest-physical == machine-physical** (identity) minus pvmk's hole. Reason:
SDMAC, Agnus and Zorro bus masters DMA to physical addresses and there is no
IOMMU; identity mapping keeps unmodified DMA drivers correct. The hole is
removed from the guest's `MemHeader` list and is unmapped in every shadow
table (guest access → bus error reflected into the guest).

pvmk maps itself and the hardware for *its own* use with transparent
translation registers restricted to supervisor function codes (030 `TT0/TT1`
FC mask; 040/060 `DTT/ITT` S-field = supervisor-only), so pvmk costs zero ATC
entries and is invisible to the guest.

---

## 4. Virtual CPU

### 4.1 Privilege model
- Real **supervisor**: pvmk only.
- Real **user**: guest kernel *and* guest user code. pvmk tracks a virtual
  `vSR` (S, M, T1/T0, IPL, CCR comes from the real SR).
- Every guest kernel ↔ guest user transition goes through pvmk (guest `RTE`
  traps; user exceptions/traps arrive at pvmk first), so pvmk switches the
  shadow MMU root and the real stack pointer at those points.

### 4.2 Virtual CPU state (per vCPU)
`vSR, vUSP, vISP, vMSP, vVBR, vSFC, vDFC, vCACR`, plus per family:
- 030: `vTC, vCRP, vSRP, vTT0, vTT1, vMMUSR`
- 040/060: `vTCR, vURP, vSRP, vITT0/1, vDTT0/1, vMMUSR` (+060: `vPCR`, `vBUSCR`)
- FPU: real FPU state is owned by the guest; pvmk only saves it when pvmk
  itself needs the FPU (it doesn't) or when switching personalities.

### 4.3 Emulated instructions
Decoded in the privilege-violation / line-F handlers:

| Group | Instructions |
|---|---|
| Status | `MOVE to/from SR`, `ANDI/ORI/EORI #,SR`, `MOVE USP` |
| Control | `MOVEC` (all CRs incl. `VBR`, `CACR`, `SFC/DFC`, `MSP/ISP`, 040/060 MMU CRs, `PCR`), `RTE` (all frame formats), `STOP` (→ idle), `RESET` (→ policy: ignore or device reset) |
| Address space | `MOVES` (emulated with the guest's vSFC/vDFC view) |
| 030 MMU | `PMOVE`, `PFLUSH*`, `PTEST`, `PLOAD` |
| 040/060 | `PFLUSH*`, `PTEST` (040), `PLPA` (060), `CINV`, `CPUSH` |
| FPU | `FSAVE`, `FRESTORE` (executed for the guest on its behalf, validated) |

A small **decoder table** keyed on the opcode's top 10 bits keeps this
compact; effective-address decoding is shared code.

### 4.4 Exception reflection
Every exception that the guest would see natively (trap #0 syscalls,
bus/address errors from guest pages, illegal, line-A/F, FP exceptions, trace,
virtual interrupts) is delivered by:
1. computing the guest's view of the frame (format matching the real CPU:
   030 `$0/$2/$9/$A/$B`, 040 `$0/$2/$3/$4/$7`, 060 `$0/$2/$4`);
2. pushing it onto the guest's *virtual* supervisor stack (vISP/vMSP);
3. setting vSR (S=1, T=0, IPL as appropriate), loading PC from `vVBR + 4·vec`;
4. switching shadow root to the guest-kernel view and resuming in user mode.

Guest `RTE` traps; pvmk validates and pops the frame from the guest stack,
emulating continuation semantics (030 bus-error rerun, 040 writeback, 060
restart).

### 4.5 Interrupt virtualization (the cheap trick)
The 68k's SR IPL mask applies in user mode too. On every return to the guest,
pvmk sets **real IPL = vIPL**. Hence:
- `spl` emulation = record vIPL, set real IPL, return. No pending-queue logic
  for the common case; hardware masks exactly what the guest masked.
- An interrupt that fires is by construction deliverable: pvmk reflects it to
  `vVBR + autovector` immediately.
- pvmk reserves for itself: **level 7** (NMI, debug break-in), and claims
  device interrupts only for devices it owns (see §6 device policy).
- Level-triggered sources (Paula/INTREQ, CIA ICR) are not acked by pvmk for
  guest-owned devices — the guest's handler acks them as on bare metal.

### 4.6 Hypercalls
`TRAP #15` executed with **vS=1** is a hypercall; with vS=0 it is reflected to
the guest kernel as a normal trap (so guest user programs can't hypercall).
ABI: `d0` = call number, `d1–d4`/`a0–a1` args, `d0` result, all other regs
preserved.

| # | Call | Purpose |
|---|---|---|
| 0 | `hv_version` | ABI version, CPU model, features bitmap |
| 1 | `hv_putc / hv_write` | PV console + log channel |
| 2 | `hv_getc` | PV console input |
| 3 | `hv_idle` | replaces `STOP` loop; returns on virtual IRQ |
| 4 | `hv_setipl` | batched spl (used by patched sites, §7.3) |
| 5 | `hv_mmu_sync` | explicit shadow resync hint for a range |
| 6 | `hv_bootinfo` | get pointer to pvmk-built bootinfo / cmdline |
| 7 | `hv_reboot / hv_halt` | return to pvmk shell instead of hard reset |
| 8 | `hv_break` | enter debugger (shell or GDB) |
| 9 | `hv_log` | tagged log line with level |

### 4.7 CPU back-ends and support packages
| | 68030 | 68040 | 68060 |
|---|---|---|---|
| MMU | PMMU (`PMOVE`, TC/CRP/SRP, TT0/1) | `MOVEC` TCR/URP/SRP, ITT/DTT | as 040, `PLPA`, no `PTEST` |
| Caches | CACR (I/D, burst, WA) | `CINV/CPUSH`, copyback | + store buffer, branch cache, `PCR` |
| Frames | `$A/$B` bus error | `$7` access error | `$4` access error |
| FPU | 68881/2 | built-in, partial | built-in, partial |
| Missing in hw | — | FP transcendental etc. | + `MOVEP`, 64-bit `MULx/DIVx`, `CAS2`, `CHK2/CMP2`, misaligned `CAS` |

pvmk owns the vector table, so **pvmk hosts Motorola's FPSP (040) and
FPSP/ISP (060)** — the same packages NetBSD and Linux use — as loadable
modules (≈ 40–60 KB) that are only resident when an 040/060 is detected.
AMIX binaries compiled for 68020/68881 then run unmodified.

CPU detection is by probing (illegal-instruction handlers around `MOVEC`
of 040/060-only CRs, `PCR` read for 060) — not via `AttnFlags`, so it also
works when pvmk is loaded without Kickstart. Revision and FPU presence
(68LC040/68EC060) are logged at boot.

---

## 5. MMU virtualization ("virtual TLB")

- The guest builds its own page tables in guest memory and loads the root via
  `PMOVE CRP/SRP` (030) or `MOVEC URP/SRP` (040/060) → trap → pvmk records
  the virtual root but **never installs it**.
- pvmk maintains **two shadow trees per guest address space**:
  - *kernel view* (vS=1): guest supervisor-only pages accessible to real
    user mode, pvmk hole unmapped;
  - *user view* (vS=0): guest supervisor-only pages invalid.
  Shadow root switches on every vS transition.
- Shadows are filled **lazily on fault**: bus error in user mode → walk the
  guest tables (using guest TC/TCR geometry), and either install a shadow
  entry or reflect a guest fault with a correctly synthesised frame/MMUSR.
- Invalidation is driven by guest `PFLUSH*`/`PFLUSHA` (the HAT must issue
  these on 68k for its own correctness) and by root/TC changes. A shadow cache
  keyed by guest root (small, e.g. 8 entries) avoids rebuilding on context
  switch.
- **Fallback** if AMIX's HAT turns out to modify live PTEs without flushing:
  write-protect guest page-table pages in the shadow and resync on write
  faults. Decide after tracing (M4) — instrument with `hv_log` counters.
- Guest TT registers: emulated by synthesising shadow mappings (never handed
  to the real TT registers, which pvmk owns).
- 040/060 copyback caches: page-table pages for the hardware walker are
  mapped cache-inhibited/serialized in pvmk.

---

## 6. Devices

### 6.1 pvmk-owned (minimal, polled where possible)
| Driver | Use | Notes |
|---|---|---|
| Paula UART (`SERDAT/SERDATR/SERPER`) | debug console, shell, GDB | `SERPER = clock/baud − 1` (PAL 3 546 895 Hz, NTSC 3 579 545 Hz); default 115200 (≈ −0.7 % PAL), fallback 9600/19200 |
| CIA-B timer A | timeouts, autoboot countdown, log timestamps | E-clock based |
| Disk driver layer | partitions, kernels, `ls`/`cat` | several native drivers side by side, each probing for its own hardware; partitions record which driver reaches them |
| A3000 internal SCSI (WD33C93A + SDMAC) | native disk driver | polled, DMA through a chip RAM bounce buffer; read-only. Kept unconditionally (also emulated by the Z3660-amix firmware) |
| Z3660 piscsi mailbox | native disk driver | used only if the AutoConfig board `0x144B:0x01` is present; synchronous MMIO mailbox (unit, block, length, buffer address, then the read register), no interrupts |
| Kickstart's drivers (stage0) | any other controller | before takeover only: RDB, partition list, kernel loading |
| RDB parser | partition table | `devices/hardblocks.h` layout, over any block reader |
| `COLOR00` progress codes | headless diagnosis | distinct colour per boot stage, like Kickstart |
| Later: A4091 (NCR 53C710), A2091 | extra disks | M6+ |

### 6.2 Device ownership policy (per personality, shell-configurable)
- **pass-through** (default): guest gets custom chips, CIAs, SCSI, Zorro
  boards mapped directly; IRQs routed to the guest.
- **pv**: guest uses a hypercall-backed device (console first).
- **pvmk**: device reserved for pvmk.

Serial port: `serial=debug` (pvmk owns it, guest gets `pvcons`),
`serial=guest` (pass-through after handoff; pvmk keeps writing log only to the
RAM ring), `serial=shared` (pvmk multiplexes; guest output prefixed). Default
for bring-up: `debug`.

---

## 7. Personalities

### 7.1 Personality images
Each bootable partition carries a loader the shell understands:

| Kind | Detection | Loader |
|---|---|---|
| **AMIX** | existing AMIX boot partition (boot1 + info blocks, `stand/makeiblk`) | `ld-amix` |
| **PVPI** | new generic header in block 0/2 | `ld-pvpi` |
| **Kickstart** | `kick.rom` image in PVPI container, or live ROM at `$F80000` | `ld-kick` |
| **raw** | flat binary + load address (testing) | `ld-raw` |

PVPI header (big-endian, 512-byte block):
```
magic 'PVPI' | hdr_version | abi_min/abi_max | cpu_mask (030/040/060)
flags (needs_fpu, wants_serial, …) | load_addr | entry | image_size
image_crc32 | n_segments | segment table | cmdline[128]
```

### 7.2 AMIX loader (`ld-amix`)
The kernel is loaded from the **AMIX root file system (UFS)** at a path the
user gives; the AMIX boot partition is the fallback.

```
boot unix                     # default path /stand/unix on partition UNIX
boot unix:/stand/unix.test    # any path on the root partition
boot dh1:/stand/unix          # any UFS partition, any path
boot -native unix:/stand/unix # M2: hand the machine to AMIX entirely
```
`default=` in `pvmk.conf` takes the same syntax (e.g. `default=unix:/stand/unix`).

**I/O path.** The shell reads the kernel with SORIX-LDOM's native disk
drivers (A3000 internal SCSI, Z3660). Reading it through Kickstart's
drivers at stage0 (a boot request and a reboot, like `boot dh0`) was
considered for disks without a native driver and dropped: AMIX needs a
driver for its root disk as well, so a disk SORIX-LDOM cannot read natively
is usually one AMIX cannot use as root either. Other controllers get a
native driver instead (as the Z3660 did). Kept as an optional idea only.

**UFS reader** (`svc/ufs.c`, read-only, about 600–800 lines, same block
callback interface as the FFS reader): superblock at byte 8192 (magic
`0x011954`), cylinder group geometry, inodes (12 direct, single/double/
triple indirect blocks), directories, symbolic links (fast and slow, with a
loop limit), big-endian as written by AMIX. The shell gets `ls` and `cat` on
UFS partitions to check a kernel before booting it. `s5` is only detected
and reported (AMIX root is UFS).

Loader steps:
1. Locate the kernel: UFS path lookup (or, fallback, the AMIX boot
   partition: boot2 info block at offset `0x400` → skip boot2 → kernel info
   block with ELF size, checksum and load address, `boot/chksum.c`).
2. Read the ELF (`/stand/unix` is a relocatable ELF of about 1 MB) into
   memory reserved at stage0.
3. Relocate it to the chosen address (logic from `boot/elf2brel.c` /
   `bind.c`).
4. Apply **PV patch table** (§7.3).
5. Build `struct bootinfo`: `autocon[]` from the `ConfigDev` list captured
   from Kickstart at pvmk stage0 (§8), `memory[]` = real memory minus pvmk
   hole, `keystates`, `bootdata`.
6. Create vCPU: vSR = `$2700`, vVBR = 0, MMU off (identity shadow),
   `D0 = 3`, `D1 = &bootinfo`, PC = `bd_entry`. Start.

AMIX-side source changes (built with the existing `m68k-cbm-sysv4` cross
toolchain):
- `ml/pstart.c`: if `hv_version` succeeds, skip cache/MMU tricks that pvmk
  emulates poorly (none mandatory — all trap — but fewer traps).
- `amiga/kernel/servant.s` reboot/halt → `hv_reboot`.
- idle loop `STOP` → `hv_idle`.
- `amiga/console`: add `pvcons` (hypercall putc/getc) registered in
  `master.d/kernel.c` `cdevsw`; selected when `serial=debug`.
- Detection: `TRAP #15` with `d0 = 0` returns magic `'PVMK'` under pvmk;
  natively `TRAP #15` must be harmless — install a stub vector in `pstart`
  before probing, so the **same kernel boots native and under pvmk**.

### 7.3 Load-time spl patching (performance phase, M5)
The two-instruction spl sequence `move.w %sr,%dN; move.w #imm,%sr` is 6 bytes.
Using ELF relocations to stay inside `.text` and avoid relocated words, the
loader rewrites recognised sequences into a 6-byte PV form, e.g.
`move.w (vsr).w,%dN` + A-line `$A0xx` (`hv_setipl` fast entry via line-A
vector, IPL in the low bits). Lowering IPL still enters pvmk (needed to apply
the real mask); raising becomes a single fast trap with no decode. Only do
this if profiling (§9.5) shows spl traps dominate.

### 7.4 AmigaOS personality (`ld-kick`, M7)
- Capture the Kickstart image at stage0 (read `$F80000–$FFFFFF`; on
  SuperKickstart A3000s this is RAM, read it before takeover).
- Guest starts at Kickstart reset entry with vS=1, IPL 7, MMU off.
- Kickstart's memory probing sees the pvmk hole as end-of-RAM (bus error
  reflected) — verify with Kickstart 2.04 and 3.1.
- `68040.library`/`68060.library` MMU and cache calls trap and are emulated;
  `SetPatch`, `Supervisor()`, `Disable()` (Paula INTENA, not privileged) work
  as-is with pass-through.
- `ColdReboot()` (`RESET` + jump) → returns to pvmk shell.

### 7.5 Native handoff (`boot -native`)
pvmk can also give the machine away entirely: tear down its MMU/TT, restore
the guest's view, enter the personality in real supervisor mode, leaving only
a level-7 debug vector and the RAM log. This is what M1 delivers and is the
permanent fallback when virtualization overhead is unwanted.

---

## 8. Boot chain

pvmk can boot from two kinds of media. Both use the same 1 KB boot block code
and the same image. Only the device and the block offsets differ.

```
Power-on → Kickstart (ROM)
  ├─ floppy: DF0: bootblock "DOS\1" + valid checksum           (§8.1, primary for bring-up)
  └─ hard disk: RDB scan → partition with DE_BOOTBLOCKS=2 ("PVMK")
  → boot1 (≤1 KB, PIC; derived from amiga/boot/boot1.c)
      loads pvmk image via exec DoIO on the IOStdReq Kickstart passed in A1
      (trackdisk.device or scsi.device), checks CRC, flushes caches, jumps
  → pvmk stage0  (Kickstart still alive)
      floppy only: read pvmk.conf + optional modules from the FFS volume,
                   then motor off (TD_MOTOR 0)
      capture: ExecBase memory list, AttnFlags (cross-check), ConfigDev list
               (expansion.library), PAL/NTSC, Kickstart version + image,
               boot device/unit, keyboard state
      log → serial: "[0.000] pvmk stage0 ..."  COLOR00 = stage colour
  → takeover (logic from boot/copyit.s incl. 040/060 MMU teardown patch)
      Disable, stop DMA/INTs, caches flush, MMU off,
      relocate pvmk to top of fast RAM, set VBR, TT regs, stacks
  → pvmk stage1: cpu probe, FPSP/ISP load, serial init, timers, SCSI init
  → shell (autoboot countdown) → loader → personality
```

### 8.1 Floppy boot and the ADF image

**What the build produces.** `make adf` writes `build/pvmk.adf`. This is a
901 120-byte image of a standard Amiga DD disk: 80 cylinders × 2 heads × 11
sectors × 512 bytes, 1 760 blocks. It is formatted as **FFS (`DOS\1`)** with
the volume name `PVMK`.

| Blocks | Content |
|---|---|
| 0–1 | Boot block: `DOS\1`, checksum, root block = 880, `boot1` code, **PVBB parameter area** |
| 2–879 | FFS data area: the file `pvmk` (core image, **allocated contiguously** by `mkadf`) |
| 880 | Root block (volume `PVMK`) |
| 881 | Bitmap block |
| 882+ | Remaining files: `pvmk.conf`, `fpsp040.mod`, `fpsp060.mod`, `isp060.mod`, `README`, optional `personalities/` |

FFS is used instead of OFS on purpose. FFS data blocks carry no headers, so
the contiguous `pvmk` file is a plain byte image on disk. The boot block can
then load it with **one `CMD_READ`**, without parsing the file system.
Kickstart 2.0+ (every A3000) can boot from FFS floppies.

**Boot block (`boot/bootblock.s`, ≤ 1012 bytes of code).**
- Kickstart reads blocks 0–1 and checks the boot block checksum. That is the
  sum of 256 longwords with end-around carry, and the complement is stored at
  offset 4. Kickstart then calls offset 12 with `A6 = SysBase` and
  `A1 = IOStdReq` for `trackdisk.device` unit 0.
- The **PVBB area** sits at a fixed offset near the end of block 1. It holds:
  magic `'PVBB'`, version, `start_block`, `n_blocks`, `image_size`,
  `image_crc32`, `entry_offset` and flags. `mkadf` fills it in and then
  recomputes the boot block checksum.
- The boot block does the following:
  1. `AllocMem(image_size, MEMF_FAST|MEMF_PUBLIC)`, falling back to any memory.
  2. One `DoIO` read of `n_blocks` from `start_block`.
  3. A CRC32 check.
  4. `CacheClearU()`, which is required on 040/060 copyback caches.
  5. A jump to stage0, passing `SysBase`, the `IOStdReq` and a boot-medium
     tag (`BOOT_FLOPPY`).
- If any step fails, it shows the same dead-end alert that AMIX `boot1` uses,
  with a code for read error, bad magic or bad CRC. It also sets a `COLOR00`
  code.
- When the floppy is booted, the same code reads from block offset 0. When the
  hard disk is booted, it reads relative to the partition start. One source
  file, built twice with different `-D` flags.

**Stage0 on floppy (Kickstart still alive).**
- It reads the file system **read-only** with a minimal FFS reader of about
  1.5 KB. The reader handles the root block, the hash chain, file header
  blocks and extension blocks, and is reused by the shell's `ls df0:`. It loads:
  - `pvmk.conf` (key=value: `serial=`, `baud=`, `default=`, `timeout=`,
    `log=`, `trace=`). It is plain text, so it can be edited from Workbench
    with any editor.
  - The FPSP/ISP modules that match the probed CPU.
  - Optionally, small personality images under `personalities/`. These are
    PVPI or raw test guests such as `pvtest`. This lets the vCPU be tested
    with **no hard disk at all**.
- Then it stops the drive motor (`TD_MOTOR`, 0), waits for the read DMA to
  finish, and hands over to the takeover step. After takeover pvmk has **no
  floppy driver**: everything it needs from the disk has already been loaded.
  Personalities still come from hard disk partitions through the SCSI driver.
- If `pvmk.conf` is missing or broken, built-in defaults are used and a log
  line `conf: using defaults (reason)` is printed.

**Integrity if the disk is modified from AmigaOS.** If the user replaces the
`pvmk` file from Workbench, AmigaDOS may store it in different blocks. The
PVBB data then becomes stale and the CRC check fails with a specific alert.
Recovery:
- `host/mkadf` rebuilds the image.
- `pvinstall DF0:` (a small AmigaOS CLI tool, shipped on the disk) locates the
  `pvmk` file's block run. It verifies the run is contiguous and defragments
  it if not, then rewrites PVBB and the boot block checksum. It is the same
  tool that installs pvmk to a hard disk `PVMK` partition.

**Config persistence.** `save` in the shell cannot write the floppy, because
pvmk has no floppy driver and the disk may be write-protected. On floppy
boot, `save` writes to a hard disk `PVMK` partition if one exists. Otherwise
it prints the `pvmk.conf` text to serial, ready to paste into the file under
AmigaOS.

**Host tool `host/mkadf`** (C, no dependencies, about 600 lines):
```
mkadf -o build/pvmk.adf -n PVMK \
      --bootblock build/bootblock.bin \
      --image build/pvmk.bin \
      --file pvmk.conf=conf/pvmk.conf \
      --file fpsp040.mod=build/fpsp040.mod ... \
      --file c/pvinstall=build/pvinstall
```
It formats FFS, places `pvmk` contiguously from block 2, adds the other
files, fills PVBB and fixes up all checksums (boot block, root, bitmap,
headers). `mkadf --verify pvmk.adf` re-reads the image, checks every
checksum and the CRC, and lists the files. CI runs it after every build.

**Writing the ADF to a real floppy.** A standard PC floppy drive **cannot**
write the Amiga track format (11 sectors, one track written as a whole). Use
one of these:
- A flux or USB floppy interface on the host: **Greaseweazle**
  (`gw write --format amiga.amigados build/pvmk.adf`), KryoFlux or
  SuperCard Pro.
- The A3000 itself: copy `pvmk.adf` over serial, network or CF/SCSI2SD, then
  write it to `DF0:` with an ADF-writing tool (for example ADF-Blitzer or
  `adf2disk`-style utilities).
- Gotek users can drop `pvmk.adf` onto the USB stick as-is.

The disk boots from the A3000 boot menu (hold both mouse buttons at power-on)
even when a bootable hard disk is present. The floppy is first in the boot
order anyway when a disk is inserted.

**Emulator.** WinUAE/FS-UAE mount `pvmk.adf` directly as DF0:. This becomes
the fastest inner loop for M0/M1: `make adf && uae-run`, with no HD image
rebuild.

Alternative entry points (development speed):
- `pvload` — AmigaOS CLI executable that does stage0 + takeover from a file
  (like NetBSD `loadbsd`). Fastest inner loop with a real A3000.
- `sload` — shell command receiving an image over serial (raw binary with
  length+CRC framing, or S-records) straight into RAM; `go` to run.
- Emulator: WinUAE A3000 config (full 030/040/060 MMU emulation, serial →
  TCP); Linux host via Wine or FS-UAE where MMU emulation suffices.

Warm reset recovery: the log ring lives at a fixed address in pvmk's hole
with magic + CRC; stage0 on the next boot detects it and prints
`previous boot log available (dmesg -p)`.

---

## 9. Shell and debugging

### 9.1 Shell commands
```
help                      ver / cpu / mmu       memory map, CPU/FPU/MMU info
part                      list RDB partitions (unit, name, dostype, size, boot pri)
ls df0:[path]             list files on the boot floppy (cached at stage0, read-only)
probe <part>              detect personality kind, print header/info blocks
                          <part> may also be df0:personalities/<file> (preloaded at stage0)
load <part> [args]        load personality, do not start (stop before entry)
boot [<part>] [args]      load + start ; boot -native ; boot -trap N (trace first N emulations)
go [addr]                 start loaded personality / jump
sload <addr>              serial upload
md <addr> [len]           memory dump        mw <addr> <val>   memory write
regs / vregs              real / guest virtual registers
bt                        guest backtrace (frame-pointer walk, ELF symbols if loaded)
trace on|off <class>      log classes: boot, ld, vcpu, vmmu, irq, hcall, scsi
log level <n> / dmesg [-p]
set <var> <val>           serial, baud, autoboot, timeout, default part
save                      persist config to PVMK partition config block
                          (floppy boot without PVMK partition: print pvmk.conf text)
gdb                       switch serial port to GDB remote protocol
reset                     hard reset   ; resume   continue guest after break-in
```
Line editing + 8-entry history; ~4 KB.

Autoboot: `default=dh0.2 timeout=3` stored in `DF0:pvmk.conf` (floppy boot)
or in the PVMK partition's config block (HD boot); any key interrupts.

### 9.2 Example boot transcript
```
[0.000] pvmk 0.1 stage0 (Kickstart 39.106, NTSC) boot=floppy df0 blk 2+112 crc ok
[0.001] ffs: PVMK: pvmk.conf (6 keys), fpsp060.mod skipped (cpu 030), motor off
[0.001] mem: chip 2048K @000000, fast 16384K @07000000; reserving 256K @07FC0000
[0.002] acon: 3 boards (A3000 SCSI, ... )
[0.004] takeover: MMU off, VBR=07FC0000, relocated
[0.005] cpu: 68030 rev 2, FPU 68882, MMU 030 PMMU
[0.020] scsi0: WD33C93A rev A, unit 0: QUANTUM LP240S
[0.031] rdb: 4 partitions
pvmk> part
  #  name   dostype   size    kind
  0  PVMK   PVM\0      256K   pvmk
  1  DH0    DOS\3       60M   amigados
  2  UNIX   UNI\1       10M   amix-boot (relocunix, 1.2M)
  3  ROOT   UNI\0      160M   -
pvmk> boot 2
[2.110] ld-amix: info blk ok, boot2 skipped (24K), kernel 1213440 bytes crc ok
[2.890] ld-amix: ELF rel, 3 segs, 18211 relocs, load @07000000
[2.902] ld-amix: bootinfo @07FD0000: 3 acon, 2 mem regions
[2.903] vcpu: entry 07000400 D0=3 D1=07FD0000 vSR=2700
[2.910] vcpu: movec vbr<-07000000 ; pmove tc<-...  (trace 1/64)
...
```

### 9.3 Log subsystem
- `log(class, level, fmt, …)` → ring buffer (32 KB) + serial if enabled.
- Timestamps from CIA E-clock counter (sub-ms resolution).
- Classes as in `trace` command; per-class enable bitmap; level 0–4.
- Polled TX so logging works with interrupts off, during takeover, and inside
  exception handlers. Never allocates.

### 9.4 Fault reporting
Unhandled pvmk exceptions and guest crashes print: vector name, decoded frame
(per CPU format), real + virtual registers, MMUSR/FSLW, faulting instruction
bytes and disassembly hint, guest stack words, shadow-walk of the fault
address. Then drop into the shell (`resume` if recoverable).

### 9.5 GDB remote stub
- RSP over the same Paula port; `gdb` command or a received `$` packet
  switches mode; `D`/detach returns to shell.
- Targets: pvmk itself and the guest (registers presented as the guest sees
  them: vSR, vISP/vUSP, virtual CRs as `monitor` commands).
- Breakpoints: `BKPT #n` / `ILLEGAL` inserted into guest text (shadow mapping
  temporarily writable); single-step via real T1 bit; watchpoints via shadow
  page protection (optional).
- Break-in: `Ctrl-C` on serial (RBF level 5) — works while vIPL < 5; for
  guest code running at IPL 7, pvmk runs the guest at real IPL ≤ 6 in
  "debug" mode and emulates the difference (documented limitation otherwise).
- Host: `gdb-multiarch` (m68k) with pvmk ELF and AMIX `unix` symbol files.
- Profiling counters exposed via `monitor stats`: traps per opcode class,
  shadow faults, reflections, hypercalls — drives the §7.3 decision.

---

## 10. Source layout and toolchain

```
pvmk/
  PLAN.md
  Makefile                 # m68k-elf-gcc, -m68020-60 -Os -ffreestanding, flat bin + ELF
  boot/  bootblock.s pvbb.h mkpart.c        # shared floppy/RDB boot block, PVBB area, partition packer
  stage0/ stage0.c ffs_ro.c takeover.s      # Kickstart-alive capture, read-only FFS, takeover
  conf/  pvmk.conf                          # default config placed on the ADF
  tools/ pvinstall.c                        # AmigaOS CLI: install/repair floppy or PVMK partition
  core/  entry.s vectors.s trap.c vcpu.c decode.c emul.c reflect.c
         virq.c hcall.c vmmu.c shadow030.c shadow040.c
  cpu/   cpu030.s cpu040.s cpu060.s probe.s
  sp/    fpsp040/ fpsp060/ isp060/          # Motorola support packages (modules)
  drv/   paula.c cia.c sdmac.c wd33c93.c rdb.c
  svc/   shell.c cmds.c log.c gdbstub.c dis.c elf.c
  ld/    ld_amix.c ld_pvpi.c ld_kick.c ld_raw.c
  guest/ pvtest/                            # bare-metal test guest (vCPU conformance)
  host/  mkadf.c (ADF builder/verifier), pvterm (serial helper), pvpack (PVPI builder), winuae/ configs
  include/pvmk/hcall.h                      # shared with AMIX side
```
AMIX-side changes as patches in `amix/` (continuing the 00NN series),
built with the existing `m68k-cbm-sysv4` toolchain.

Code rules: no 64-bit `MUL/DIV`, no `MOVEP`, `CAS2`, `CHK2/CMP2` in pvmk
(060 would trap into the ISP — which pvmk itself hosts); CPU-specific code
only in `cpu/` and `shadow0x0.c`, dispatched via a function table filled at
probe time.

Size budget (enforced by a `make size-check` target):
| Part | Budget |
|---|---|
| core (vectors, vcpu, decode, virq, hcall, vmmu) | 24 KB |
| cpu back-ends | 8 KB |
| drivers | 8 KB |
| shell + log + gdbstub + loaders | 24 KB |
| FPSP/ISP (040/060 only) | ≤ 60 KB |
| data: shadow tables pool, log ring, stacks | ≤ 128 KB |

---

## 11. Milestones

| M | Deliverable | Acceptance |
|---|---|---|
| **M0** | Toolchain (m68k-elf), WinUAE A3000 configs (030/040/060), `pvterm`, Makefile, size check, `mkadf` + `bootblock.s` | `make adf` produces `pvmk.adf` that passes `mkadf --verify`; booting it in WinUAE prints "hello" on serial |
| **M1** | Boot monitor from the **boot floppy** (bootblock, read-only FFS, `pvmk.conf`), stage0 capture, takeover, serial log, `COLOR00` codes, shell (`md/mw/regs/cpu/dmesg/ls`), `sload/go` | Real A3000 boots to the SORIX-LDOM shell from a physical floppy written from `pvmk.adf`; boot log + previous-boot log readable |
| **M2** | Read-only UFS reader, `ls`/`cat` on UFS, kernel loading with a free path (`boot unix_root:/stand/unix`) through the native disk drivers, `ld-amix` relocation + bootinfo synthesis, native start (SCSI driver, RDB and `part` were done early, in M1) | Unmodified AMIX boots multi-user via SORIX-LDOM **natively** from `/stand/unix` on its UFS root, on 030, from the internal SCSI and from a Z3660 HDF |
| **M3** | vCPU core: emulation of all §4.3 instructions (030), reflection, `pvtest` conformance guest (shipped on the ADF under `personalities/`), **break-in and `resume`**, GDB stub, MMU self-test | `pvtest` passes 100 % on 030 (WinUAE + real HW), booted entirely from floppy; a break-in while `pvtest` runs opens the SORIX-LDOM shell and `resume` continues it; GDB can step pvtest |
| **M4** | vMMU 030 shadow, IRQ virtualization, hypercalls, AMIX PV patches (`pvcons`, idle, reboot, detection) | AMIX boots **single-user deprivileged** on 030; a break-in into running AMIX and `resume` work; same kernel still boots native |
| **M5** | Stability + performance: multi-user, SCSI/network under load, stats, optional spl patching | AMIX multi-user; kernel build of a driver completes; overhead measured & documented |
| **M6** | 040 then 060 back-ends, FPSP/ISP modules, 040/060 shadow MMU | M3–M5 acceptance repeated on A3640 (040) and an 060 card |
| **M7** | `ld-kick`: Kickstart 3.1 personality, pass-through custom chips | Workbench boots under pvmk; `ColdReboot` returns to shell |
| **M8** (stretch) | Concurrent personalities with device partitioning (e.g., AMIX headless on serial+SCSI, AmigaOS on display) | Proof of concept only |
| **M9** (to be decided) | ILOM-like web console over the Z3660 network interface: NIC driver owned by SORIX-LDOM, minimal TCP/IP (ARP, IPv4, ICMP, small TCP; static IP from `pvmk.conf`, DHCP later), HTTP server with login (password hash in `pvmk.conf`, session cookie; plain HTTP on the LAN, HTTPS optional later). Pages: version, CPU/MMU, uptime, memory, disks, log, personalities and their state; actions: stop (break-in), resume, reset, kill. Needs M4's IRQ virtualization so the console answers while a guest masks interrupts. Stack tested on the host against a mock NIC; the Z3660 driver on real hardware. Name, scope and order still to be decided | From a browser on the LAN: log in, see the status of SORIX-LDOM and a running AMIX, stop and resume it |
| **M10** (last) | Optional hard disk boot of SORIX-LDOM itself: the same boot block on an RDB `PVMK` partition (`DE_BOOTBLOCKS`), `pvinstall` to create/repair it. Until then, and possibly for good, the boot floppy is the boot medium | Real A3000 boots SORIX-LDOM from its `PVMK` partition with no floppy |

### AMIX-side milestones (in the AMIX tree, built with `~/opt/amix-cross`)

These change the AMIX kernel itself and run in parallel with the
SORIX-LDOM milestones. Everything stays bootable natively (`boot -native`)
and deprivileged (`boot -pv`); SORIX-LDOM's load-time PV patches keep
working on unmodified kernels.

| A | Deliverable | Acceptance |
|---|---|---|
| **A1** | AMIX kernel for 68040/68060: adopt the amigaux.org port (github.com/asokero/amix-040-060-port, built per its BUILDING.md with jusii/amix-kerntools against our own AMIX 2.1c tree; it binary-patches the stock `stand/unix`, relinks HAT and pstart overrides, changes the page size from 2 KB to 4 KB, adds Motorola FPSP/060SP from NetBSD sources, copyback caches). It replaces the local partial series `amix/0001`–`0009`. SORIX-LDOM's loader must follow its loader's contract (github.com/asokero/amix-unix-boot, `unix_boot040`): poke the kernel's `cputype` global (40/60), PC-relative relocations (FPSP kernels), bind at `0x08000000` with accelerator RAM, MMU off via `movec` | The port's kernel boots natively to login on the Z3660 68060 in the A3000 through `boot -native`, and on 030 FS-UAE the stock kernel still does. With 4 KB pages the 68040/68060 shadow MMU maps AMIX pages one to one (unblocks M6) |
| **A2** | `z3660scsi` for AMIX: use github.com/jusii/amix-z3660scsi (with the Z3660-amix firmware, github.com/jusii/Z3660-amix branch `release-1.0`) | AMIX root and swap on Z3660 HDFs, native and under SORIX-LDOM |
| **A3** | `z3660net` for AMIX: use github.com/jusii/amix-z3660net (`zen0`) | `ping`, `telnet`, NFS from AMIX on the Z3660 |
| **A4** | `z3660rtg` for AMIX: use github.com/jusii/amix-z3660rtg with X11 (isoriano1968/x11r6.3-amix, asokero/xrtg-amix) | AMIX console/X11 on the Z3660 RTG output |

Source: amigaux.org (amigaux 1.0, 2026-10-14) and its GitHub list. Its
hardware page lists the Z3660 with a real 68060/68LC060 as supported and
the Z3660's 68030 and emulated 68040 modes as not supported (68030 mode
planned for 1.1); the A3000 was verified with an earlier build only.
Under SORIX-LDOM, the same drivers drive pass-through devices; the
Z3660's register windows must be mapped for the guest.

Real-hardware test platforms (all on the A3000):
- 68030: the A3000's own onboard 68030 (accelerator off). This is where
  the 030 paths (M3–M5, deprivileged AMIX with the stock kernel) are
  confirmed, with the AMIX disk on a ZuluSCSI at the A3000's onboard SCSI
  controller (SORIX-LDOM's WD33C93 driver; AMIX's own A3000 SCSI driver).
- 68040/68060: a physical CPU (the Z3660's real 68060), or the Z3660's
  emulated CPU once an emulation with a working MMU is available (the
  boot-time MMU self-test decides; today's emu040 mode is not supported
  by amigaux for AMIX).

Notes:
- A1 is the prerequisite for AMIX on the 68040/68060 platforms; A2 for
  the Z3660's disks there.
- Device ownership: when SORIX-LDOM's own web console (M9) uses the Z3660
  network interface, AMIX's `z3660net` (A3) cannot drive it at the same
  time; one of them owns it (§6.2), or SORIX-LDOM offers AMIX a
  paravirtual network device.

M1 + M2 alone already give a useful product: a debuggable, serial-traced boot
manager for AMIX/AmigaOS on the A3000. The microkernel property (guests
under SORIX-LDOM's control, break-in and resume) starts with M3.

The hard disk boot of SORIX-LDOM itself was moved from M1 to M10: it is
only worth doing once everything works from the boot floppy.

Status (2026-10-15): M0 done and tested on a real A3000 (Z3660). M1 floppy
part done: takeover, shell, boot log, screen console, keyboard, CPU probing.
Disks done early: partitions through Kickstart's drivers (any controller),
native A3000 SCSI, `part`/`ls`, `boot dh0` (declining floppy boot block;
on real hardware still being verified). Native Z3660 piscsi driver written
(next to the A3000 SCSI driver), awaiting a test on the real machine.
M2 done in FS-UAE: UFS reader; `boot unix_root:[path]` loads
/stand/unix from the UFS root, relocates it and starts AMIX natively; AMIX
2.1 reaches multi-user login (68030). Open: the test on the real A3000
(Z3660 driver, a CPU mode with a 68030 MMU for the 2.1 kernel). Loading
through Kickstart's drivers was dropped (see §7.2). `boot dh0` on the real
A3000 works; the recoverable boot alert is filtered.
M3 in progress: virtual CPU core (trap-and-emulate of all §4.3
instructions for 030/040/060, exception and interrupt reflection, real IPL
= virtual IPL, hypercalls), pvtest conformance guest (23 checks) passing on
030/040/060, break-in with Ctrl-C and `resume` working. Break-in is checked
on the serial interrupt and on every guest exception; a silent guest spinning
with interrupts masked can only be stopped by the level 7 interrupt (to do).
MMU self-test done (boot time; guests refused without a working MMU;
passes on 030/040/060 in FS-UAE, correctly fails on a 68EC030).
Idle on STOP done: the CPU stops at the guest's interrupt level and the
waking interrupt is delivered to the guest (serial input stays
SORIX-LDOM's: Ctrl-C breaks in during idle too).
GDB stub done: RSP on the Paula port for the stopped guest (`gdb` command;
target.xml, g/G/p/P, m/M fault-safe, Z0 breakpoints as ILLEGAL, step via
T1, continue, detach, kill, noack, guest console as `O` packets); `make test`
drives gdb-multiarch against pvtest on 030/040/060. Not yet: stub for pvmk
itself, `$`-packet auto switch, `monitor` commands, watchpoints.
M3 complete in FS-UAE; real-A3000 confirmation pending.
M4 in progress (68030): `boot -pv unix_root:` starts the unmodified AMIX
kernel deprivileged. Done: 68030 shadow MMU (core/vmmu.c: guest tables
walked in software incl. long/short/indirect descriptors, limits, S/WP,
U/M bits, guest TT0/TT1; fixed 8/8/5/2K shadow tables; kernel and user
views with an ATC flush on every view change; PFLUSH and TC/CRP/SRP/TT
changes drop the shadow; PTEST answered from the walk; the hole is never
mapped), guest bus errors (68030 frames A/B reflected with the guest's
function code, entered through a fresh frame; the guest's RTE hands the
frame back to the CPU), faults inside emulated instructions (synthetic
restartable format A frame), full 68020 extension words, interrupts that
arrive while SORIX-LDOM handles a trap, and the first PV patch: AMIX's
lcopyin/lcopyout replaced at load time by HC_COPYIN/HC_COPYOUT (same
u_caddrflt contract; the kernel on disk is unchanged and still boots
natively). AMIX's memory list ends below the hole.
Found on the way: FS-UAE's 68882 saves an idle frame after a null
FRESTORE (a real 68881/2 saves a null frame); AMIX's chk_fpu then
overruns its 8-byte reset_fsave and later panics in hat_pt2ptdat. The
emulated FSAVE gives the documented null frame in that case.
Debugging aids: `guest log` (AMIX's putbuf), `pt` (guest table walk),
`md -g` (guest virtual), `watch` (guest writes, also behind the MMU),
first reflected exceptions and guest faults in `guest`.
Halt and reboot: AMIX's haltsys is patched to HC_HALT (and a guest RESET
counts as a reboot): halt stops the guest, reboot loads the kernel again
and restarts it deprivileged (`boot -pv` target remembered).
Break-in from the Amiga keyboard: Ctrl + Left Amiga + Help (FS-UAE on a
PC: Left Ctrl + Left Windows + End). SORIX-LDOM snoops CIA-A's serial
data register on the guest's keyboard interrupts (non-destructive), stops
the guest and puts its console on the screen and keyboard; its chip RAM
block and DMACON are saved, and `resume` gives them back with AMIX's
copper list (ExecingCopList). Serial Ctrl-C works as before.
DMAEN stays on when a kernel is started (AMIX enables bitplane and copper
DMA but relies on Kickstart's master enable).
State: AMIX boots deprivileged in FS-UAE (030) to multi-user and login;
fsck, login, su, shutdown and reboot work; break-in and resume work from
serial and from the keyboard. Slow: about 22,000 emulated privileged
instructions per second, mostly inlined spl sequences, and every AMIX
flushmmu rebuilds the shadow. Open in M4: the test on the A3000's
onboard 68030. Performance (§7.3 spl patching, keeping the kernel view
across flushes) is M5.
The 68040/68060 cannot map AMIX's 2 KB pages one to one (4/8 KB pages
only, VM code object-only): deprivileged AMIX there needs another approach
(M6); the 68030 path works as designed.

---

## 12. Risks and open questions

| Risk | Mitigation |
|---|---|
| Trap overhead on 030 @ 25 MHz (≈2–4 µs per emulated instruction) with ~1.7 k inlined spl sites in hot paths | Measure in M4; §7.3 patching; `boot -native` fallback |
| AMIX HAT updating live PTEs without `PFLUSH` | Trace in M4; write-protect page-table pages fallback |
| Timing-sensitive pass-through drivers (floppy MFM, serial, audio) under trap latency | Keep them native-fast: interrupts reflected without decode; IPL via real SR |
| No IOMMU: guest DMA can overwrite pvmk | Identity mapping + pvmk hole excluded from guest memory lists; accept (cooperative guests), document |
| SuperKickstart A3000s (Kickstart in MMU-protected RAM) | Capture image at stage0; handle `$F80000` RAM write-protect state |
| Serial port shared by shell, log, GDB, guest | Explicit `serial=` policy + `$`-packet mode switch; PV console for guest |
| 060 errata / revision differences | Log revision, set `PCR` conservatively, follow 060 support package guidance |
| FPSP/ISP licensing | Motorola released them for free redistribution (used in NetBSD/Linux) — keep their notices; verify before shipping |
| AMIX object code assumptions about real supervisor mode (e.g. reading `SR` S-bit to detect context) | Covered: `MOVE from SR` is privileged on 010+, emulated with vS |
| Floppy image unusable on real HW (PC drive can't write Amiga format; marginal disks) | Document Greaseweazle/KryoFlux/on-Amiga writing; `mkadf --verify`; boot block CRC check with a specific alert; keep image small (≤ 200 KB) so few tracks are needed |
| `pvmk` file moved by AmigaDOS after editing the disk | PVBB CRC check fails clearly; `pvinstall DF0:` repairs; only `pvmk.conf` is meant to be edited |
| 040/060 copyback cache after the boot block loads code | `CacheClearU()` before jumping, in both the boot block and stage0 module loads |
| Kickstart memory probe vs. pvmk hole | Verify on 2.04/3.1 in M7; fall back to placing pvmk in accelerator RAM |

Open questions to settle before M3:
1. Is an A3640/060 card available for real-HW testing, or emulator-only for M6?
2. Should the shell also run on the A3000 screen (needs a tiny ECS text
   console, ~6 KB) or is serial-only acceptable?
3. Preferred hypercall vector: `TRAP #15` conflicts with any AMIX debugger
   usage? (check `os/exp` vector users before freezing the ABI).
