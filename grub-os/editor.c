#include "editor.h"
#include <stdint.h>
#include <stddef.h>

#define VGA_MEM   ((uint16_t*)0xB8000)
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
#define SC_BACK   0x0E
#define SC_ESC    0x01

#define EDIT_H  23
#define MAX_LINES 256
#define MAX_LINE_LEN 256

static char *edit_lines[MAX_LINES];
static int line_count;
static int cx, cy;

static void editor_draw(void) {
    vga_clear();
    vga_puts_at("[ESC=exit] Tiny editor", 0, 0, COLOR(0xF,0x0));

    for (int y = 0; y < EDIT_H && y < line_count; y++)
        vga_puts_at(edit_lines[y], 1, y + 1, COLOR(0xF,0x0));

    vga_putc_at('_', cx + 1, cy + 1, COLOR(0xE,0x0));
}

static void editor_load(char *buf, size_t len) {
    line_count = 0;
    cx = cy = 0;
    size_t pos = 0;

    while (pos < len && line_count < MAX_LINES) {
        edit_lines[line_count] = buf + pos;
        size_t l = 0;
        while (pos + l < len && buf[pos + l] != '\n' && l < MAX_LINE_LEN-1)
            l++;
        buf[pos + l] = 0;
        pos += l + 1;
        line_count++;
    }
    if (line_count == 0) {
        edit_lines[0] = buf;
        buf[0] = 0;
        line_count = 1;
    }
}

static void editor_insert_char(char c) {
    char *line = edit_lines[cy];
    size_t len = 0;
    while (line[len]) len++;
    if (len >= MAX_LINE_LEN-1) return;

    for (size_t i = len + 1; i > (size_t)cx; i--)
        line[i] = line[i-1];
    line[cx++] = c;
}

static void editor_backspace(void) {
    if (cx == 0) return;
    char *line = edit_lines[cy];
    size_t len = 0;
    while (line[len]) len++;
    for (size_t i = cx - 1; i < len; i++)
        line[i] = line[i+1];
    cx--;
}

void editor_run(char *buf, size_t len) {
    editor_load(buf, len);
    editor_draw();

    for (;;) {
        if (!kbd_has_data()) continue;
        uint8_t sc = kbd_read_scancode();

        if (sc == SC_ESC) return;
        if (sc == SC_LEFT && cx > 0) cx--;
        else if (sc == SC_RIGHT) {
            size_t l = 0; while (edit_lines[cy][l]) l++;
            if (cx < (int)l) cx++;
        } else if (sc == SC_UP && cy > 0) {
            cy--;
            size_t l = 0; while (edit_lines[cy][l]) l++;
            if (cx > (int)l) cx = (int)l;
        } else if (sc == SC_DOWN && cy < line_count-1) {
            cy++;
            size_t l = 0; while (edit_lines[cy][l]) l++;
            if (cx > (int)l) cx = (int)l;
        } else if (sc == SC_BACK) {
            editor_backspace();
        } else {
            char c = scancode_to_char(sc);
            if (c >= 32 && c <= 126) editor_insert_char(c);
        }

        /* Redraw only after an input event — continuous repainting in the
         * idle loop caused heavy flicker. */
        editor_draw();
    }
}
