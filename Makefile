# pvmk build.
#
#   make            build build/pvmk.adf (boot floppy) and verify it
#   make check      instruction check (68060-safe) + size budget + ADF verify
#   make run        boot the ADF in FS-UAE (UAE_CPU=030|040|060) with the serial
#                   console in this terminal (Ctrl-] quits)
#   make test       acceptance test in FS-UAE on 030/040/060 (shell, reset, logs)
#   make run-amix   boot in FS-UAE with a writable copy of your AMIX disk
#                   (build/amix-test.hdf); then: boot unix_root:
#   make toolchain  fetch the m68k cross toolchain into $(TOOLCHAIN_DIR) (no root)
#   make clean

TOOLCHAIN_DIR	?= $(HOME)/opt/m68k-cross
CROSS		?= $(TOOLCHAIN_DIR)/bin/m68k-linux-gnu-
CC		:= $(CROSS)gcc
LD		:= $(CROSS)ld
OBJCOPY		:= $(CROSS)objcopy
OBJDUMP		:= $(CROSS)objdump
READELF		:= $(CROSS)readelf
SIZE		:= $(CROSS)size
HOSTCC		?= cc
PYTHON		?= python3

VERSION		:= $(shell git describe --always --dirty 2>/dev/null || echo 0.0-dev)
B		:= build

# -m68020-60: runs on 68030/040/060 (no 64-bit MUL/DIV that the 060 lacks).
CPUFLAGS	:= -m68020-60 -msoft-float
CFLAGS		:= $(CPUFLAGS) -Os -std=gnu11 -ffreestanding -nostdinc -fno-pic \
		   -fno-pie -fno-builtin -fno-common -fno-asynchronous-unwind-tables \
		   -fomit-frame-pointer -Wall -Wextra -Werror \
		   -Wno-address-of-packed-member -fno-jump-tables -Iinclude \
		   -MMD -MP -DPVMK_VERSION='"$(VERSION)"'
ASFLAGS		:= $(CPUFLAGS) -MMD -MP -Iinclude -D__ASSEMBLER__
LDFLAGS		:= -T pvmk.ld --emit-relocs -nostdlib -no-pie --no-dynamic-linker --no-warn-rwx-segments
LIBGCC		:= $(shell $(CC) $(CPUFLAGS) -print-libgcc-file-name 2>/dev/null)
HOSTCFLAGS	:= -O2 -Wall -Wextra -Iinclude

# Size budgets (bytes), see PLAN.md section 10.
BUDGET_BOOTBLOCK := 960
BUDGET_IMAGE	 := 65536

PVMK_OBJS	:= $(B)/stage0/entry.o $(B)/stage0/stage0.o $(B)/stage0/libcall.o \
		   $(B)/stage0/takeover.o $(B)/stage0/alertpatch.o $(B)/core/stage1.o $(B)/core/vectors.o \
		   $(B)/core/exc.o $(B)/core/amixenter.o $(B)/core/vcpu.o \
		   $(B)/core/vcpu_s.o $(B)/guest/pvtest/blob.o $(B)/cpu/cpu.o $(B)/cpu/probe.o $(B)/cpu/mmutest.o \
		   $(B)/cpu/mmutest_s.o \
		   $(B)/drv/paula.o $(B)/drv/display.o $(B)/drv/keyboard.o $(B)/drv/wd33c93.o $(B)/drv/z3660scsi.o $(B)/svc/kprintf.o $(B)/svc/log.o \
		   $(B)/svc/ffs.o $(B)/svc/conf.o $(B)/svc/shell.o $(B)/svc/crc32.o \
		   $(B)/svc/disk.o $(B)/svc/rdb.o $(B)/svc/bootreq.o $(B)/svc/ufs.o $(B)/svc/amixload.o

ADF_FILES	:= -f pvmk.conf=conf/pvmk.conf -f README=conf/README.disk

.PHONY: all adf hdf run-amix check check-insns size-check verify run test toolchain clean
all: adf verify

adf: $(B)/pvmk.adf

$(B)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/%.o: %.S
	@mkdir -p $(@D)
	$(CC) $(ASFLAGS) -c $< -o $@

# Boot block: must be position independent, i.e. carry no relocations.
$(B)/bootblock.bin: $(B)/boot/bootblock.o
	@if $(READELF) -r $< | grep -q R_68K; then \
		echo "bootblock.o has relocations (not position independent)"; \
		$(READELF) -r $<; exit 1; fi
	$(OBJCOPY) -O binary -j .text $< $@
	@test $$(stat -c %s $@) -eq 1024 || { echo "bootblock.bin is not 1024 bytes"; exit 1; }

# pvtest: conformance test guest for the virtual CPU, embedded in the image
PVTEST_BASE	:= 0x60000
$(B)/pvtest.elf: guest/pvtest/pvtest.S
	@mkdir -p $(B)/guest
	$(CC) -m68030 -c $< -o $(B)/guest/pvtest.o
	$(LD) -Ttext=$(PVTEST_BASE) -nostdlib --no-warn-rwx-segments -o $@ \
		$(B)/guest/pvtest.o

