# AMIX on an A2500/030 (A2630 + BigRAM, A2091 + ZuluSCSI): evaluation

Desk study, 2026-11-06. Nothing was run on the A3000 or in FS-UAE. Nothing is committed.
Tags: **[V]** verified in code or disassembly (file:line given), **[D]** vendor/hardware
documentation, **[I]** inferred, to be checked on the emulator or the machine.

Target machine: A2000 (A2500) + A2630 (68030 25 MHz + 68882, 2/4 MB) + BigRAM on the
A2630's expansion connector + A2091 (WD33C93A + Commodore DMAC) with a ZuluSCSI + native
OCS/ECS display, Kickstart 3.2, no Z3660, no RTG.

## Verdicts

| question | verdict |
|---|---|
| 1. Does pvmk see the BigRAM and hand it to AMIX? | **Yes, as it stands.** Kickstart lists it, pvmk copies the list into bootinfo, binds the kernel at the start of it and puts its own hole at its top. AMIX then uses **only** that one region (the A2630's own 4 MB is dropped). With the **stock** kernel, plan on capping it (`-m #16` .. `#64`) until the ceiling is found: a 4 MB kvseg cannot describe 112 MB of 2 KB pages. |
| 2a. Does our current `/stand/unix` (z3l chain) boot on the 68030? | **No.** The image executes 68040 opcodes from its first instructions (`cinva`, `movec %itt0/%urp/%srp`, `cpusha`, 040 `pflusha`) and its whole HAT builds 68040-format tables. There is no 68030 path left in it. pvmk itself refuses it on a 68030 (`kernel_fits`). |
| 2b. Can pvmk boot *a* core AMIX on the A2500? | **Yes, with the stock Commodore 2.1 kernel** (the 68020/68030 kernel the port is made from): it has the A2091 driver with a chip-RAM bounce buffer for DMA above 16 MB, roots on SCSI ID 6 by default, and pvmk's native handoff has booted it on a 68030 in FS-UAE. Today it must come over serial (`boot -m #16 ser:#<size>`), because pvmk has **no A2091 disk driver** (only A3000 SDMAC + Z3660). |
| 3. A2091 + ZuluSCSI | Kernel side: **yes** (stock `a2091queue`/`a2091intr`, still present in every chain image). pvmk side: **needs a driver** (`drv/a2091.c`), about one file, register map known exactly. |
| 4. Console without RTG | **Yes** (stock native-chipset console, keyboard; login on the serial port via ttymon). Kernel messages go to the screen only unless a serial-mirror overlay is linked. |

**On the user's statement "our kernel should already support all versions":** the evidence does
not support it for the 68030 (section 2). The port README itself says what the image is: "a
self-contained bootable 040/060 kernel" (`relink-040.sh` header), and "Support for Z3660
(emulated 030/040)" in the README refers to the Z3660 line generally, not to a test of the port
image on a 030; no acceptance document records one. A single image for 030..060 is possible
(section 2.3), but it is new work, not a configuration of what exists.

---

## 0. Hardware facts first: which BigRAM

- **"BigRAM Plus"** (Individual Computers, 2012) is a **Zorro III** memory card for the
  A3000/A4000. An A2000 has no Zorro III, so it cannot be in this machine. **[D]**
  (amiga.resource.cx/exp/bigramplus)
- The card that sits **on the A2630** is **BigRAM2630** (also iComp). This document assumes
  that card. **[D]**

BigRAM2630, from the vendor register page (wiki.icomp.de/wiki/BigRAM2630_registers) **[D]**:

| range | use |
|---|---|
| `$0100.0000-$07FF.FFFF` | **112 MB of 32-bit fast RAM**, on the A2630 local bus (not Zorro) |
| `$00C0.0000-$00DB.FFFF` | optional 1.8 MB "trapdoor" RAM, **off by default**, only for systems without DMA Zorro cards: keep it off (A2091) |
| `$00E9.0000-$00E9.FFFF` | autoconfig nibbles + command/status/flash registers |
| `$00E0.0000`, `$00F0.0000`, `$00F9.0000` | MapROM / DiagROM windows, off by default |

- Autoconfig identity: manufacturer `$1212`, product 26, serial 2630 (2631 "reverse"), diag
  vector at `$4C00`. The A2630 connector lacks config lines, so the card waits for the A2630 to
  configure and then "sneaks" its nibbles into `$E8.0000`. One ConfigDev, a small I/O board. **[D]**
- The 112 MB is added to the free memory list by the card's own diag code during autoconfig
  ("no additional software installation"). So it is one Exec MemHeader `0x01000000-0x07FFFFFF`
  (minus the header), MEMF_FAST|MEMF_PUBLIC. **[D]/[I]** (attributes inferred)
- Rev 6 A2630 boards need a trace cut + wire to U501 pin 21; rev 9 work as delivered. A2630 boot
  ROM rev 6 or 7 is needed for Kickstart 2.0+. **[D]**
- The A2630's own 2/4 MB autoconfigures as a Zorro II memory board (514/81), normally at
  `0x00200000`. **[D]** (514/81) / **[I]** (address: standard Zorro II placement)
- So the machine has **three separate regions**: chip `0-0x0007FFFF/0x000FFFFF`, A2630
  `0x00200000-0x005FFFFF`, BigRAM `0x01000000-0x07FFFFFF`. A2630 and BigRAM are **not**
  contiguous (Zorro II I/O, chip registers and ROM lie between). **[I]**
- **The A2091 is a 24-bit Zorro II DMA master: it cannot reach the BigRAM.** Every disk
  transfer into or out of BigRAM must be bounced (pvmk and AMIX both do, see 3). Under AmigaOS,
  the A2091 partitions need Mask `0x00FFFFFE` (HDToolBox) or FFS buffers in BigRAM corrupt
  data. **[I]** (standard A2091-with-32-bit-RAM practice)

## 1. Memory: pvmk -> bootinfo -> AMIX

### 1.1 pvmk sees it **[V]**
- stage0 reads Kickstart's MemList into `bootcap.mem[]` / `bootcap.memhdr[]` (up to
  `BC_MAXMEM` = 16, `include/pvmk/bootcap.h:10,75`) and the ConfigDevs into `bootcap.configdev[]`.
