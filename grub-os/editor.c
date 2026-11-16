/* Thin bare-metal backend for the portable editor core
 * (editor_core.c): PS/2 scancodes in, 80x25 VGA blit out. */

#include "editor.h"
#include "editor_core.h"
#include <stdint.h>
#include <stddef.h>

#define VGA_MEM ((volatile uint16_t*)0xB8000)

extern int  kbd_has_data(void);
extern uint8_t kbd_read_scancode(void);

static void blit(ec_cell *scr) {
    for (int i = 0; i < EC_W * EC_H; i++)
        VGA_MEM[i] = (uint16_t)((scr[i].attr << 8) | scr[i].ch);
}

static ec_cell screen[EC_W * EC_H];

size_t editor_run(char *buf, size_t len, size_t cap, const char *title) {
    ec_open_edit(buf, len, cap, title);
    ec_render(screen);
    blit(screen);
    while (!ec_done()) {
        if (!kbd_has_data()) continue;
        ec_key(kbd_read_scancode());
        ec_render(screen);
        blit(screen);
    }
    return ec_result_len();
}

void editor_view(const char *buf, size_t len, const char *title) {
    ec_open_view(buf, len, title);
    ec_render(screen);
    blit(screen);
    while (!ec_done()) {
        if (!kbd_has_data()) continue;
        ec_key(kbd_read_scancode());
        ec_render(screen);
        blit(screen);
    }
}
