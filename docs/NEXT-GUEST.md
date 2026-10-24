# NeXT guest for pvmk — design note (draft)

Status: draft, 2026-10-23. Nothing here is implemented.

## 1. Goal
Run a NeXT 68040 machine as a pvmk guest on the A3000 + Z3660 (68060 rev 6, 50 MHz), executing guest code natively on the 060 under the existing trap-and-emulate vCPU and shadow MMU, with NeXT devices supplied by models taken from the Previous emulator.
- First target (N1): the NeXT boot ROM Rev_2.5_v66 runs its POST and reaches the ROM monitor.
- Later: visible display and keyboard (N2), NeXTSTEP 3.3 booting from an HDF (N3).
Non-goals for now: Turbo and Color machines, NeXTdimension, DSP, sound, MO, floppy, printer, Ethernet.
Why pvmk rather than an AMIX kernel module, or Previous running interpreted on AMIX: pvmk already owns the machine and has the vCPU and shadow MMU for 040/060 guests. An AMIX-hosted VMM would rebuild both inside a binary-patched kernel, and every device exit would make a round trip through a user process. Interpreted Previous is estimated at 5–10 % of a real NeXTstation on this CPU; native execution should be at or above real speed once deprivileged-mode cache handling is fixed (§9).

## 2. Guest machine profile
NeXTstation, non-Turbo, mono, 68040 at 25 MHz, new clock chip (MCCS1850). Values measured on real machines (Previous sysReg.c header):
- SCR1 0x0200c000 = 0x00011102 (slab, board rev 1, 100 ns VRAM, 120 ns RAM, 25 MHz)
- SCR2 0x0200d000 = 0x00ff0c80 at reset (also carries the RTC/NVRAM serial lines)
- Interrupt mask 0x02007800: 0x80027640 OR-ed in on every write (non-maskable bits)
- Interrupt status 0x02007000: level from scr_get_interrupt_level()
ROM: Rev_2.5_v66.BIN, 128 KB, loaded from a file the user supplies (not embedded in pvmk).

## 3. Guest physical map and host backing (Previous cpu/memory.c, ioMemTabNEXT.c)
- 0x00000000–0x00FFFFFF boot ROM 128 KB, mirrored: host copy, read-only; writes dropped
- 0x01000000–0x01FFFFFF diagnostic ROM window: same backing
- 0x02000000–0x0201FFFF device space (+ mirror 0x02100000): unmapped → trap → device model
- 0x020C0000 BMAP (040 machines): trap → bmap.c
- 0x04000000–0x07FFFFFF RAM, 4 banks × 16 MB: populate banks 0–1 (32 MB) from Z3 RAM; absent banks → bus error (the ROM sizes memory this way)
- 0x0B000000 VRAM 256 KB (1120×832, 2 bpp, stride 288 bytes): host RAM mapped directly; dirty tracking for display
- 0x0C000000–0x0FFFFFFF VRAM write-function aliases (MWF0–3): trap → RMW emulation
- 0x10000000–0x1FFFFFFF RAM write-function aliases (MWF0–3): trap → RMW emulation
- 0xF0000000+ NeXTbus slot space: bus error (no boards); anything else: bus error
Host backing: guest RAM, VRAM and the ROM copy in the Z3660's 256 MB Z3 RAM at 0x40000000; guest addresses never coincide with host addresses, so all translation goes through the shadow MMU. MWF is rare in the ROM but heavy in Display PostScript; speed there is an N3 question.

## 4. vCPU: a 68040 on a 68060 (guest must never see an 060)
1. Exception frames: synthesize 040 frames via the guest VBR: format 0/2 for traps and interrupts, format 7 access-error frames for bus errors (absent banks, empty slots, unmapped space). No 060 format 4 frame may reach the guest.
2. MOVEC: shadow VBR, CACR, TC, URP, SRP, ITT0/1, DTT0/1, MMUSR, SFC, DFC, USP, MSP, ISP. Translate the guest's 040 CACR (DE/IE) to 060 bits. PCR/BUSCR must look absent.
3. MMU instructions: PFLUSH invalidates shadow entries; PTEST (040 form) walks guest tables and sets guest MMUSR; guest TC.E=0 means guest virtual = guest physical, still via shadow tables since guest physical ≠ host physical.
4. CINV/CPUSH trap already; do the host equivalent only where coherency needs it.
5. 060-missing integer instructions (64-bit MUL/DIV, MOVEP, CAS2, CHK2/CMP2) emulated in pvmk (ISP060 or equivalent); the guest never sees vector 61.
6. FPU: 060 unimplemented-FP / data-type traps handled in pvmk by FPSP060, invisibly; FSAVE/FRESTORE trap, present 040 frame formats (null/idle/busy), keep real 060 state internally.