- `reserve_hole()` (`stage0/stage0.c` ~176-220) puts pvmk's hole at the top of the largest
  MEMF_FAST region **not on an autoconfigured board** (pass 0). BigRAM's RAM is not inside its
  ConfigDev (the board is the `$E9xxxx` register window), so the hole goes to the top of
  BigRAM, ~`0x07Fxxxxx`. **[V]** code / **[I]** ConfigDev size
- `amix_bind_address()` (`svc/amixload.c:92-113`) binds the kernel at the start of the largest
  non-chip region below `0x40000000` = **`0x01000000`**. The A2630's 4 MB loses (smaller).
- `amix_start()` (`svc/shell.c` ~846-870) copies all MemHeaders and ConfigDevs into
  `bootinfo.memory[]`/`autocon[]` (`AMIX_NAUTO` = 16) and, for `boot -m N` / `mem=N`, shrinks
  only the region holding the kernel to N MB. Native start leaves the hole region untrimmed
  (AMIX owns the machine); `-pv` trims it at the hole.

### 1.2 What AMIX uses **[V]**
`config()` (`.text 0x18f5c`) builds `MAINSTORE`/`VSIZOFMEM` from the **one** region that
contains the kernel and drops the rest (`amix-040-060-port/docs/MEMORY-REGIONS-PLAN.md` §1,
`docs/boot-path-map.md` §1b). Here: BigRAM from `0x01000000`. The A2630's 4 MB is unused; chip
RAM is used separately (display, A2091 bounce buffers).

### 1.3 Address-layout compatibility
- Stock 030 kernel: level-A entry 0 is an early-terminating identity map of
  `0x00000000-0x3FFFFFFF`, entry 1 the kernel's VA `0x40000000-0x7FFFFFFF` (2 KB pages),
  entries 2/3 CI I/O at `0x80000000`/`0xC0000000` (`docs/mmu-format-030-to-040.md` §1.3) **[V]**.
  BigRAM at `0x01000000` lies inside the identity window: fine. There is no fixed
  "kernel text at 0x08000000": that is only where the A3000/Z3660 RAM starts; the kernel is a
  relocatable ELF bound by pvmk. **[V]**
