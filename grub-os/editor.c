/* Tiny editor / viewer for the mini commander.
 * Lines are pointers into the caller's buffer (content stays intact,
 * including '\n' bytes) so the buffer can be written back unchanged.
 * Redraws only after key events — no flicker. */

#include "editor.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define VGA_W     80
#define VGA_H     25
#define COLOR(fg,bg) ((fg) | ((bg) << 4))

extern void vga_clear(void);
extern void vga_putc_at(char c, int x, int y, uint8_t color);
extern void vga_puts_at(const char *s, int x, int y, uint8_t color);
extern int  kbd_has_data(void);
extern uint8_t kbd_read_scancode(void);
extern char scancode_to_char(uint8_t sc);

#define SC_LEFT   0x4B
#define SC_RIGHT  0x4D
#define SC_UP     0x48
#define SC_DOWN   0x50
#define SC_HOME   0x47
#define SC_END    0x4F
#define SC_PGUP   0x49
#define SC_PGDN   0x51
#define SC_BACK   0x0E
#define SC_DEL    0x53
#define SC_ESC    0x01
#define SC_ENTER  0x1C
#define SC_F3     0x3D
#define SC_F10    0x44

#define STATUS_ROW 23
#define EDIT_ROWS 22       /* text rows 1..22 */
#define MAX_LINES 256

static char  *line_ptr[MAX_LINES];
static int    line_len[MAX_LINES];
static int    line_count;
static int    cx, cy;        /* cursor in lines, not screen rows */
static int    top;           /* first visible line */
static size_t buf_used;
static size_t buf_cap;
static int    readonly;
static char   edit_title[64];

static void rebuild(char *buf) {
    line_count = 0;
    size_t pos = 0;
    while (pos < buf_used && line_count < MAX_LINES) {
        size_t l = 0;
        while (pos + l < buf_used && buf[pos + l] != '\n') l++;
        line_ptr[line_count] = buf + pos;
        line_len[line_count] = (int)l;
        line_count++;
        pos += l + 1;
    }
    if (line_count == 0) {
        line_ptr[0] = buf;
        line_len[0] = 0;
        line_count = 1;
    }
    if (cy >= line_count) cy = line_count - 1;
    if (top > line_count - 1) top = line_count - 1 > 0 ? line_count - 1 : 0;
}

static void clamp_cx(void) {
    if (cx > line_len[cy]) cx = line_len[cy];
    if (cx < 0) cx = 0;
}

static void scroll_fix(void) {
    if (cy < top) top = cy;
    if (cy >= top + EDIT_ROWS) top = cy - EDIT_ROWS + 1;
    if (top < 0) top = 0;
}

static void editor_draw(void) {
    vga_clear();
    vga_puts_at(edit_title, 0, 0, COLOR(0xF,0x0));

    for (int row = 0; row < EDIT_ROWS; row++) {
        int li = top + row;
        /* blank the row, then the text */
        for (int x = 0; x < VGA_W; x++)
            vga_putc_at(' ', x, row + 1, COLOR(0xF,0x0));
        if (li < line_count) {
            const char *s = line_ptr[li];
            int lim = line_len[li];
            if (lim > VGA_W - 2) lim = VGA_W - 2;
            for (int i = 0; i < lim; i++)
                vga_putc_at(s[i], 1 + i, row + 1, COLOR(0xF,0x0));
        }
    }

    /* status line */
    {
        char st[96];
        int li = 0;
        const char *mode = readonly ? "[view]  " : "[edit]  ";
        const char *m = mode;
        while (*m && li < (int)sizeof(st) - 1) st[li++] = *m++;
        const char *t = edit_title;
        while (*t && li < (int)sizeof(st) - 1) st[li++] = *t++;
        m = "  line ";
        while (*m && li < (int)sizeof(st) - 1) st[li++] = *m++;
        /* line number */
        {
            char nb[8];
            int v = cy + 1, i = 0;
            if (!v) nb[i++] = '0';
            while (v) { nb[i++] = (char)('0' + v % 10); v /= 10; }
            while (i && li < (int)sizeof(st) - 1) st[li++] = nb[--i];
        }
        m = "/";
        while (*m && li < (int)sizeof(st) - 1) st[li++] = *m++;
        {
            char nb[8];
            int v = line_count, i = 0;
            if (!v) nb[i++] = '0';
            while (v) { nb[i++] = (char)('0' + v % 10); v /= 10; }
            while (i && li < (int)sizeof(st) - 1) st[li++] = nb[--i];
        }
        st[li] = 0;
        for (int x = 0; x < VGA_W; x++)
            vga_putc_at(' ', x, STATUS_ROW, COLOR(0x0,0x7));
        vga_puts_at(st, 0, STATUS_ROW, COLOR(0x0,0x7));
    }

    /* cursor */
    if (cy >= top && cy < top + EDIT_ROWS && cx < VGA_W - 2)
        vga_putc_at('_', 1 + cx, 1 + (cy - top), COLOR(0xE,0x0));
}

