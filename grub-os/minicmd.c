/* Mini Commander — bare-metal two-panel file manager.
 * Logic adapted from _import/mini-commander (panel/filelist/sort/colors)
 * onto the freestanding VGA text screen and the ext2 driver. */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "ext2.h"
#include "editor.h"
#include "minicmd.h"

#define VGA_W 80
#define VGA_H 25
#define COLOR(fg,bg) ((uint8_t)((fg) | ((bg) << 4)))

/* keyboard scancodes */
#define SC_UP    0x48
#define SC_DOWN  0x50
#define SC_TAB   0x0F
#define SC_ENTER 0x1C
#define SC_F3    0x3D
#define SC_F4    0x3E

/* mini-commander palette on blue (mc colors, VGA fg|bg) */
#define MC_BORDER   COLOR(0xF,0x1)   /* white on blue */
#define MC_BASE     COLOR(0xF,0x1)   /* regular file */
#define MC_HEADER   COLOR(0xE,0x1)   /* yellow on blue: headers/info */
#define MC_DIR      COLOR(0xD,0x1)   /* magenta on blue: directories */
#define MC_EXEC     COLOR(0xA,0x1)   /* green on blue: executables */
#define MC_CURSOR   COLOR(0x0,0x3)   /* black on cyan: cursor row */
#define MC_INACTIVE COLOR(0x0,0x7)   /* inactive panel cursor */
#define MC_PATH     COLOR(0x0,0x7)   /* path chip */

extern void vga_putc_at(char c, int x, int y, uint8_t color);
extern void vga_puts_at(const char *s, int x, int y, uint8_t color);
extern int  kbd_has_data(void);
extern uint8_t kbd_read_scancode(void);
extern void serial_puts(const char *s);
extern void serial_put_dec(uint32_t v);

#define MC_MAX_ENTRIES 128
#define MC_NAME_MAX 64

typedef struct {
    char     name[MC_NAME_MAX];
    uint32_t ino;
    uint32_t size;
    uint32_t mtime;
    uint16_t mode;
    uint8_t  is_dir;
    uint8_t  is_exec;
} McEntry;

typedef struct {
    int x, y, w, h;
    McEntry entries[MC_MAX_ENTRIES];
    int count;
    int cursor;
    uint32_t cur_ino;
    char path[96];
    uint32_t ino_stack[16];  /* inode stack for ".." navigation */
    int sp;
} McPanel;

static McPanel left_panel, right_panel;
static McPanel *active_panel = &left_panel;

static uint8_t filebuf[64 * 1024];

/* ---------- small helpers ---------- */

static char mc_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static int mc_namecmp(const char *a, const char *b) {
    while (*a && mc_lower(*a) == mc_lower(*b)) { a++; b++; }
    return (int)(uint8_t)mc_lower(*a) - (int)(uint8_t)mc_lower(*b);
}

/* decimal, right-aligned into a fixed field */
static void mc_fmt_num(char *field, int width, uint32_t v) {
    char tmp[12];
    int i = 0, pos = width;
    if (!v) tmp[i++] = '0';
    while (v) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
    while (pos > 0) {
        pos--;
        field[pos] = (i > 0) ? tmp[--i] : ' ';
    }
}

/* unix time -> "dd-mm-yy" (9 chars incl NUL) */
static void mc_fmt_date(char *out, uint32_t t) {
    uint32_t days = t / 86400;
    uint32_t secs = t % 86400;
    uint32_t hh = secs / 3600, mm = (secs % 3600) / 60;

    days += 719468;
    uint32_t era = days / 146097;
    uint32_t doe = days - era * 146097;
    uint32_t yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
    int64_t  y   = (int64_t)yoe + (int64_t)era * 400;
    uint32_t doy = doe - (365*yoe + yoe/4 - yoe/100);
    uint32_t mp  = (5*doy + 2) / 153;
    uint32_t d   = doy - (153*mp + 2)/5 + 1;
    uint32_t m   = mp + (mp < 10 ? 3 : -9);
    y += (m <= 2);
    if (y < 0) y = 0;
    uint32_t yy = (uint32_t)y % 100;

    out[0] = (char)('0' + (d / 10) % 10);
    out[1] = (char)('0' + d % 10);
    out[2] = '-';
    out[3] = (char)('0' + (m / 10) % 10);
    out[4] = (char)('0' + m % 10);
    out[5] = '-';
    out[6] = (char)('0' + (yy / 10) % 10);
    out[7] = (char)('0' + yy % 10);
    (void)hh; (void)mm;
    out[8] = 0;
}

