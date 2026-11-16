#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "ext2.h"
#include "minicmd.h"

#define VGA_MEM   ((uint16_t*)0xB8000)
#define VGA_W     80
#define VGA_H     25
#define COLOR(fg,bg) ((fg) | ((bg) << 4))

static uint8_t term_color = COLOR(0xF,0x0);

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0,%1"::"a"(val),"Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t r; __asm__ volatile ("inb %1,%0":"=a"(r):"Nd"(port)); return r;
}

void vga_clear(void) {
    for (size_t y=0;y<VGA_H;y++)
        for (size_t x=0;x<VGA_W;x++)
            VGA_MEM[y*VGA_W+x] = ((uint16_t)term_color<<8) | ' ';
}

void vga_putc_at(char c, int x, int y, uint8_t color) {
    if (x<0||x>=VGA_W||y<0||y>=VGA_H) return;
    VGA_MEM[y*VGA_W+x] = ((uint16_t)color<<8) | (uint8_t)c;
}

void vga_puts_at(const char *s, int x, int y, uint8_t color) {
    while (*s && x<VGA_W) vga_putc_at(*s++, x++, y, color);
}

/* keyboard */

int kbd_has_data(void) { return inb(0x64) & 1; }
uint8_t kbd_read_scancode(void) { return inb(0x60); }

char scancode_to_char(uint8_t sc) {
    static const char table[128] = {
        [0x02]='1',[0x03]='2',[0x04]='3',[0x05]='4',[0x06]='5',[0x07]='6',
        [0x08]='7',[0x09]='8',[0x0A]='9',[0x0B]='0',
        [0x10]='q',[0x11]='w',[0x12]='e',[0x13]='r',[0x14]='t',[0x15]='y',
        [0x16]='u',[0x17]='i',[0x18]='o',[0x19]='p',
        [0x1E]='a',[0x1F]='s',[0x20]='d',[0x21]='f',[0x22]='g',[0x23]='h',
        [0x24]='j',[0x25]='k',[0x26]='l',
        [0x2C]='z',[0x2D]='x',[0x2E]='c',[0x2F]='v',[0x30]='b',[0x31]='n',
        [0x32]='m',[0x39]=' '
    };
    if (sc & 0x80) return 0;
    return table[sc];
}

#define SC_UP    0x48
#define SC_DOWN  0x50
#define SC_TAB   0x0F
#define SC_ENTER 0x1C

/* ATA PIO */

void ata_wait(void) {
    while (inb(0x1F7) & 0x80);
    while (!(inb(0x1F7) & 0x08));
}

void ata_read28(uint32_t lba, uint8_t *buffer) {
    outb(0x1F6, 0xE0 | ((lba>>24)&0x0F));
    outb(0x1F2, 1);
    outb(0x1F3, (uint8_t)lba);
    outb(0x1F4, (uint8_t)(lba>>8));
    outb(0x1F5, (uint8_t)(lba>>16));
    outb(0x1F7, 0x20);
    ata_wait();
    for (int i=0;i<256;i++) {
        uint16_t w;
        __asm__ volatile("inw %w1,%0":"=a"(w):"d"(0x1F0));
        buffer[i*2]   = w & 0xFF;
        buffer[i*2+1] = w >> 8;
    }
}

void ata_write28(uint32_t lba, const uint8_t *buffer) {
    outb(0x1F6, 0xE0 | ((lba>>24)&0x0F));
    outb(0x1F2, 1);
    outb(0x1F3, (uint8_t)lba);
    outb(0x1F4, (uint8_t)(lba>>8));
    outb(0x1F5, (uint8_t)(lba>>16));
    outb(0x1F7, 0x30);
    ata_wait();
    for (int i=0;i<256;i++) {
        uint16_t w = buffer[i*2] | (buffer[i*2+1]<<8);
        __asm__ volatile("outw %0,%w1"::"a"(w),"d"(0x1F0));
    }
    /* wait for the drive to finish processing the write; issuing the
     * next command while BSY makes QEMU silently drop it */
    while (inb(0x1F7) & 0x80) ;
    inb(0x1F7);  /* status read acknowledges the interrupt */
}

/* serial (COM1) debug output */
#define COM1 0x3F8

static void serial_init(void) {
    outb(COM1+1, 0x00);
    outb(COM1+3, 0x80);
    outb(COM1+0, 0x01); /* 115200 baud */
    outb(COM1+1, 0x00);
    outb(COM1+3, 0x03); /* 8N1 */
    outb(COM1+2, 0xC7);
    outb(COM1+4, 0x0B);
}

void serial_putc(char c) {
    while (!(inb(COM1+5) & 0x20));
    outb(COM1, (uint8_t)c);
}

void serial_puts(const char *s) {
    while (*s) serial_putc(*s++);
}

void serial_put_dec(uint32_t v) {
    char buf[11];
    int i = 0;
    if (!v) { serial_putc('0'); return; }
    while (v) { buf[i++] = '0' + (v % 10); v /= 10; }
    while (i) serial_putc(buf[--i]);
}

/* EXT2 filesystem start (LBA). 0 = unpartitioned disk image (disk.img);
 * set to 2048 for a partitioned disk like vhd_boot.img. */
#define EXT2_PART_LBA 0

void kernel_main(uint32_t magic, uint32_t mb_info) {
    (void)magic; (void)mb_info;

    vga_clear();
    vga_puts_at("Mini Commander (TAB, arrows, ENTER)", 0, 0, COLOR(0xF,0x0));

    serial_init();
    serial_puts("[mc] boot\r\n");

    if (!ext2_mount(EXT2_PART_LBA)) {
        vga_puts_at("EXT2 mount failed", 0, 1, COLOR(0xC,0x0));
        serial_puts("[mc] EXT2 mount FAILED\r\n");
    } else {
        vga_puts_at("EXT2 mounted", 0, 1, COLOR(0xA,0x0));
        serial_puts("[mc] EXT2 mounted OK\r\n");
    }

    /* two-panel mini commander (port of _import/mini-commander) */
    minicmd_run();
}

