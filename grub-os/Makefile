CC = gcc
CXX = g++
LD = ld

CFLAGS = -m32 -ffreestanding -O2 -Wall -Wextra -nostdlib -fno-stack-protector -fno-pic -fno-builtin
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
LDFLAGS = -m elf_i386 -T linker.ld -nostdlib

OBJS = boot.o kernel.o ext2.o editor.o e1000.o rtl8139.o net.o ip.o tftp.o chainload.o tcp.o libc.o

all: kernel.elf

boot.o: boot.S
	$(CC) -m32 -c boot.S -o boot.o

kernel.o: kernel3.c ext2.h editor.h e1000.h net.h ip.h tcp.h tftp.h chainload.h
	$(CC) $(CFLAGS) -c kernel.c -o kernel.o

ext2.o: ext2.c ext2.h
	$(CC) $(CFLAGS) -c ext2.c -o ext2.o

editor.o: editor.c editor.h
	$(CC) $(CFLAGS) -c editor.c -o editor.o

e1000.o: e1000.c e1000.h
	$(CC) $(CFLAGS) -c e1000.c -o e1000.o

rtl8139.o: rtl8139.c rtl8139.h
	$(CC) $(CFLAGS) -c rtl8139.c -o rtl8139.o

net.o: net.c net.h e1000.h rtl8139.h
	$(CC) $(CFLAGS) -c net.c -o net.o

ip.o: ip.c ip.h net.h
	$(CC) $(CFLAGS) -c ip.c -o ip.o

tftp.o: tftp.c tftp.h
	$(CC) $(CFLAGS) -c tftp.c -o tftp.o

chainload.o: chainload.c chainload.h
	$(CC) $(CFLAGS) -c chainload.c -o chainload.o

tcp.o: tcp.cc tcp.h ip.h net.h
	$(CXX) $(CXXFLAGS) -c tcp.cc -o tcp.o

libc.o: libc.c
	$(CC) $(CFLAGS) -c libc.c -o libc.o

kernel.elf: $(OBJS) linker.ld
	$(LD) $(LDFLAGS) $(OBJS) -o kernel.elf

GRUB_MKRESCUE = $(shell command -v grub-mkrescue || command -v grub2-mkrescue)

iso: kernel.elf grub.cfg
	mkdir -p iso/boot/grub
	cp kernel.elf iso/boot/kernel.elf
	cp grub.cfg iso/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o twopane_ext2.iso iso

run: iso
	qemu-system-i386 -cdrom twopane_ext2.iso

clean:
	rm -f *.o kernel.elf twopane_ext2.iso
	rm -rf iso

.PHONY: all iso run clean

# Debian Trixie : apt install gcc-multilib g++-multilib qemu-system-i386