- The 030 uses no ITT/DTT here; 040 TT registers (`ITT0/DTT0/DTT1`) are only in the port's
  `pstart040.s`. The 030's own TT0/TT1 (`pmove`) are touched only by the old loader. **[V]**
- No Zorro III, so the `0x40000000` collision of the A3000 (MEMORY-REGIONS-PLAN §3) does not
  arise. **[V]**

### 1.4 The stock kernel's memory ceiling **[I], important**
`src/kvseg040.s` header: with the stock 4 MB kvseg, 128 MB of **4 KB** pages already starve
kvseg (page array 1.9 MB + boot allocations; first big `kmem_alloc` sleeps forever). The stock
030 kernel uses **2 KB** pages, so 112 MB means ~57 000 page_t (x60 B = ~3.4 MB) in the same
4 MB kvseg. Expect a hang or "No space" panic well below 112 MB. The A2500UX shipped with a few
MB, so this was never exercised. **Cap with `-m` and find the ceiling in steps** (16, 32, 48,
64). Growing kvseg for the 030 would need a 030 version of `patch_kvseg.py` (its sysseginit
loop patch is written for the 040 table layout).

## 2. The kernel on a 68030

### 2.1 The current image cannot run on a 030 **[V]**
First 040-only instructions in boot order:

1. `_start` -> `config_cachefix` (`src/config040.s:37`, linked unconditionally by
   `relink-040.sh:363,554,1025`): `.word 0xf498` = **`cinva ic`**. On a 68030 that is an F-line
   opcode for coprocessor id 2 -> Line-F exception (vector 11) before the kernel has its own
   vectors. This is the first thing the kernel does.
2. `pstart040.s:318-347`: `movec %srp/%urp` (0x807/0x806), `movec %itt0/%itt1/%dtt0/%dtt1`
   (0x004-0x007), `cpusha bc` (0xf4f8), 040 `pflusha` (0xf518), `movec %tc` with the 040
   format. The 030 has none of these control registers (illegal instruction) or opcodes.
3. Everything after: `hat040.s` (`movec %urp`), `runtime040.s` resume/fault paths (`cpusha`,
   `cinva`, `pflusha`), `getfault040`/`userspace040`/`wb040` decode the 040 **format-7** frame,
   `patch_pmmu_040.py`/`patch_pflusha_040.py` replaced the stock 030 `ptest`/`pmove`/`pflusha`
   sites in place, and Model B moved the whole VM to 4 KB pages with 040 descriptors.
   The only CPU gate in the image is `cputype == 60` around `movec %pcr` (`pstart040.s:403`).
4. pvmk knows: `kernel_fits()` (`svc/shell.c:1266-1279`) refuses a kernel with a `cputype`
   symbol on a CPU below 68040 ("this is a 68040/68060 kernel (cputype); the CPU is a 68030");
   `boot -f` would force it into the Line-F trap above.

The CACR value of the chain (`patch-kernel-cacr.py ... a0c08000`), PCR handling and the FPSP/060SP
pieces are 040/060 matters and do not apply to a 030; on the 030 the stock values
`sup_cacr=0x1019`, `cacr=0x3919` are the valid ones. The 68882 needs nothing (stock AMIX
requires a 6888x; the A2630 has one). **[V]** (`pstart040.s` comment ~374, README "Does AMIX need an FPU?")

### 2.2 Overlays that would carry over to a 030 image
CPU-neutral in principle: `random` (C driver + cdevsw patch), `paula`/`audio` (C + byte
patches), `shm` (shminfo patch). But all relink scripts compile against the **Model B**
(4 KB page) sysroot and run `check_page_geometry`, so they need a stock-geometry (2 KB) build.
`ksegtbl`, `kvseg`, `memregions`, `vt040`, `z3660*`, `zz9000*` are 040-layout specific or
absent hardware. **[V]** (script inputs) / **[I]** (portability)

### 2.3 What "one kernel for 030..060" would take (option B, not proposed for bring-up)
A cputype-gated 030 path for: `config_cachefix` (030 CACR clear instead of `cinva`), `pstart`
(030 tables with 4 KB pages, TC PS=12, `pmove`), every `hat*040` routine (030 descriptor format,
`pflush`/`pmove %crp`), `resume040`/runtime cache and ATC flushes, fault decoding of 030 format
A/B frames (the stock handlers can stay for the 030), `vtop`/`ptest` (030 `ptest`+`pmove %psr`).
Roughly the size of the 040 HAT port itself. The 030 cannot share the 040 table format, so the
VM layer must exist twice. That is why a separate image is forced for the first boot.