## 5. Device access (MMIO) model
Device pages unmapped → 060 access fault → pvmk decodes the instruction (size, direction, EA, register, RMW) → calls the Previous handler through a shim mimicking Previous's IoMem: write = store value into shadow IoMem[] at the address, set IoAccessCurrentAddress, call *_Write; read = set address, call *_Read, take value from IoMem[] → pvmk completes the instruction (regs, CCR, PC) and resumes. Apply the table's mirror mask (2nd column) before lookup, like ioMem.c. Initial coverage: MOVE.B/W/L to/from register in the usual modes, CLR, TST, BTST/BSET/BCLR, AND/OR/EOR-immediate to memory, MOVEM; anything else logs the opcode (Previous cpu/disasm.c) and stops the guest.

## 6. Interrupts and time
- set_interrupt()/scr_get_interrupt_level() (sysReg.c) compute the NeXT IPL; replace M68000_CheckInterrupt() with "inject level-n interrupt into the vCPU when the guest SR mask allows".
- Previous device code already uses real microseconds (Timing_GetTime()) for the hardclock (0x02016000) and the 1 MHz 20-bit event counter (0x0201a000): back it with a pvmk µs clock.
- Replace CycInt_AddTimeEvent/UpdateTimeEvent/RemovePendingEvent with a small pvmk one-shot µs timer queue. N1 users: hardclock, 68 Hz VBL (video.c → dma_video_interrupt()), RTC, KMS.

## 7. Display
- N1 (debug): pvmk shell command e.g. "nextshot" sends VRAM over serial as PGM (1120×832, 4 grays, stride 288).
- N2 (native chipset): ECS Super-HiRes = 1280 px/line with 2 bitplanes = 4 colours = NeXT's 4 grays. Show a 1120×512 interlaced window of the 832 lines and scroll; convert 2 bpp chunky→planar for dirty lines only (write-protect VRAM pages, or compare to a copy at VBL).
- Later: Z3660 RTG at 1120×832 RGB565 (interface in amix-z3660rtg docs/FIRMWARE-RTG-INTERFACE.md).

## 8. Devices per milestone
N1: sysReg.c (SCR1/2, int regs, hardclock, event counter), rtcnvram.c (RTC/NVRAM via SCR2, MCCS1850), bmap.c, dma.c CSRs + video channel/VBL (video.c), kms.c (keyboard present), scc.c (if probed), brightness + memory-timing pass-through; ESP/MO/floppy/Ethernet/printer/DSP configured absent as Previous does.
N2: serial-terminal keystrokes → NeXT key events in kms.c; then Amiga keyboard (keymap.c for reference) and mouse.
N3: ESP SCSI + DMA (esp.c, dma.c, scsi.c) backed by an HDF via pvmk's piscsi driver.

## 9. Prerequisite: deprivileged-mode caching
Deprivileged guests run with the data cache off and every CPUSHL trapping; a NeXT guest would be equally slow. Not blocking for N1, needed before N2: data cache on with a TT layout keeping device space and guest device pages non-cacheable (DTT0 priority), CPUSH/CINV handled without a full trap where possible.

## 10. Code organisation
pvmk/guest/next/{previous/ (device files, minimal changes), shim/ (main.h, configuration.h, log.h, cycInt.h, ioMem.h, m68000.h replacements; no SDL, no threads, fixed config), next_vm.c (map, ROM load, MMIO dispatch, IRQ injection), next_display.c}. Record the Previous version (v4.4, July 2026). Licence: Previous is GPL-2.0-or-later; a pvmk binary containing it must be GPL-compatible — decide before importing.

## 11. Testing
FS-UAE first (add a NeXT-ROM case to make test on the 060 config). Reference traces: Previous v4.4 on the Mac, same ROM/config (NeXTstation mono, 32 MB, MCCS1850), device logging on; log the same events in pvmk (address, size, value, guest PC) and diff — the first divergence names the bug. Hardware only after FS-UAE passes, with serial capture.
N1 acceptance: POST completes without double fault or unhandled-instruction stop, device trace matches Previous up to the monitor, nextshot shows the ROM monitor.