/* ---------- directory loading (filelist.c logic) ---------- */

typedef struct {
    McPanel *panel;
    int overflow;
} LoadCtx;

static void load_cb(const char *name, uint8_t type, uint32_t ino, void *user) {
    LoadCtx *ctx = (LoadCtx*)user;
    McPanel *p = ctx->panel;
    if (!strcmp(name, ".") || !strcmp(name, "..")) return;  /* ".." is synthesized */
    if (p->count >= MC_MAX_ENTRIES) { ctx->overflow = 1; return; }

    McEntry *e = &p->entries[p->count++];
    strncpy(e->name, name, MC_NAME_MAX - 1);
    e->name[MC_NAME_MAX - 1] = 0;
    e->ino = ino;
    e->is_dir = (type == 2);
    e->size = 0;
    e->mtime = 0;
    e->mode = 0;
    e->is_exec = 0;

    Ext2Inode ino_st;
    if (ext2_read_inode(ino, &ino_st)) {
        e->size = ino_st.size;
        e->mtime = ino_st.mtime;
        e->mode = ino_st.mode;
        e->is_exec = !e->is_dir && (ino_st.mode & 0x49);
    }
}

/* ".." first, directories first, then case-insensitive by name */
static int entry_before(const McEntry *a, const McEntry *b) {
    int a_up = !strcmp(a->name, "..");
    int b_up = !strcmp(b->name, "..");
    if (a_up != b_up) return a_up;
    if (a->is_dir != b->is_dir) return a->is_dir;
    return mc_namecmp(a->name, b->name) < 0;
}

static void panel_sort(McPanel *p) {
    for (int i = 1; i < p->count; i++) {
        McEntry key = p->entries[i];
        int j = i - 1;
        while (j >= 0 && entry_before(&key, &p->entries[j])) {
            p->entries[j + 1] = p->entries[j];
            j--;
        }
        p->entries[j + 1] = key;
    }
}

static void panel_load(McPanel *p) {
    LoadCtx ctx = { p, 0 };
    p->count = 0;
    ext2_list_dir(p->cur_ino, load_cb, &ctx);
    if (p->sp > 0) {
        /* synthesize the ".." entry pointing at the parent inode */
        McEntry *e = &p->entries[p->count];
        if (p->count < MC_MAX_ENTRIES) {
            p->count++;
            strncpy(e->name, "..", MC_NAME_MAX - 1);
            e->name[MC_NAME_MAX - 1] = 0;
            e->ino = p->ino_stack[p->sp - 1];
            e->size = 0; e->mtime = 0; e->mode = 0;
            e->is_dir = 1; e->is_exec = 0;
        }
    }
    panel_sort(p);
    p->cursor = 0;
}

/* ---------- navigation (dive_into_directory logic) ---------- */

static void panel_enter_dir(McPanel *p, const char *name, uint32_t ino) {
    if (p->sp < 16) p->ino_stack[p->sp++] = p->cur_ino;
    p->cur_ino = ino;
    size_t l = strlen(p->path);
    if (l + 1 < sizeof(p->path) && p->path[l - 1] != '/') p->path[l++] = '/';
    if (l < sizeof(p->path)) strncpy(p->path + l, name, sizeof(p->path) - l - 1);
    p->path[sizeof(p->path) - 1] = 0;
    panel_load(p);
    serial_puts("[mc] chdir ");
    serial_puts(p->path);
    serial_puts(" (");
    serial_put_dec((uint32_t)p->count);
    serial_puts(" entries)\r\n");
}

static void panel_go_up(McPanel *p) {
    if (p->sp == 0) return;
    p->cur_ino = p->ino_stack[--p->sp];
    int l = (int)strlen(p->path);
    while (l > 1 && p->path[l - 1] != '/') l--;
    if (l > 1) p->path[l - 1] = 0;   /* cut "/name" */
    else p->path[1] = 0;             /* back to root "/" */
    panel_load(p);
    serial_puts("[mc] chdir ");
    serial_puts(p->path);
    serial_puts(" (up)\r\n");
}