### 2.4 Option A (recommended for bring-up): the stock 030 kernel
`amix-root/stand/unix` (2.1p2a, sha256 `c3feede0...`, layout-identical to 2.1c) **is** the 68030
kernel. It is not a new build: it is the input of the port chain, unmodified. It already has
the A2091 and A2090 drivers and a default root of card 0 unit 6. pvmk recognises it (no
`cputype`) and boots it natively on a 68030 (milestones M2-M4 in FS-UAE, 030, A3000 model).
**[V]** (symbols below; pvmk PLAN.md status)

## 3. A2091 + ZuluSCSI

### 3.1 Kernel side: present **[V]**
- Stock and every chain image contain `a2091queue` (`.text 0xc714`) and `a2091intr`
  (`0xc8a0`); `scsicard[]` row 2 = `0x02020003 -> a2091queue`
  (`docs/Z3660-KERNEL-FEASIBILITY-260731.md` §3). `nm build/z3l/v1106-ksg` still lists them:
  "no drivers for absent hardware" removed Hydra/A2065 interrupt glue, not the SCSI rows.
- **24-bit DMA is handled.** Stock `startdma` (`0xccb0`): if the buffer address is
  `> 0x00FFFFFF` it calls `AllocMem(len, 2)` (chip), `bcopy`s write data into it, DMAs from
  there; `stopdma` (`0xcd3c`) copies read data back and `FreeMem`s. If chip allocation fails:
  `panic("a2091: no chip mem")` (string at `0xcc04`). So BigRAM works, slowly, and chip RAM
  pressure is a panic risk.
- Register use in the stock driver matches the Linux `a2091.h` layout: ISTR `+0x40`
  (FE bit 0), CNTR `+0x42` (0x30 read / 0x38 write = PDMD|INTEN[|DDIR]), ACR `+0x84`,
  ST_DMA `+0xE0`, SP_DMA `+0xE2`, CINT `+0xE4`, FLUSH `+0xE8`; SASR/SCMD `+0x91/+0x93`.
- Root: stock `rootdev = 0x00480016` = `c6d0s1`, swap `/dev/dsk/c6d0s2` = card 0, unit 6
  (`tools/stamp-rootcard.py` docstring). With the A2091 as the only SCSI card it is card 0.
  **Put the ZuluSCSI at ID 6** and the stock kernel needs no stamping. **[V]** values /
  **[I]** card-0 numbering of the A2091 alone
- The chain image is stamped card 1 unit 0 (Z3660); irrelevant now, since it cannot run on the 030.

### 3.2 pvmk side: missing **[V]**
- `drv/wd33c93.c` is the A3000 controller only: `SDMAC 0xdd0000`, SDMAC offsets
  (`DAWR 0x02, CNTR 0x0a, ACR 0x0c, ... SASR 0x41`), SDMAC CNTR bits. `core/stage1.c:53-63`
  probes it blindly, then `z3660_init()`. `svc/rdb.c:266-283` builds partitions only from those
  two. So on the A2500 pvmk finds **no partitions**: `boot unix_root:` cannot work.
  PLAN.md:282 lists "A2091" as later work.
- The blind SDMAC probe writes to `0xDD00xx` on an A2000, where nothing is decoded. It should
  time out ("no A3000 internal SCSI controller"); whether the A2000/A2630 bus answers with a bus
  error instead is untested. **[I]**
- What works today without changes: pvmk boots from floppy (ADF) or the PVMK partition through
  the A2091's ROM (`scsi.device` at boot is Kickstart's), and `boot -m #N ser:#<bytes>` receives
  the kernel over serial (~2.5 min / 1.9 MB at 115200, memory note z3660-real-060-amix).
  The kernel then reaches its root with its **own** A2091 driver.

