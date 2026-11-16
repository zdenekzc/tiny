#ifndef VGA_H
#define VGA_H

#include <stddef.h>
#include <stdint.h>

/* VGA text mode constants */
#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define VGA_ATTR_BLUE_BG_WHITE_FG 0x1F /* background=blue (1), foreground=white (F) */

void vga_clear(void);
void vga_putc(char c, uint8_t attr);
void vga_write(const char *buf, size_t len, uint8_t attr);
void vga_draw_panel(int x, int y, int w, int h, uint8_t attr);

#endif /* VGA_H */