/* ---------- drawing (panel.c logic) ---------- */

#define NAME_W_INNER(p) ((p)->w - 2 - 19)

static void draw_panel(McPanel *p, int active) {
    uint8_t border = MC_BORDER;
    int inner = p->w - 2;
    int name_w = NAME_W_INNER(p);

    /* frame */
    for (int i = 0; i < p->w; i++) {
        vga_putc_at('-', p->x + i, p->y, border);
        vga_putc_at('-', p->x + i, p->y + p->h - 1, border);
    }
    for (int j = 0; j < p->h; j++) {
        vga_putc_at('|', p->x, p->y + j, border);
        vga_putc_at('|', p->x + p->w - 1, p->y + j, border);
    }
    vga_putc_at('+', p->x, p->y, border);
    vga_putc_at('+', p->x + p->w - 1, p->y, border);
    vga_putc_at('+', p->x, p->y + p->h - 1, border);
    vga_putc_at('+', p->x + p->w - 1, p->y + p->h - 1, border);

    /* path chip */
    int py = p->y + 1;
    for (int i = 0; i < inner; i++) vga_putc_at(' ', p->x + 1 + i, py, MC_PATH);
    vga_puts_at(p->path, p->x + 2, py, MC_PATH);

    /* column header: Name | Size | Date */
    int hy = p->y + 2;
    for (int i = 0; i < inner; i++) vga_putc_at(' ', p->x + 1 + i, hy, MC_HEADER);
    vga_puts_at("Name", p->x + 2, hy, MC_HEADER);
    vga_puts_at("Size", p->x + 1 + 24, hy, MC_HEADER);
    vga_puts_at("Date", p->x + 1 + 34, hy, MC_HEADER);

    /* rows */
    int max_rows = p->h - 5;
    int list_y = p->y + 3;
    for (int i = 0; i < max_rows; i++) {
        McEntry *e = (i < p->count) ? &p->entries[i] : 0;
        uint8_t col;
        if (e && i == p->cursor)
            col = active ? MC_CURSOR : MC_INACTIVE;
        else if (e && e->is_dir)
            col = MC_DIR;
        else if (e && e->is_exec)
            col = MC_EXEC;
        else
            col = MC_BASE;

        char line[128];
        int li = 0;
        int lim = inner;
        line[li++] = ' ';
        if (e) {
            const char *n = e->name;
            while (*n && li < 1 + name_w) line[li++] = *n++;
        }
        while (li < 1 + name_w) line[li++] = ' ';
        line[li++] = ' ';
        if (!e) {
            while (li < lim) line[li++] = ' ';
        } else if (e->is_dir) {
            char f[8];
            for (int k = 0; k < 7; k++) f[k] = ' ';
            for (int k = 0; k < 5; k++) f[2 + k] = "<DIR>"[k];
            for (int k = 0; k < 7; k++) line[li++] = f[k];
        } else {
            char f[8];
            mc_fmt_num(f, 7, e->size);
            for (int k = 0; k < 7; k++) line[li++] = f[k];
        }
        if (e) {
            line[li++] = ' ';
            char dt[9];
            mc_fmt_date(dt, e->mtime);
            for (int k = 0; k < 8; k++) line[li++] = dt[k];
        }
        while (li < lim) line[li++] = ' ';
        line[li] = 0;
        vga_puts_at(line, p->x + 1, list_y + i, col);
    }

    /* info row */
    int iy = p->y + p->h - 2;
    for (int i = 0; i < inner; i++) vga_putc_at(' ', p->x + 1 + i, iy, MC_HEADER);
    vga_puts_at("items: ", p->x + 2, iy, MC_HEADER);
    {
        char f[8];
        mc_fmt_num(f, 7, (uint32_t)p->count);
        /* strip leading spaces for a simple left print */
        int k = 0;
        while (k < 7 && f[k] == ' ') k++;
        vga_puts_at(f + k, p->x + 9, iy, MC_HEADER);
    }
}