static void insert_byte(char *buf, uint32_t pos, char c) {
    if (buf_used >= buf_cap) return;
    memmove(buf + pos + 1, buf + pos, buf_used - pos);
    buf[pos] = c;
    buf_used++;
}

static void delete_byte(char *buf, uint32_t pos) {
    if (pos >= buf_used) return;
    memmove(buf + pos, buf + pos + 1, buf_used - pos - 1);
    buf_used--;
}

size_t editor_run(char *buf, size_t len, size_t cap, const char *title) {
    strncpy(edit_title, title ? title : "", sizeof(edit_title) - 1);
    edit_title[sizeof(edit_title) - 1] = 0;
    readonly = 0;
    buf_used = len;
    buf_cap = cap;
    cx = cy = top = 0;
    rebuild(buf);
    editor_draw();

    for (;;) {
        if (!kbd_has_data()) continue;
        uint8_t sc = kbd_read_scancode();

        if (sc == SC_ESC || sc == SC_F10) return buf_used;

        if (sc == SC_LEFT && cx > 0) cx--;
        else if (sc == SC_RIGHT) {
            if (cx < line_len[cy]) cx++;
        } else if (sc == SC_UP && cy > 0) cy--;
        else if (sc == SC_DOWN && cy < line_count - 1) cy++;
        else if (sc == SC_HOME) cx = 0;
        else if (sc == SC_END) cx = line_len[cy];
        else if (sc == SC_PGUP) { cy -= EDIT_ROWS - 1; if (cy < 0) cy = 0; }
        else if (sc == SC_PGDN) {
            cy += EDIT_ROWS - 1;
            if (cy > line_count - 1) cy = line_count - 1;
        } else if (!readonly) {
            uint32_t pos = (uint32_t)(line_ptr[cy] - buf) + (uint32_t)cx;
            if (sc == SC_BACK) {
                if (cx > 0) {
                    delete_byte(buf, pos - 1);
                    cx--;
                    rebuild(buf);
                } else if (cy > 0) {
                    delete_byte(buf, pos - 1);  /* the '\n' before this line */
                    cy--;
                    rebuild(buf);
                    cx = line_len[cy];
                }
            } else if (sc == SC_DEL) {
                if (cx < line_len[cy]) {
                    delete_byte(buf, pos);
                    rebuild(buf);
                } else if (cy < line_count - 1) {
                    delete_byte(buf, pos);      /* the '\n' after this line */
                    rebuild(buf);
                }
            } else if (sc == SC_ENTER) {
                insert_byte(buf, pos, '\n');
                rebuild(buf);
                cy++;
                cx = 0;
            } else {
                char c = scancode_to_char(sc);
                if (c >= 32 && c <= 126) {
                    insert_byte(buf, pos, c);
                    rebuild(buf);
                    cx++;
                }
            }
        }

        clamp_cx();
        scroll_fix();
        editor_draw();
    }
    return buf_used;
}

void editor_view(const char *buf, size_t len, const char *title) {
    strncpy(edit_title, title ? title : "", sizeof(edit_title) - 1);
    edit_title[sizeof(edit_title) - 1] = 0;
    readonly = 1;
    buf_used = len;
    buf_cap = 0;
    cx = cy = top = 0;
    rebuild((char*)buf);
    editor_draw();

    for (;;) {
        if (!kbd_has_data()) continue;
        uint8_t sc = kbd_read_scancode();

        if (sc == SC_ESC || sc == SC_F3 || sc == SC_F10) return;
        if (sc == SC_UP && cy > 0) cy--;
        else if (sc == SC_DOWN && cy < line_count - 1) cy++;
        else if (sc == SC_PGUP) { cy -= EDIT_ROWS - 1; if (cy < 0) cy = 0; }
        else if (sc == SC_PGDN) {
            cy += EDIT_ROWS - 1;
            if (cy > line_count - 1) cy = line_count - 1;
        }

        scroll_fix();
        editor_draw();
    }
}
