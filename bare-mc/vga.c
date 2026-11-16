#include "vga.h"
#include <stddef.h>
#include <string.h>

/* VGA text mode buffer is located at physical address 0xB8000.
 * In QEMU we can directly write to this address because we are running
 * in flat 64‑bit mode without paging.
 */
static volatile uint16_t *vga_buffer = (uint16_t *)0xB8000;

static int cursor_x = 0;
static int cursor_y = 0;

static void move_cursor(void) {
    // For this simple demo we ignore updating the hardware cursor.
    (void)cursor_x; (void)cursor_y;
}

void vga_clear(void) {
    for (int y = 0; y < VGA_HEIGHT; ++y) {
        for (int x = 0; x < VGA_WIDTH; ++x) {
            vga_buffer[y * VGA_WIDTH + x] = (' ' ) | (VGA_ATTR_BLUE_BG_WHITE_FG << 8);
        }
    }
    cursor_x = cursor_y = 0;
    move_cursor();
}

void vga_putc(char c, uint8_t attr) {
    if (c == '\n') {
        cursor_x = 0;
        cursor_y++;
        if (cursor_y >= VGA_HEIGHT) cursor_y = 0;
        return;
    }
    if (c == '\r') {
        cursor_x = 0;
        return;
    }
    if (cursor_x >= VGA_WIDTH) {
        cursor_x = 0;
        cursor_y++;
    }
    if (cursor_y >= VGA_HEIGHT) {
        cursor_y = 0;
    }
    vga_buffer[cursor_y * VGA_WIDTH + cursor_x] = ((uint16_t)c) | ((uint16_t)attr << 8);
    cursor_x++;
    move_cursor();
}

void vga_write(const char *buf, size_t len, uint8_t attr) {
    for (size_t i = 0; i < len; ++i) {
        vga_putc(buf[i], attr);
    }
}

void vga_draw_panel(int x, int y, int w, int h, uint8_t attr) {
    // Clamp to screen bounds
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    if (x + w > VGA_WIDTH) w = VGA_WIDTH - x;
    if (y + h > VGA_HEIGHT) h = VGA_HEIGHT - y;

    for (int row = 0; row < h; ++row) {
        for (int col = 0; col < w; ++col) {
            int pos = (y + row) * VGA_WIDTH + (x + col);
            vga_buffer[pos] = (' ') | ((uint16_t)attr << 8);
        }
    }
}
