CC    ?= gcc
NASM  ?= nasm
LD    ?= ld
BUILD := build

CFLAGS := -std=gnu11 -O2 -g -Wall -Wextra \
          -ffreestanding -fno-pic -fno-pie -fno-stack-protector \
          -fno-tree-loop-distribute-patterns \
          -fno-asynchronous-unwind-tables -fno-unwind-tables \
          -mno-red-zone -mgeneral-regs-only -mcmodel=small \
          -Ikernel/include -MMD -MP

ifdef TEST_FAULT
CFLAGS += -DTEST_FAULT
endif

ifdef ONLY_W32
CFLAGS += -DONLY_W32
endif

ifdef TEST_STACKOVERFLOW
CFLAGS += -DTEST_STACKOVERFLOW
endif

LDFLAGS := -nostdlib -static -z max-page-size=0x1000 -z noexecstack \
           --no-warn-rwx-segments -T linker.ld

C_SRCS   := $(wildcard kernel/*.c kernel/mm/*.c kernel/lib/*.c)
ASM_SRCS := boot/boot.asm $(wildcard kernel/*.asm)
GAS_SRCS := $(wildcard kernel/*.S)
OBJS     := $(patsubst %.c,$(BUILD)/%.o,$(C_SRCS)) \
            $(patsubst %.asm,$(BUILD)/%.o,$(ASM_SRCS)) \
            $(patsubst %.S,$(BUILD)/%.o,$(GAS_SRCS))

USER_BIN := $(BUILD)/user/tests.bin

LX_DIR  := $(BUILD)/user/linux
LX_ELFS := $(LX_DIR)/lxmin.elf $(LX_DIR)/lxexec.elf $(LX_DIR)/lxglibc.elf
LX_FREE := -O2 -ffreestanding -fPIE -fno-stack-protector -fno-builtin -nostdlib -Wall -Wextra

$(LX_DIR)/lxmin.o: user/linux/lxmin.c user/linux/lx.h
	@mkdir -p $(LX_DIR)
	$(CC) $(LX_FREE) -c $< -o $@
$(LX_DIR)/lxmin.elf: $(LX_DIR)/lxmin.o
	$(LD) -pie --no-dynamic-linker -z norelro -z noexecstack -e _start -o $@ $<
$(LX_DIR)/lxexec.elf: $(LX_DIR)/lxmin.o
	$(LD) -no-pie -z norelro -z noexecstack -e _start -Ttext-segment=0x8000400000 -o $@ $<
$(LX_DIR)/lxglibc.elf: user/linux/lxglibc.c
	@mkdir -p $(LX_DIR)
	$(CC) -O2 -static-pie $< -o $@ -lm
$(BUILD)/kernel/linuximg.o: $(LX_ELFS)

W32_DIR  := $(BUILD)/user/w32
MINGW    ?= x86_64-w64-mingw32
W32_RTFLAGS := -std=gnu11 -O2 -g -Wall -Wextra -ffreestanding -fPIE -fno-stack-protector \
               -fno-tree-loop-distribute-patterns -fno-asynchronous-unwind-tables -fno-unwind-tables
W32_RTOBJS := $(W32_DIR)/rt_core.o $(W32_DIR)/rt_k32.o $(W32_DIR)/rt_crt.o
W32_PEFREE := -O2 -ffreestanding -fno-tree-loop-distribute-patterns -fno-stack-protector -fno-asynchronous-unwind-tables -nostdlib -Wall -Wextra -Wno-array-bounds
W32_PELD   := -nostdlib -nostartfiles -Wl,-e,entry -Wl,--subsystem,console -lkernel32 -lmsvcrt -lgcc
W32_EXES := $(W32_DIR)/w32min.exe $(W32_DIR)/w32fix.exe $(W32_DIR)/w32norel.exe $(W32_DIR)/w32crt.exe
define NEED_MINGW
	@command -v $(MINGW)-gcc >/dev/null || { echo "error: $(MINGW)-gcc not found (apt install gcc-mingw-w64-x86-64) - needed for the PE test programs"; exit 1; }
endef

$(W32_DIR)/%.o: user/w32/%.c user/w32/rt.h kernel/include/w32abi.h kernel/include/abi.h
	@mkdir -p $(W32_DIR)
	$(CC) $(W32_RTFLAGS) -c $< -o $@
$(W32_DIR)/rt.elf: $(W32_RTOBJS)
	$(LD) -no-pie -z norelro -z noexecstack -z max-page-size=0x1000 --no-warn-rwx-segments -e w32rt_start \
	      -Ttext-segment=0x8000010000 -o $@ $^

$(W32_DIR)/w32min.o: user/w32/w32min.c
	$(NEED_MINGW)
	@mkdir -p $(W32_DIR)
	$(MINGW)-gcc $(W32_PEFREE) -c $< -o $@
$(W32_DIR)/w32min.exe: $(W32_DIR)/w32min.o
	$(MINGW)-gcc $< -o $@ $(W32_PELD) -Wl,--dynamicbase
$(W32_DIR)/w32fix.exe: $(W32_DIR)/w32min.o
	$(MINGW)-gcc $< -o $@ $(W32_PELD) -Wl,--image-base=0x8000800000 -Wl,--disable-dynamicbase -Wl,--disable-reloc-section
$(W32_DIR)/w32norel.exe: $(W32_DIR)/w32min.o
	$(MINGW)-gcc $< -o $@ $(W32_PELD) -Wl,--disable-dynamicbase -Wl,--disable-reloc-section
$(W32_DIR)/w32crt.exe: user/w32/w32crt.c
	$(NEED_MINGW)
	@mkdir -p $(W32_DIR)
	$(MINGW)-gcc -O2 -s $< -o $@
$(BUILD)/kernel/win32img.o: $(W32_DIR)/rt.elf $(W32_EXES)

KERNEL := $(BUILD)/kernel.elf
ISO    := $(BUILD)/infinityos.iso

comma := ,
DISKARGS := $(if $(wildcard $(BUILD)/disk.img),-drive file=$(BUILD)/disk.img$(comma)format=raw$(comma)if=ide$(comma)index=0) \
            $(if $(wildcard $(BUILD)/scratch.img),-drive file=$(BUILD)/scratch.img$(comma)format=raw$(comma)if=ide$(comma)index=1) \
            $(if $(wildcard $(BUILD)/disk4k.img),-drive file=$(BUILD)/disk4k.img$(comma)format=raw$(comma)if=ide$(comma)index=2) \
            $(if $(wildcard $(BUILD)/scratch2k.img),-drive file=$(BUILD)/scratch2k.img$(comma)format=raw$(comma)if=ide$(comma)index=3)

.DEFAULT_GOAL := all
.PHONY: all iso run test clean check disks
all: iso

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) -Ikernel/include -MMD -MP -c $< -o $@

$(USER_BIN): $(BUILD)/user/tests.o user/user.ld
	$(LD) -nostdlib -T user/user.ld -o $@ $<

$(BUILD)/user/tests.o: kernel/include/abi.h
$(BUILD)/kernel/userimg.o: $(USER_BIN)

$(BUILD)/%.o: %.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 -g -F dwarf $< -o $@

$(KERNEL): $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

check: $(KERNEL)
	grub-file --is-x86-multiboot2 $(KERNEL) && echo "multiboot2: OK"

iso: $(ISO)
$(ISO): $(KERNEL) boot/grub.cfg
	@mkdir -p $(BUILD)/iso/boot/grub
	cp $(KERNEL) $(BUILD)/iso/boot/kernel.elf
	cp boot/grub.cfg $(BUILD)/iso/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(BUILD)/iso 2>/dev/null

CDARGS := -drive file=$(ISO)$(comma)if=none$(comma)id=cd$(comma)media=cdrom$(comma)format=raw -device ich9-ahci$(comma)id=ahci \
          -device ide-cd$(comma)drive=cd$(comma)bus=ahci.0$(comma)bootindex=0

run: $(ISO)
	qemu-system-x86_64 $(CDARGS) $(DISKARGS) -m 256M -serial stdio -no-reboot

disks:
	tools/mkdisk.sh

test: $(ISO)
	tools/mkdisk.sh
	tools/test.sh
	tools/fscheck.sh

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d)
