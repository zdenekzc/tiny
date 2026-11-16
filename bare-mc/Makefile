# Makefile for the minimal bare‑metal kernel that runs mini‑commander via GRUB
#
# The kernel is built as a 64‑bit ELF binary with a multiboot2 header.
# It links with a small C runtime (crt0) that provides the entry point.
# The build uses the host gcc (no cross‑compiler needed because we target x86_64
# and run under QEMU).

# Tools
CC ?= gcc
LD ?= ld
OBJCOPY ?= objcopy
GRUB_MKRESCUE ?= grub2-mkrescue

# Flags
CFLAGS := -ffreestanding -fno-pie -m64 -O2 -Wall -Wextra -nostdinc -I. -I./include
LDFLAGS := -nostdlib -static -melf_x86_64 -T linker.ld

# Sources
SRCS := kernel.c vga.c syscall.c posix.c
OBJS := $(SRCS:.c=.o)

# Output files
KERNEL_BIN := kernel.bin
ISO := bare-mc.iso
GRUB_DIR := grub_dir

all: $(ISO)

$(KERNEL_BIN): $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $^
	# Strip to raw binary (multiboot2 expects a flat binary)
	$(OBJCOPY) -O binary $@ $@

# Simple linker script that places the multiboot header at the start
linker.ld: ; @echo "Generating minimal linker script" > $@
	@cat <<'EOF' >> $@
SECTIONS
{
  . = 0x100000; /* load address */
  .multiboot : { *(.multiboot) }
  .text : { *(.text*) }
  .rodata : { *(.rodata*) }
  .data : { *(.data*) }
  .bss : { *(.bss*) }
}
EOF

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Build the initramfs containing mini‑commander and minimal /etc files
initramfs.tar: $(KERNEL_BIN)
	mkdir -p bare-mc/initramfs_root/bin
	cp -a _output/mc bare-mc/initramfs_root/bin/mini-commander
	# Minimal required files for compatibility (empty placeholders)
	mkdir -p bare-mc/initramfs_root/etc
	touch bare-mc/initramfs_root/etc/passwd
	touch bare-mc/initramfs_root/etc/group
	cd bare-mc/initramfs_root && tar -cf ../initramfs.tar .

# GRUB configuration and ISO creation
$(ISO): $(KERNEL_BIN) initramfs.tar
	mkdir -p $(GRUB_DIR)/boot/grub
	cp $(KERNEL_BIN) $(GRUB_DIR)/boot/kernel.bin
	cp initramfs.tar $(GRUB_DIR)/boot/initramfs.tar
	cat > $(GRUB_DIR)/boot/grub/grub.cfg <<'EOF'
set timeout=3
menuentry "Bare MC" {
    multiboot2 /boot/kernel.bin
    module2 /boot/initramfs.tar type=tar
}
EOF
	$(GRUB_MKRESCUE) -o $@ $(GRUB_DIR) > /dev/null

clean:
	rm -f $(OBJS) $(KERNEL_BIN) linker.ld initramfs.tar $(ISO)
	rm -rf $(GRUB_DIR) bare-mc/initramfs_root
