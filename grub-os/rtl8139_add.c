#include "rtl8139.h"

void kernel_main(...) {
    /* ... existing init ... */

    if (rtl8139_init()) {
        vga_puts_at("RTL8139 up", 0, 3, COLOR(0xA,0x0));
    } else {
        vga_puts_at("RTL8139 not found", 0, 3, COLOR(0xC,0x0));
    }

    for (;;) {
        /* poll NIC if you don’t have IRQ wired yet */
        rtl8139_poll();

        /* your existing net stack poll, shell, etc. */
    }
}
