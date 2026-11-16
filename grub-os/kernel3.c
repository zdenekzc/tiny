#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "ext2.h"
#include "editor.h"

#include "net.h"
#include "ip.h"
#include "tftp.h"
#include "chainload.h"
#include "tcp.h"

/* VGA text mode */

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

#define SC_UP     0x48
#define SC_DOWN   0x50
#define SC_TAB    0x0F
#define SC_ENTER  0x1C
#define SC_F5     0x3F
#define SC_F6     0x40
#define SC_F7     0x41

/* ATA PIO (single sector) */

static void ata_wait(void) {
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
        __asm__ volatile("inw %1,%0":"=a"(w):"Nd"(0x1F0));
        buffer[i*2]   = w & 0xFF;
        buffer[i*2+1] = w >> 8;
    }
}

/* panels */

#define MAX_ENTRIES 64

typedef struct {
    char     name[64];
    uint32_t ino;
    uint8_t  type; /* 1=file,2=dir */
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
        for (int x=1;x<p->w-1;x++)
            vga_putc_at(' ', p->x+x, row, col);
        char line[70];
        char t = (p->entries[i].type==2)?'D':'F';
        snprintf(line, sizeof(line), "%c %s", t, p->entries[i].name);
        vga_puts_at(line, p->x+2, row, col);
    }
}

/* EXT2 partition LBA (adjust to your disk) */
#define EXT2_PART_LBA 2048

/* network config */

static net_config_t g_net_cfg = {
    .mac   = {0x52,0x54,0x00,0x12,0x34,0x56},
    .ip    = 0x0A00020F,  /* 10.0.2.15 */
    .gw_ip = 0x0A000202,  /* 10.0.2.2 */
};

static uint8_t tftp_buf[4*1024*1024];
static uint8_t filebuf[64*1024];

/* optional TCC JIT hook */
int tcc_run_string(const char *code); /* define if you integrate TCC */

void kernel_main(uint32_t magic, uint32_t mb_info) {
    vga_clear();
    vga_puts_at("Two-pane EXT2 + NET/TFTP/HTTP", 0, 0, COLOR(0xF,0x0));

    if (!ext2_mount(EXT2_PART_LBA)) {
        vga_puts_at("EXT2 mount failed", 0, 1, COLOR(0xC,0x0));
        for(;;);
    } else {
        vga_puts_at("EXT2 OK", 0, 1, COLOR(0xA,0x0));
    }

    if (!net_init(&g_net_cfg)) {
        vga_puts_at("NET init failed", 0, 2, COLOR(0xC,0x0));
    } else {
        arp_init(g_net_cfg.ip);
        tcp_init(g_net_cfg.ip);
        vga_puts_at("NET OK (TFTP+HTTP)", 0, 2, COLOR(0xA,0x0));
    }

    Panel left  = { .x=0,  .y=3, .w=40, .h=22, .current_ino=2 };
    Panel right = { .x=40, .y=3, .w=40, .h=22, .current_ino=2 };
    panel_load_dir(&left);
    panel_load_dir(&right);
    Panel *active = &left;

    for (;;) {
        draw_panel(&left,  active==&left);
        draw_panel(&right, active==&right);

        tcp_poll(); /* HTTP server */

        if (!kbd_has_data()) continue;
        uint8_t sc = kbd_read_scancode();

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
        } else if (sc == SC_F5) {
            vga_puts_at("TFTP netboot.elf (chainload)...", 0, VGA_H-1, COLOR(0xF,0x0));
            uint32_t sz;
            if (tftp_get("netboot.elf", tftp_buf, sizeof(tftp_buf), &sz)) {
                vga_puts_at("TFTP OK, chainloading...", 0, VGA_H-1, COLOR(0xA,0x0));
                chainload_multiboot_elf(tftp_buf, sz, magic, (void*)mb_info);
            } else {
                vga_puts_at("TFTP failed", 0, VGA_H-1, COLOR(0xC,0x0));
            }
        } else if (sc == SC_F6) {
            vga_puts_at("TFTP netboot.elf (save+reboot)...", 0, VGA_H-1, COLOR(0xF,0x0));
            uint32_t sz;
            if (tftp_get("netboot.elf", tftp_buf, sizeof(tftp_buf), &sz)) {
                /* TODO: find inode of /boot/netboot.elf and overwrite it */
                vga_puts_at("Saved, rebooting...", 0, VGA_H-1, COLOR(0xA,0x0));
                outb(0x64, 0xFE);
            } else {
                vga_puts_at("TFTP failed", 0, VGA_H-1, COLOR(0xC,0x0));
            }
        } else if (sc == SC_F7) {
            /* TCC JIT on selected file (if integrated) */
            PanelEntry *e = &active->entries[active->cursor];
            if (e->type == 1) {
                uint32_t size;
                if (ext2_read_file(e->ino, filebuf, sizeof(filebuf)-1, &size)) {
                    filebuf[size] = 0;
                    int r = tcc_run_string((char*)filebuf);
                    char msg[64];
                    snprintf(msg, sizeof(msg), "TCC: program returned %d", r);
                    vga_puts_at(msg, 0, VGA_H-1, COLOR(0xA,0x0));
                }
            }
        }
    }
}