`drv/a2091.c` (proposed): copy of `wd33c93.c` with the A2091 DMAC map above, base address from
the ConfigDev of 514/3 (A2091; 514/2 = A590), A2091 CNTR bit values, same chip-RAM bounce buffer
(already below 16 MB, so no extra 24-bit logic), `NATIVE_A2091` partitions in `rdb_scan`, and
the SDMAC probe gated on "is this an A3000" (no 514/3 board, or Ramsey present). About 300 lines,
testable in FS-UAE.

### 3.3 ZuluSCSI
- Image name `HD6_<name>.hda` (or `.img`) on the SD root = SCSI ID 6, 512-byte sectors. The
  PC already has `~/Documents/FS-UAE/Hard Drives/HD6_AMIX_AMIGA3000.img` (the A3000 030 test disk
  name scheme). An RDB HDF is a raw disk image and can be copied as is. **[I]**
- A2091 with ROM 7.0 recommended; terminate the ZuluSCSI (only device). **[I]**

## 4. Console and display
- Stock kernel: native-chipset console and keyboard (`figuredisplaytype()` in `config`), which
  is what the A2500UX used. OCS/ECS and 512 KB/1 MB chip RAM are fine for the text console;
  chip RAM is also the A2091 bounce pool. **[V]** code path / **[I]** behaviour on this machine
- Serial: login via ttymon on the serial port, as on the A3000 (memory note
  amix-serial-login-transfer); kernel messages appear on serial only with a serial-mirror
  overlay (`src/serdbg.s` exists for the stock 030 kernel, used by `relink-030-dbg.sh`; a quiet
  030 variant would be a short new script). **[V]** files / **[I]** script
- The `vt` overlay, zz9000/RTG, Z3660 core-1 GPU: absent hardware, not in a core system.
- pvmk itself: chip display + keyboard + Paula serial are machine-neutral; pvmk is built
  `-m68020-60` and its `make test` runs on 030 (FS-UAE). **[V]**

## 5. Bring-up plan, in small steps

Per kernel-boot-step-by-step: the disk is a copy, the first boot uses a small memory cap, one
change at a time, memtest before each new range.

**Prerequisites (user):** Kickstart 3.2 ROM for A500/A600/A2000 (only A1200/A3000 ROMs are in
`~/Documents/FS-UAE/Kickstarts`), A2630 boot ROM rev 6/7 image, A2091 ROM 7.0 image (for the
emulator). A2630 rev 9 or patched rev 6 for BigRAM2630. BigRAM trapdoor RAM off.

**Step 0, disk image (PC only).** Start from a stock-userland AMIX with root on ID 6: a **copy**
of `AMIGA_UNIX.hdf` (FS-UAE test disk, stock 2.1 kernel, root on `scsi6`) or of
`HD6_AMIX_AMIGA3000.img`. Check `/stand/unix` on it is the stock 030 kernel (sha256 above or
the 2.1c one). Packages built `-m68060` and lib060 (mem* may use `move16`, 040+ only) stay off
this disk. Add a pvmk.conf for this machine: `mem=16`, `entry=AMIX,UNIX_Root:/stand/unix,native`
(the entry needs step 2), `keymap=de`.

