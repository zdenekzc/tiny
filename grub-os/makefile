CC = gcc
CXX = g++
LD = ld

# All build artifacts (objects, kernel.elf, ISO tree, ISO) go to _build/grub-os
BUILD_DIR = ../_build/grub-os

CFLAGS = -m32 -ffreestanding -O2 -Wall -Wextra -nostdlib -fno-stack-protector -fno-pic -fno-builtin
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
LDFLAGS = -m elf_i386 -T linker.ld -nostdlib

HDRS = $(wildcard *.h)

SRCS_C   = kernel.c minicmd.c ext2.c editor.c e1000.c rtl8139.c net.c ip.c tftp.c chainload.c libc.c
SRCS_CC  = tcp.cc
SRCS_S   = boot.S
OBJS = $(addprefix $(BUILD_DIR)/,$(SRCS_C:.c=.o) $(SRCS_CC:.cc=.o) $(SRCS_S:.S=.o))

all: $(BUILD_DIR)/kernel.elf

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: %.c $(HDRS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.cc $(HDRS) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.S | $(BUILD_DIR)
	$(CC) -m32 -c $< -o $@

$(BUILD_DIR)/kernel.elf: $(OBJS) linker.ld | $(BUILD_DIR)
	$(LD) $(LDFLAGS) $(OBJS) -o $@

GRUB_MKRESCUE = $(shell command -v grub-mkrescue || command -v grub2-mkrescue)

iso: $(BUILD_DIR)/kernel.elf grub.cfg
	mkdir -p $(BUILD_DIR)/iso/boot/grub
	cp $(BUILD_DIR)/kernel.elf $(BUILD_DIR)/iso/boot/kernel.elf
	cp grub.cfg $(BUILD_DIR)/iso/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o $(BUILD_DIR)/twopane_ext2.iso $(BUILD_DIR)/iso

run: iso
	qemu-system-i386 -cdrom $(BUILD_DIR)/twopane_ext2.iso

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all iso run clean

# Debian Trixie : apt install gcc-multilib g++-multilib qemu-system-i386
