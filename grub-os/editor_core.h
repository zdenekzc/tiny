#pragma once
#include <stddef.h>

/* Portable editor/viewer core. No VGA, no I/O ports, no OS calls —
 * the same file compiles for the bare-metal kernel and for Linux.
 * The host backend feeds scancodes via ec_key() and blits the
 * framebuffer returned by ec_render() to the real screen.
 * Scancodes are PS/2 set 1 (what the bare-metal keyboard gives us). */

#define EC_W 80
#define EC_H 25
#define EC_TEXT_ROWS 22     /* text rows 1..22 */
#define EC_STATUS_ROW 23

enum {
    EC_KEY_NONE  = 0,
    EC_KEY_ESC   = 0x01,
    EC_KEY_BACK  = 0x0E,
    EC_KEY_TAB   = 0x0F,
    EC_KEY_F3    = 0x3D,
    EC_KEY_F10   = 0x44,
    EC_KEY_ENTER = 0x1C,
    EC_KEY_HOME  = 0x47,
    EC_KEY_UP    = 0x48,
    EC_KEY_PGUP  = 0x49,
    EC_KEY_LEFT  = 0x4B,
    EC_KEY_RIGHT = 0x4D,
    EC_KEY_END   = 0x4F,
    EC_KEY_DOWN  = 0x50,
    EC_KEY_PGDN  = 0x51,
    EC_KEY_DEL   = 0x53
};

typedef struct { unsigned char ch, attr; } ec_cell;

/* open a session; buf stays intact (lines are ptr+len slices) */
void   ec_open_edit(char *buf, size_t len, size_t cap, const char *title);
void   ec_open_view(const char *buf, size_t len, const char *title);
void   ec_key(int sc);            /* feed one scancode */
int    ec_done(void);             /* nonzero once the user exited */
size_t ec_result_len(void);       /* content length after editing */
void   ec_render(ec_cell *screen);/* render EC_W*EC_H cells */

/* reverse lookup for hosts that have real characters, not scancodes */
int    ec_scancode_for_char(char c);
