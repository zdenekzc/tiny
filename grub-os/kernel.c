#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "ext2.h"
#include "e1000.h"
#include "net.h"
#include "ip.h"
#include "tcp.h"
#include "tftp.h"
#include "chainload.h"
#include "editor.h"

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

static void serial_putc(char c) {
    while (!(inb(COM1+5) & 0x20));
    outb(COM1, (uint8_t)c);
}

static void serial_puts(const char *s) {
    while (*s) serial_putc(*s++);
}

static void serial_put_dec(uint32_t v) {
    char buf[11];
    int i = 0;
    if (!v) { serial_putc('0'); return; }
    while (v) { buf[i++] = '0' + (v % 10); v /= 10; }
    while (i) serial_putc(buf[--i]);
}

/* panels */

#define MAX_ENTRIES 64

typedef struct {
    char     name[64];
    uint32_t ino;
    uint8_t  type;
} PanelEntry;

typedef struct {
    int x,y,w,h;
    PanelEntry entries[MAX_ENTRIES];
    int count;
    int cursor;
    uint32_t current_ino;
} Panel;

static void panel_add_entry_cb(const char *name, uint8_t type, uint32_t ino, void *user) {
    Panel *p = (Panel*)user;
    if (p->count >= MAX_ENTRIES) return;
    PanelEntry *e = &p->entries[p->count++];
    strncpy(e->name, name, sizeof(e->name)-1);
    e->name[sizeof(e->name)-1] = 0;
    e->ino = ino;
    e->type = type;
}

static void panel_load_dir(Panel *p) {
    p->count = 0;
    p->cursor = 0;
    ext2_list_dir(p->current_ino, panel_add_entry_cb, p);
}

static void draw_panel(Panel *p, int active) {
    uint8_t border = COLOR(0xA,0x1);
    uint8_t normal = COLOR(0xF,0x1);
    uint8_t hilite = active ? COLOR(0x0,0xE) : COLOR(0x0,0x7);

    for (int i=0;i<p->w;i++) {
        vga_putc_at('-', p->x+i, p->y, border);
        vga_putc_at('-', p->x+i, p->y+p->h-1, border);
    }
    for (int j=0;j<p->h;j++) {
        vga_putc_at('|', p->x, p->y+j, border);
        vga_putc_at('|', p->x+p->w-1, p->y+j, border);
    }
    vga_putc_at('+', p->x, p->y, border);
    vga_putc_at('+', p->x+p->w-1, p->y, border);
    vga_putc_at('+', p->x, p->y+p->h-1, border);
    vga_putc_at('+', p->x+p->w-1, p->y+p->h-1, border);

    int list_y = p->y+1;
    for (int i=0;i<p->count && i<p->h-2;i++) {
        uint8_t col = (i==p->cursor)?hilite:normal;
        int row = list_y+i;
        /* Build the whole row once (pad with spaces) and write it in a
         * single pass — avoids the blank-flash that causes flicker. */
        char line[130];
        int inner = p->w-2;
        if (inner > 128) inner = 128;
        int li = 0;
        line[li++] = ' ';
        line[li++] = (p->entries[i].type==2)?'D':'F';
        line[li++] = ' ';
        const char *n = p->entries[i].name;
        while (*n && li < inner) line[li++] = *n++;
        while (li < inner) line[li++] = ' ';
        line[li] = 0;
        vga_puts_at(line, p->x+1, row, col);
    }
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

    /*
    if (!e1000_init()) {
        vga_puts_at("E1000 init failed", 0, 1, COLOR(0xC,0x0));
    } else {
        vga_puts_at("E1000 OK", 0, 1, COLOR(0xA,0x0));
    }
    */

    /*
    net_config_t cfg = {
        .mac = {0x52,0x54,0x00,0x12,0x34,0x56}, // or 0, NIC will fill if you implement that
        .ip  = 0x0A00020F,   // 10.0.2.15 for example
        .gw_ip = 0x0A000202, // 10.0.2.2
    };
    
    if (!net_init(&cfg)) {
        vga_puts_at("NET init failed", 0, 2, COLOR(0xC,0x0));
    } else {
        arp_init(cfg.ip);
        tcp_init(cfg.ip);
        vga_puts_at("NET OK", 0, 2, COLOR(0xA,0x0));
    }
    */

    Panel left  = { .x=0,  .y=2, .w=40, .h=23, .current_ino=2 };
    Panel right = { .x=40, .y=2, .w=40, .h=23, .current_ino=2 };
    panel_load_dir(&left);
    panel_load_dir(&right);

    serial_puts("[mc] left entries: ");
    serial_put_dec((uint32_t)left.count);
    serial_puts(", right entries: ");
    serial_put_dec((uint32_t)right.count);
    serial_puts("\r\n");
    for (int i = 0; i < left.count; i++) {
        serial_puts("[mc]   ");
        serial_puts(left.entries[i].type == 2 ? "D " : "F ");
        serial_puts(left.entries[i].name);
        serial_puts("\r\n");
    }

    Panel *active = &left;
    static uint8_t filebuf[64*1024];

    draw_panel(&left,  active==&left);
    draw_panel(&right, active==&right);

    for (;;) {
        if (!kbd_has_data()) continue;
        uint8_t sc = kbd_read_scancode();

        tcp_poll();

        if (sc == SC_TAB) {
            active = (active==&left)?&right:&left;
        } else if (sc == SC_UP) {
            if (active->cursor>0) active->cursor--;
        } else if (sc == SC_DOWN) {
            if (active->cursor<active->count-1) active->cursor++;
        } else if (sc == SC_ENTER) {
            if (active->count==0) continue;
            PanelEntry *e = &active->entries[active->cursor];
            if (e->type == 2) {
                active->current_ino = e->ino;
                panel_load_dir(active);
            } else {
                uint32_t size;
                if (ext2_read_file(e->ino, filebuf, sizeof(filebuf)-1, &size)) {
                    if (size >= sizeof(filebuf)) size = sizeof(filebuf)-1;
                    filebuf[size] = 0;
                    editor_run((char*)filebuf, size);
                    ext2_write_file_overwrite(e->ino, filebuf, size);
                }
            }
        }

        /* Redraw only after an input event — continuous repainting in the
         * idle loop was causing heavy screen flicker. */
        draw_panel(&left,  active==&left);
        draw_panel(&right, active==&right);
    }
}
