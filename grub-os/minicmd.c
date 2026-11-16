/* Mini Commander — bare-metal two-panel file manager.
 * Logic adapted from _import/mini-commander (panel/filelist/sort/colors)
 * onto the freestanding VGA text screen and the ext2 driver. */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "ext2.h"
#include "ext2w.h"
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
#define SC_ESC   0x01
#define SC_BACK  0x0E
#define SC_F3    0x3D
#define SC_F4    0x3E
#define SC_F5    0x3F
#define SC_F6    0x40
#define SC_F7    0x41
#define SC_F8    0x42
#define SC_F9    0x43
#define SC_F10   0x44

/* mini-commander palette, matching _import/mini-commander panel.c
 * (ncurses 8 colors + A_BOLD -> bright VGA variants) */
#define MC_BORDER   COLOR(0xF,0x1)   /* white on blue */
#define MC_FILE     COLOR(0x7,0x1)   /* regular file: white on blue */
#define MC_HEADER   COLOR(0xE,0x1)   /* yellow on blue: headers/info */
#define MC_DIR      COLOR(0xF,0x1)   /* dirs: white+bold on blue, prefix '/' */
#define MC_EXEC     COLOR(0xA,0x1)   /* green+bold on blue, prefix '*' */
#define MC_SRC      COLOR(0x3,0x1)   /* .c/.h/.sh/.php: cyan on blue */
#define MC_ARCH     COLOR(0xD,0x1)   /* .gz/.tar/.xz: magenta+bold on blue */
#define MC_CURSOR   COLOR(0x0,0x3)   /* black on cyan: cursor row */
#define MC_INACTIVE COLOR(0x0,0x7)   /* inactive panel cursor */
#define MC_PATH     COLOR(0x0,0xF)   /* path chip: black on white (active) */

extern void vga_putc_at(char c, int x, int y, uint8_t color);
extern void vga_puts_at(const char *s, int x, int y, uint8_t color);
extern int  kbd_has_data(void);
extern uint8_t kbd_read_scancode(void);
extern char scancode_to_char(uint8_t sc);
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

/* unix time -> "Mon DD HH:MM" (13 chars incl NUL), like the Linux mc */
static void mc_fmt_date(char *out, uint32_t t) {
    static const char *mo[12] = {
        "Jan","Feb","Mar","Apr","May","Jun",
        "Jul","Aug","Sep","Oct","Nov","Dec"
    };
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

    const char *mname = (m >= 1 && m <= 12) ? mo[m - 1] : "   ";
    out[0] = mname[0]; out[1] = mname[1]; out[2] = mname[2];
    out[3] = ' ';
    out[4] = (d >= 10) ? (char)('0' + (d / 10) % 10) : ' ';
    out[5] = (char)('0' + d % 10);
    out[6] = ' ';
    out[7] = (char)('0' + (hh / 10) % 10);
    out[8] = (char)('0' + hh % 10);
    out[9] = ':';
    out[10] = (char)('0' + (mm / 10) % 10);
    out[11] = (char)('0' + mm % 10);
    out[12] = 0;
}

/* size with units, right-aligned in 7 chars (format_size_with_units) */
static void mc_fmt_size(char *out, uint32_t v) {
    if (v < 100000) {
        mc_fmt_num(out, 7, v);
        return;
    }
    char tmp[8];
    int i = 0;
    uint32_t q; char unit;
    if (v < 1024u * 1024u) { q = v / 1024u; unit = 'K'; }
    else if (v < 1024u * 1024u * 1024u) { q = v / (1024u * 1024u); unit = 'M'; }
    else { q = v / (1024u * 1024u * 1024u); unit = 'G'; }
    if (!q) { tmp[i++] = '0'; }
    while (q) { tmp[i++] = (char)('0' + q % 10); q /= 10; }
    tmp[i++] = unit;
    /* right-align */
    for (int k = 0; k < 7; k++) out[k] = ' ';
    int pos = 7;
    for (int k = 0; k < i && pos > 0; k++) out[--pos] = tmp[k];
}