**Step 1, emulator: pvmk on an A2000-class machine (no code change).** FS-UAE, A500/A2000
chipset, KS 3.2 A2000 ROM, 030+68882+MMU, A2630 board and A2091 via WinUAE pass-through keys
(`uae_cpuboard_type=A2630`, its ROM, `uae_cpuboardmem1_size=4`, A2091 controller + ROM; exact
key names in FS-UAE 3.2.35 to verify). Boot pvmk from ADF. Check: stage0 board list (514/81,
514/3), memory list, SDMAC probe result, `memtest`, hole placement, `kernel_fits` refusing the
z3l kernel.
Emulating the BigRAM at `0x01000000` exactly is probably not possible (WinUAE has no BigRAM2630;
its "motherboard fast low" RAM ends at `0x07FFFFFF` but KS 3.2's A2000 ROM may not size it).
If it cannot be had, an A3000-model run with an A2091 added still tests >16 MB bounce (A3000
fast RAM is at `0x07xxxxxx`), with the A3091 as card 0 and the A2091 as card 1 (stamp root
accordingly). **[I]**

**Step 2, pvmk A2091 driver** (`drv/a2091.c`, gated SDMAC probe, rdb hookup). `make check`,
`make test` (030/040/060 unchanged), then the step-1 config: `scsi`, `part`, `ls unix_root:/stand`,
`load unix_root:`.

**Step 3, emulator: stock kernel, native, capped.** `boot -m #16 unix_root:/stand/unix`
(or `ser:` before step 2). Expect: "Total Unix memory" ~16 MB, A2091 initialised, root c6d0s1,
login on screen and serial. `fsck -n`, shut down cleanly.

**Step 4, emulator: the ceiling.** `-m #32`, `#48`, `#64`, `#112`, one per boot. Record where
kvseg runs out (hang at first fork, or panic). Heavy disk I/O at each step (tar of /usr) to load
the chip bounce path.

**Step 5, real A2500.** Fresh copy of the image on the ZuluSCSI SD (ID 6). Serial capture at
115200 8N1 *before* booting pvmk (serial-before-reboot). pvmk from floppy; `memtest` BigRAM in
chunks; then exactly the step 3/4 sequence, starting at `-m #16`.

**Step 6 (later, separate decisions):** 030 relinks of the CPU-neutral overlays (random, paula,
audio, quiet serial mirror) on the stock geometry; a 030 kvseg patch if the ceiling is too low;
PV mode (`-pv`) on the real 030; option B (single 030..060 image) as its own project.

## 6. Risks and blockers, ranked

1. **Blocker: the z3l chain kernel is 68040/68060-only** (2.1). No 030 code path; first fault
   is `cinva ic` in `config_cachefix`. Use the stock 030 kernel, or start option B.
2. **Blocker for disk boot: no A2091 driver in pvmk** (3.2). Workaround: serial kernel load.
3. **Stock kernel memory ceiling** (1.4): 112 MB will not fit a 4 MB kvseg with 2 KB pages.
   Cap with `-m`; 030 kvseg patch if needed.
4. **A2091 bounce through chip RAM** (3.1): every transfer to/from BigRAM is copied; a failed
   chip allocation panics ("a2091: no chip mem"). With 512 KB chip RAM and an X/console bitmap
   this is a real risk; 1 MB chip helps.
5. **The A2630's 4 MB is wasted** (one-region limit). The alternative, kernel in the A2630
   region (no bounce, 4 MB only), needs a pvmk option to choose the region. The memregions work
   (MEMORY-REGIONS-PLAN) is for the 040 line.
6. **Emulator gap**: BigRAM2630 placement and A2630/A2091 options in FS-UAE unverified (5, step 1).
7. **Blind SDMAC probe on an A2000** (3.2): probably harmless; gate it anyway.
8. **AmigaOS side**: A2091 partitions without Mask `0x00FFFFFE` corrupt data once BigRAM is in
   the free pool; check before installing anything under AmigaOS on this machine.
9. Userland built for the 060 (lib060 `move16`, tuned packages): keep off the core disk.

## Sources
- pvmk: `svc/shell.c` (678-679, 686-870, 1266-1366), `svc/amixload.c:92-113`,
  `stage0/stage0.c` (`reserve_hole`, `rom_scan`), `core/stage1.c:53-63`, `drv/wd33c93.c`,
  `svc/rdb.c:266-283`, `include/pvmk/{scsi,bootcap,amix}.h`, `PLAN.md:282`.
- Port (`upstream/amix-040-060-port`, branch z3660-amix-a3000): `chain-z3660-a3000.sh`,
  `relink-040.sh`, `src/config040.s`, `src/pstart040.s`, `src/kvseg040.s`,
  `src/patch_pmmu_040.py`, `tools/stamp-rootcard.py`, `docs/MEMORY-REGIONS-PLAN.md`,
  `docs/mmu-format-030-to-040.md`, `docs/boot-path-map.md`,
  `docs/Z3660-KERNEL-FEASIBILITY-260731.md`, README.
- Stock kernel `upstream/amix-root/stand/unix` disassembled (`m68k-linux-gnu-objdump -dr`,
  `.text 0xc714-0xcf70`); `nm build/z3l/v1106-ksg`.
- BigRAM2630: https://wiki.icomp.de/wiki/BigRAM2630 , https://wiki.icomp.de/wiki/BigRAM2630_registers ;
  BigRAM Plus: https://amiga.resource.cx/exp/bigramplus ; A2630: https://amiga.resource.cx/exp/a2630
