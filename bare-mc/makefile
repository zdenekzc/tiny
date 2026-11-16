# Makefile for the minimal bare‑metal kernel that runs mini‑commander via GRUB
#
# The kernel is built as a 64‑bit ELF binary with a multiboot2 header.
# It links with a small C runtime (crt0) that provides the entry point.
# The build uses the host gcc (no cross‑compiler needed because we target x86_64
# and run under QEMU).

# Tools
CC ?= gcc
LD ?= ld

# Repository root (parent directory of bare-mc)
REPO_ROOT := $(abspath ..)
OBJCOPY ?= objcopy
GRUB_MKRESCUE ?= grub2-mkrescue

# Flags
CFLAGS := -ffreestanding -fno-pie -m64 -O2 -Wall -Wextra -I. -I./include
LDFLAGS := -nostdlib -static -melf_x86_64 -T linker.ld

# Sources
SRCS := kernel.c vga.c syscall.c posix.c
OBJS := $(SRCS:.c=.o)

# Output files
KERNEL_BIN := kernel.bin
ISO := bare-mc.iso
GRUB_DIR := grub_dir

all: $(ISO)

$(KERNEL_BIN): $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

# Simple linker script that places the multiboot header at the start
linker.ld:
	@printf "ENTRY(_start)\nSECTIONS\n{\n    . = 0x100000;\n    .multiboot ALIGN(4K) : { *(.multiboot) }\n    .text ALIGN(4K) : { *(.text*) }\n    .rodata ALIGN(4K) : { *(.rodata*) }\n    .data ALIGN(4K) : { *(.data*) }\n    .bss ALIGN(4K) : { *(.bss*) *(COMMON) }\n    . = ALIGN(4K);\n    . += 0x4000;\n    stack_top = .;\n}\n" > $@
	# @printf "SECTIONS\n{\n  . = 0x100000; /* load address */\n  .multiboot : { *(.multiboot) }\n  .text : { *(.text*) }\n  .rodata : { *(.rodata*) }\n  .data : { *(.data*) }\n  .bss : { *(.bss*) }\n}\n" > $@

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Build the initramfs containing mini‑commander and minimal /etc files
initramfs.tar: $(KERNEL_BIN)
	mkdir -p initramfs_root/bin
	cp -a $(REPO_ROOT)/_output/mc initramfs_root/bin/mini-commander
	# Minimal required files for compatibility (empty placeholders)
	mkdir -p initramfs_root/etc
	touch initramfs_root/etc/passwd
	touch initramfs_root/etc/group
	cd initramfs_root && tar -cf ../initramfs.tar .

# GRUB configuration and ISO creation
$(ISO): $(KERNEL_BIN) initramfs.tar
	mkdir -p $(GRUB_DIR)/boot/grub
	cp $(KERNEL_BIN) $(GRUB_DIR)/boot/kernel.bin
	cp initramfs.tar $(GRUB_DIR)/boot/initramfs.tar
	@printf "# Ensure GRUB menu is displayed\ntimeout_style=menu\nset timeout=3\nmenuentry \"Bare MC\" {\n    multiboot2 /boot/kernel.bin\n    module2 /boot/initramfs.tar type=tar\n}\n" > $(GRUB_DIR)/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o $@ $(GRUB_DIR) > /dev/null

clean:
	rm -f $(OBJS) $(KERNEL_BIN) linker.ld initramfs.tar $(ISO)
	rm -rf $(GRUB_DIR) initramfs_root