static void draw_fkeybar(void);

static void draw_all(void) {
    draw_panel(&left_panel, active_panel == &left_panel);
    draw_panel(&right_panel, active_panel == &right_panel);
    draw_fkeybar();
}

/* bottom F1..F10 key bar (Norton Commander style), VGA row VGA_H-1 */
static void draw_fkeybar(void) {
    static const char *labels[10] = {
        "Help", "Menu", "View", "Edit", "Copy",
        "Move", "MkDir", "Del", "Menu", "Quit"
    };
    int y = VGA_H - 1;
    int x = 0;
    for (int i = 0; i < 10; i++) {
        /* key number, bright */
        if (i < 9) {
            vga_putc_at((char)('1' + i), x++, y, COLOR(0xF,0x0));
        } else {
            vga_putc_at('1', x++, y, COLOR(0xF,0x0));
            vga_putc_at('0', x++, y, COLOR(0xF,0x0));
        }
        vga_putc_at(' ', x++, y, COLOR(0x7,0x0));
        const char *l = labels[i];
        while (*l) vga_putc_at(*l++, x++, y, COLOR(0x7,0x0));
        while ((i < 9 && (x % 8) != 0) || (i == 9 && x < VGA_W))
            vga_putc_at(' ', x++, y, COLOR(0x7,0x0));
    }
}

/* ---------- main loop ---------- */

void minicmd_run(void) {
    memset(&left_panel, 0, sizeof(left_panel));
    memset(&right_panel, 0, sizeof(right_panel));

    left_panel.x = 0;  left_panel.y = 2; left_panel.w = 40; left_panel.h = 22;
    right_panel.x = 40; right_panel.y = 2; right_panel.w = 40; right_panel.h = 22;
    left_panel.cur_ino = 2; right_panel.cur_ino = 2;
    strncpy(left_panel.path, "/", sizeof(left_panel.path) - 1);
    strncpy(right_panel.path, "/", sizeof(right_panel.path) - 1);
    panel_load(&left_panel);
    panel_load(&right_panel);

    serial_puts("[mc] left entries: ");
    serial_put_dec((uint32_t)left_panel.count);
    serial_puts(", right entries: ");
    serial_put_dec((uint32_t)right_panel.count);
    serial_puts("\r\n");
    for (int i = 0; i < left_panel.count; i++) {
        serial_puts("[mc]   ");
        serial_puts(left_panel.entries[i].is_dir ? "D " : "F ");
        serial_puts(left_panel.entries[i].name);
        serial_puts("\r\n");
    }

    draw_all();
    int need_clear = 0;

    for (;;) {
        if (!kbd_has_data()) continue;
        uint8_t sc = kbd_read_scancode();

        if (sc == SC_TAB) {
            active_panel = (active_panel == &left_panel) ? &right_panel : &left_panel;
        } else if (sc == SC_UP) {
            if (active_panel->cursor > 0) active_panel->cursor--;
        } else if (sc == SC_DOWN) {
            if (active_panel->cursor < active_panel->count - 1) active_panel->cursor++;
        } else if (sc == SC_ENTER || sc == SC_F3 || sc == SC_F4) {
            if (active_panel->count == 0) continue;
            McEntry *e = &active_panel->entries[active_panel->cursor];
            if (!strcmp(e->name, "..")) {
                panel_go_up(active_panel);
            } else if (e->is_dir) {
                panel_enter_dir(active_panel, e->name, e->ino);
            } else {
                uint32_t size;
                if (ext2_read_file(e->ino, filebuf, sizeof(filebuf) - 1, &size)) {
                    if (size >= sizeof(filebuf)) size = sizeof(filebuf) - 1;
                    filebuf[size] = 0;
                    editor_run((char*)filebuf, size);
                    ext2_write_file_overwrite(e->ino, filebuf, size);
                }
                need_clear = 1;  /* editor trashed the screen */
            }
        }

        if (need_clear) {
            need_clear = 0;
            extern void vga_clear(void);
            vga_clear();
            vga_puts_at("Mini Commander (TAB, arrows, ENTER)", 0, 0, COLOR(0xF,0x0));
        }
        draw_all();
    }
}