$(B)/pvtest.bin: $(B)/pvtest.elf
	$(OBJCOPY) -O binary $< $@

$(B)/guest/pvtest/blob.o: $(B)/pvtest.bin

$(B)/pvmk.elf: $(PVMK_OBJS) pvmk.ld
	$(LD) $(LDFLAGS) -o $@ $(PVMK_OBJS) $(LIBGCC) -Map $(B)/pvmk.map

$(B)/pvmk.bin: $(B)/pvmk.elf $(B)/host/mkimage
	$(B)/host/mkimage $< $@

$(B)/host/ufstool: host/ufstool.c svc/ufs.c include/pvmk/ufs.h
	@mkdir -p $(@D)
	$(HOSTCC) $(HOSTCFLAGS) -o $@ $<

$(B)/host/%: host/%.c include/pvmk/image.h
	@mkdir -p $(@D)
	$(HOSTCC) $(HOSTCFLAGS) -o $@ $<

$(B)/pvmk.adf: $(B)/bootblock.bin $(B)/pvmk.bin $(B)/host/mkadf conf/pvmk.conf conf/README.disk
	$(B)/host/mkadf -o $@ -b $(B)/bootblock.bin -i $(B)/pvmk.bin -n PVMK $(ADF_FILES)

# Test hard disk for FS-UAE: RDB with a bootable FFS DH0, an FFS WORK and an
# AMIX-type UNIX partition.
$(B)/test-dh0.img: test/startup-sequence $(B)/host/mkadf
	$(B)/host/mkadf -q -o $@ --blocks 8192 -n TestDH0 -f s/startup-sequence=$<

$(B)/test-work.img: test/work-startup-sequence $(B)/host/mkadf
	$(B)/host/mkadf -q -o $@ --blocks 2048 -n TestWork -f s/startup-sequence=$<

$(B)/test.hdf: $(B)/test-dh0.img $(B)/test-work.img $(B)/host/mkhdf
	$(B)/host/mkhdf -o $@ -p DH0,image=$(B)/test-dh0.img,pri=0 \
		-p WORK,image=$(B)/test-work.img,pri=-5 -p UNIX,size=1M,dostype=0x554e4901

hdf: $(B)/test.hdf

verify: $(B)/pvmk.adf
	$(B)/host/mkadf --verify $<

check-insns: $(B)/pvmk.elf $(B)/boot/bootblock.o
	$(PYTHON) -I host/check-insns.py $(OBJDUMP) $^

size-check: $(B)/pvmk.bin $(B)/boot/bootblock.o
	@bb=$$($(READELF) -s $(B)/boot/bootblock.o | awk '$$8=="_bb_code_end"{print strtonum("0x"$$2)}'); \
	img=$$(stat -c %s $(B)/pvmk.bin); \
	printf "boot block code %5d / %5d bytes\n" $$bb $(BUDGET_BOOTBLOCK); \
	printf "pvmk image      %5d / %5d bytes\n" $$img $(BUDGET_IMAGE); \
	test $$bb -le $(BUDGET_BOOTBLOCK) && test $$img -le $(BUDGET_IMAGE) || \
		{ echo "size budget exceeded"; exit 1; }

check: check-insns size-check verify

UAE_CPU		?= 030
UAE_CPUS	?= 030 040 060
UAE_HDF		?= $(B)/test.hdf
# Your AMIX disk, if present: attached read-only on SCSI ID 6 by run/test
AMIX_HDF	?= $(wildcard $(HOME)/Documents/FS-UAE/Hard\ Drives/AMIGA_UNIX.hdf)
AMIX_OPT	= $(if $(AMIX_HDF),--amix-hdf "$(AMIX_HDF)")
run: $(B)/pvmk.adf $(UAE_HDF)
	$(PYTHON) -I host/uae.py run --cpu $(UAE_CPU) --adf $(B)/pvmk.adf \
		$(if $(UAE_HDF),--hdf $(UAE_HDF)) $(AMIX_OPT)

# A writable copy: AMIX checks and writes its file systems when it boots.
$(B)/amix-test.hdf:
	@test -n "$(AMIX_HDF)" || { echo "no AMIX disk (AMIX_HDF=...)"; exit 1; }
	cp "$(AMIX_HDF)" $@

run-amix: $(B)/pvmk.adf $(B)/amix-test.hdf
	$(PYTHON) -I host/uae.py run --cpu $(UAE_CPU) --adf $(B)/pvmk.adf \
		--amix-hdf $(B)/amix-test.hdf --amix-rw

test: $(B)/pvmk.adf $(B)/test.hdf
	$(PYTHON) -I host/uae.py test --cpu $(UAE_CPUS) --adf $(B)/pvmk.adf \
		--hdf $(B)/test.hdf $(AMIX_OPT)
	$(PYTHON) -I host/uae.py test --cpu 030 --adf $(B)/pvmk.adf \
		--hdf $(B)/test.hdf --hdf-ctrl uae

toolchain:
	host/get-toolchain.sh $(TOOLCHAIN_DIR)

clean:
	rm -rf $(B)

-include $(shell find $(B) -name '*.d' 2>/dev/null)