## 12. Open questions for the pvmk side
1. Does the vCPU already shadow VBR, CACR, TTx and FSAVE/FRESTORE for 040 guests, or only what AMIX exercises?
2. How does the shadow MMU handle TC.E=0 and a guest physical map completely different from the host's (no identity regions)?
3. Is there an MMIO instruction decoder yet (e.g. from the AmigaOS-guest work), or is that new?
4. Which free-running and one-shot timers can pvmk give a guest?
5. Can pvmk deliver a synthetic format-7 access error to a guest today?
6. Is 32 MB + 256 KB VRAM + ROM in Z3 RAM free under pvmk?

## 13. Answers and corrections from the pvmk code (2026-10-23)

Read from the code as of pvmk faaa9b4 plus the uncommitted work since; nothing built for this.

### 13.1 The vCPU's personality is the host CPU's (§4, question 1)
- `movec()` (core/vcpu.c) shadows SFC, DFC, CACR, USP, VBR, MSP (not on the 060), ISP, TC, ITT0/1, DTT0/1, URP, SRP, and gates the rest **by the host model** (`cpuinfo.model`): on the 060 host MMUSR (0x805) is refused (illegal, as on a real 060) and PCR/BUSCR exist; CACR keeps only the bits the host CPU reads back (060: 0xf880e000), PCR's DFP/ESS go to the real PCR. So today a guest on the Z3660 *is* a 68060. A 68040 personality on a 060 host is new work: a guest CPU model separate from `cpuinfo.model`, used by `movec()` (MMUSR present, PCR/BUSCR illegal, 040 CACR bits DE/IE only), by frame construction and by FSAVE.
- FSAVE/FRESTORE are emulated (`fsave_frestore()`), but they produce the **host's** frames (always 12 bytes on the 060). 040 null/idle/busy frames: new.
- PTEST: the 040 form is caught only on a 040 host, and only clears MMUSR (a stub). On the 060 the opcode is unimplemented (line F). A real walk is easy to add: `vmmu_ptest()` already walks guest tables for the 030 path and `walk040()` returns MMUSR-style bits.
- CINV/CPUSH (§4.4): since 2026-10-18 they are executed as the guest wrote them, on the named line/page; an invalidate that could hit pvmk's own data is turned into a push. Each one still traps (§9).
- Unimplemented integer instructions (vector 61) and unimplemented FP (vectors 11, 55, 60 …) are **reflected to the guest** today (AMIX brings its own ISP/FPSP). pvmk has no ISP/FPSP of its own: §4.5/§4.6 are new; Motorola's 060SP (as the AMIX port links it, from NetBSD sources) is the natural source.
- Exceptions are reflected with the frame the host CPU produced (format 4 on the 060), or synthesized per host model (format A on the 030, 7 on the 040, 4 on the 060, for faulting emulated accesses). §4.1's "no format 4 ever reaches the guest" means a frame translator for every reflection path: new.

### 13.2 Shadow MMU and a non-identity guest physical map (question 2)
- Guest physical **is** host physical today. `walk040()` returns `w->pa` = guest physical, and the fill maps it 1:1; the only exception is the SORIX hole, which faults (MMUSR B). With TC.E=0 (or a guest TT match) the walk takes the `phys:` path with `pa = va`.
- For a NeXT guest the fill needs a guest-physical → host map (a p2m table of regions: ROM copy, RAM banks, VRAM in Z3 RAM; device space and absent banks → fault). That is one lookup in the fill path plus the region table: small and contained. TC.E=0 then works unchanged, since the walk already yields pa = va.
- Guest 040 tables, 4 KB and 8 KB pages, ITT/DTT matching and U/M updates are already handled by `walk040()`. Shadow pages are 4 KB (7/7/6). The shadow pool is a fixed 64 KB (`POOL_SIZE`); a 32 MB guest with VRAM will need it larger or recycled.

### 13.3 MMIO (question 3, §5)
There is no MMIO instruction decoder. The AmigaOS guest touches the custom chips directly (identity pass-through), and `moves()` decodes only MOVES. But pvmk already has the pieces for a decoder-less scheme, which I'd use instead of §5's full decoder:
- the access fault itself gives address, size and direction (060 FSLW: SIZE, R/W, RMW; 040 SSW);
- the watchpoint machinery (`vmmu_watch`, `vmmu_fault` returning 1 = "trace one instruction") already maps a page for exactly one instruction and single-steps it.
Read: call the device read for (address, size), map a scratch page at that VA holding the value, trace one instruction, unmap. Write: map the scratch page, trace, read back what was written, call the device write. RMW and MOVEM fall out of the same loop (one fault per access). This avoids decoding EA modes, CCR and PC entirely; the cost is two traps per device access, fine for N1. Keep the decoder idea only if profiling says so.
- Hook point: `vmmu_fault()` returns -1 ("the guest's fault") today for an unmapped page; the NeXT p2m lookup decides there whether it is MMIO, a bus error, or a miss.