/* does name end with any of the suffixes? */
static int name_has_ext(const char *name, const char *const *exts) {
    int len = (int)strlen(name);
    for (int i = 0; exts[i]; i++) {
        int el = (int)strlen(exts[i]);
        if (len > el && !strcmp(name + len - el, exts[i])) return 1;
    }
    return 0;
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

/* inner layout for w=40 (inner=38): prefix(1) name(16) | size(7) | date(12) */
#define NAME_W_INNER(p) ((p)->w - 2 - 22)

static uint8_t entry_color(const McEntry *e, int is_cursor, int active) {
    if (is_cursor) return active ? MC_CURSOR : MC_INACTIVE;
    if (e->is_dir) return MC_DIR;
    if (e->is_exec) return MC_EXEC;
    static const char *const archives[] = { ".gz", ".tar", ".xz", 0 };
    static const char *const sources[]  = { ".c", ".h", ".sh", ".php", 0 };
    if (name_has_ext(e->name, archives)) return MC_ARCH;
    if (name_has_ext(e->name, sources)) return MC_SRC;
    return MC_FILE;
}

static char entry_prefix(const McEntry *e) {
    if (e->is_dir) return '/';
    if (e->is_exec) return '*';
    return ' ';
}

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

    /* path chip: active panel black-on-white, inactive white-on-blue */
    int py = p->y + 1;
    uint8_t pcol = active ? MC_PATH : COLOR(0xF,0x1);
    for (int i = 0; i < inner; i++) vga_putc_at(' ', p->x + 1 + i, py, pcol);
    vga_puts_at(p->path, p->x + 2, py, pcol);

    /* column header: Name | Size | Modify time */
    int hy = p->y + 2;
    for (int i = 0; i < inner; i++) vga_putc_at(' ', p->x + 1 + i, hy, MC_HEADER);
    vga_puts_at("Name", p->x + 2, hy, MC_HEADER);
    vga_puts_at("Size", p->x + 1 + 21, hy, MC_HEADER);
    vga_puts_at("Modify time", p->x + 1 + 27, hy, MC_HEADER);

    /* rows */
    int max_rows = p->h - 5;
    int list_y = p->y + 3;
    for (int i = 0; i < max_rows; i++) {
        McEntry *e = (i < p->count) ? &p->entries[i] : 0;
        uint8_t col = e ? entry_color(e, i == p->cursor, active) : MC_FILE;

        char line[128];
        int li = 0;
        int lim = inner;
        line[li++] = e ? entry_prefix(e) : ' ';
        if (e) {
            const char *n = e->name;
            while (*n && li < 1 + name_w) line[li++] = *n++;
        }
        while (li < 1 + name_w) line[li++] = ' ';
        line[li++] = '|';
        if (!e) {
            while (li < lim) line[li++] = ' ';
        } else if (!strcmp(e->name, "..")) {
            const char *u = "UP--DIR";
            for (int k = 0; k < 7; k++) line[li++] = u[k];
        } else {
            char f[8];
            mc_fmt_size(f, e->size);
            for (int k = 0; k < 7; k++) line[li++] = f[k];
        }
        if (e) {
            line[li++] = '|';
            char dt[13];
            mc_fmt_date(dt, e->mtime);
            for (int k = 0; k < 12; k++) line[li++] = dt[k];
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

/* ---------- status messages and prompts (bottom bar row) ---------- */

static int msg_pending = 0;
static char msgbuf[80];

static void show_msg(const char *s) {
    strncpy(msgbuf, s, sizeof(msgbuf) - 1);
    msgbuf[sizeof(msgbuf) - 1] = 0;
    msg_pending = 1;
}

static void msg2(const char *a, const char *b) {
    strncpy(msgbuf, a, sizeof(msgbuf) - 1);
    size_t l = strlen(msgbuf);
    strncpy(msgbuf + l, b, sizeof(msgbuf) - 1 - l);
    msgbuf[sizeof(msgbuf) - 1] = 0;
    msg_pending = 1;
}

static void paint_msg(void) {
    for (int x = 0; x < VGA_W; x++)
        vga_putc_at(' ', x, VGA_H - 1, COLOR(0xF,0x4));
    vga_puts_at(msgbuf, 0, VGA_H - 1, COLOR(0xF,0x4));
}

/* line input on the bar row; returns 1 on ENTER, 0 on ESC */
static int prompt_input(const char *prompt, char *out, int maxlen) {
    for (int x = 0; x < VGA_W; x++)
        vga_putc_at(' ', x, VGA_H - 1, COLOR(0xF,0x4));
    vga_puts_at(prompt, 0, VGA_H - 1, COLOR(0xF,0x4));
    int px = (int)strlen(prompt);
    int n = 0;
    vga_putc_at('_', px + n, VGA_H - 1, COLOR(0xE,0x4));
    for (;;) {
        if (!kbd_has_data()) continue;
        uint8_t sc = kbd_read_scancode();
        if (sc == SC_ESC) return 0;
        if (sc == SC_ENTER) { out[n] = 0; return 1; }
        if (sc == SC_BACK) {
            if (n > 0) {
                n--;
                vga_putc_at(' ', px + n, VGA_H - 1, COLOR(0xF,0x4));
                vga_putc_at('_', px + n, VGA_H - 1, COLOR(0xE,0x4));
            }
            continue;
        }
        char c = scancode_to_char(sc);
        if (c && n < maxlen - 1) {
            vga_putc_at(c, px + n, VGA_H - 1, COLOR(0xF,0x4));
            out[n++] = c;
            vga_putc_at('_', px + n, VGA_H - 1, COLOR(0xE,0x4));
        }
    }
}

static int prompt_yn(const char *prompt) {
    char ans[2];
    if (!prompt_input(prompt, ans, sizeof(ans))) return 0;
    return ans[0] == 'y' || ans[0] == 'Y';
}

/* ---------- file operations (F5 copy, F6 move, F7 mkdir, F8 delete) ---------- */

static void op_copy_move(int move) {
    serial_puts(move ? "[mc] op: move\r\n" : "[mc] op: copy\r\n");
    McPanel *src = active_panel;
    McPanel *dst = (src == &left_panel) ? &right_panel : &left_panel;
    if (src->count == 0) return;
    McEntry *e = &src->entries[src->cursor];
    if (!strcmp(e->name, "..") || e->is_dir) {
        show_msg(move ? "Cannot move directories" : "Cannot copy directories");
        return;
    }

    uint32_t size;
    if (!ext2_read_file(e->ino, filebuf, sizeof(filebuf) - 1, &size)) {
        show_msg("Read failed");
        return;
    }

    uint32_t exist;
    if (ext2w_lookup(dst->cur_ino, e->name, &exist, 0)) {
        show_msg("Target already exists");
        return;
    }

    uint32_t nino = ext2w_create(dst->cur_ino, e->name, 0, 0x81A4);
    if (!nino) { show_msg("Create failed (disk full?)"); return; }
    if (!ext2w_write_file(nino, filebuf, size)) { show_msg("Write failed"); return; }

    if (move) {
        if (!ext2w_unlink(src->cur_ino, e->name)) { show_msg("Delete failed"); return; }
        panel_load(src);
        msg2("Moved ", e->name);
    } else {
        msg2("Copied ", e->name);
    }
    panel_load(dst);
}

static void op_mkdir(void) {
    serial_puts("[mc] op: mkdir\r\n");
    char name[32];
    if (!prompt_input("Mkdir name: ", name, sizeof(name))) return;
    if (!name[0]) return;
    if (ext2w_lookup(active_panel->cur_ino, name, 0, 0)) {
        show_msg("Already exists");
        return;
    }
    if (!ext2w_create(active_panel->cur_ino, name, 1, 0x41FF)) {
        show_msg("Mkdir failed");
        return;
    }
    panel_load(active_panel);
    show_msg("Directory created");
}

static void op_delete(void) {
    serial_puts("[mc] op: delete\r\n");
    McPanel *p = active_panel;
    if (p->count == 0) return;
    McEntry *e = &p->entries[p->cursor];
    if (!strcmp(e->name, "..")) return;
    if (e->is_dir && !ext2w_dir_is_empty(e->ino)) {
        show_msg("Directory not empty");
        return;
    }
    msg2("Delete ", e->name);
    strncat(msgbuf, "? (y/n)", sizeof(msgbuf) - strlen(msgbuf) - 1);
    if (!prompt_yn(msgbuf)) return;
    if (!ext2w_unlink(p->cur_ino, e->name)) { show_msg("Delete failed"); return; }
    panel_load(p);
    show_msg("Deleted");
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
        msg_pending = 0;

        if (sc == SC_TAB) {
            active_panel = (active_panel == &left_panel) ? &right_panel : &left_panel;
        } else if (sc == SC_UP) {
            if (active_panel->cursor > 0) active_panel->cursor--;
        } else if (sc == SC_DOWN) {
            if (active_panel->cursor < active_panel->count - 1) active_panel->cursor++;
        } else if (sc == SC_F5) {
            op_copy_move(0);
        } else if (sc == SC_F6) {
            op_copy_move(1);
        } else if (sc == SC_F7) {
            op_mkdir();
        } else if (sc == SC_F8) {
            op_delete();
        } else if (sc == SC_F9) {
            show_msg("F9: no menu yet");
        } else if (sc == SC_F10) {
            show_msg("F10: nowhere to quit");
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
                    if (sc == SC_F3) {
                        editor_view((const char*)filebuf, size, e->name);
                    } else {
                        size_t newlen = editor_run((char*)filebuf, size,
                                                   sizeof(filebuf), e->name);
                        ext2w_write_file(e->ino, filebuf, (uint32_t)newlen);
                    }
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
        if (msg_pending) paint_msg();
    }
}