### 13.4 Timers and interrupts (question 4, §6)
- pvmk itself has no timer service for guests. Its clock is `time_now()` from CIA-B TOD (hsync, 64 µs ticks, ms resolution). Guests today get the **real** Amiga interrupts passed through (real IPL = virtual IPL, `reflect()` at vector 24+level); `idle()` emulates STOP by stopping the real CPU until a real interrupt.
- A NeXT guest uses no Amiga chips, so pvmk can own them: CIA-A/B timers A/B (E clock 709379 Hz, 1.41 µs; one-shot or continuous) give the one-shot µs queue, Agnus VBL is 50 Hz (NeXT's 68 Hz VBL comes from a CIA timer instead), CIA TOD the free-running clock. Real Amiga interrupts must then be handled in pvmk and never reflected; NeXT levels are injected. The injection path exists for real interrupts (`reflect`) and for format-0 call injection (`vcpu_inject`); a pending-IRQ check on every guest SR/IPL change (MOVE to SR, RTE, STOP, spl patches) is needed: small.

### 13.5 Synthetic format 7 on the 060 host (question 5)
No. On a 040 host pvmk builds format-7 frames (for faulting emulated accesses); on the 060 host it builds format 4. A 040 bus-error frame for absent banks/slots is straightforward to construct (the ROM's memory sizing does not restart the access), but it is new code, and it belongs in the frame translator of 13.1.

### 13.6 Memory (question 6)
Yes. On the Z3660 pvmk now lives in CPU RAM (0x0ffb0000, it prefers non-Zorro RAM), so the 256 MB at 0x40000000 is unused by pvmk. 32 MB + 256 KB + 128 KB fit easily. Caveat: that is the memory AmigaOS/AMIX would also use; a NeXT guest is exclusive with them for now (one guest at a time).

### 13.7 Corrections to §1, §4–§6
- §1 speed: "at or above real speed" is optimistic. Every privileged instruction, RTE, MOVE SR and cache op traps (measured on the 060 with the data cache off: ~185 µs per trap). AMIX deprivileged is far slower than native today. With §9 fixed, trap cost should drop a lot, but trap-heavy code (the NeXT kernel's spl, exceptions, MMU work) will still be slower than real; AMIX got spl patching (line-A fast path) for exactly that.
- §4.2: PCR/BUSCR are currently present and MMUSR absent on the 060 host (13.1).
- §4.3: PTEST is a stub/illegal today (13.1).
- §4.4: already done as described (13.1).
- §4.5/4.6: not "handled invisibly" today; reflected (13.1).
- §5: suggest the decoder-less single-step scheme (13.3).
- §6: Amiga interrupts are passed through today; a NeXT guest needs pvmk to own and hide them (13.4).

### 13.8 Smallest first step towards N1 (proposal, not started)
"N1-0: a next guest that logs device accesses".
1. Guest type `next` in the shell (`next ser:<bytes>` or a ROM file on the PVMK volume): ROM copy, 32 MB RAM and 256 KB VRAM in Z3 RAM.
2. p2m region table in the shadow-MMU fill (13.2); TC.E=0 at reset; SSP/PC from the ROM's reset vectors.
3. Every device-space fault logged (guest PC, address, size, R/W, value via the single-step scheme of 13.3); reads return 0 or a small table (SCR1, SCR2, interrupt status) so POST gets past the first probes; absent RAM banks and slots → bus error (host-format frame for now).
4. Stop after N accesses or on the first unhandled exception; dump the log over serial for a diff against a Previous trace.
5. FS-UAE case in make test (060 config) with a fake 128 KB ROM that pokes a few device registers (no NeXT ROM in the repo).
Effort: 2–3 focused days for N1-0, including the FS-UAE test. The 040 personality (13.1: MOVEC/MMUSR/PCR, frame translation, FSAVE, PTEST, ISP/FPSP) another 1–2 weeks; device models through a Previous shim and timer/IRQ injection to reach the ROM monitor (N1) 2–4 weeks more, depending on how much of Previous is reused (licence, §10).
