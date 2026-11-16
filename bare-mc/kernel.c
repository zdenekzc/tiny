#include "multiboot.h"
#include "vga.h"
#include <stddef.h>
#include <stdint.h>

/* Entry point for the kernel. GRUB jumps to this function after loading.
 * We use the multiboot2 header defined in multiboot.h.
 */
__attribute__((section(".text")))
void _start(void) {
    // Clear the VGA screen
    vga_clear();

    // Draw two blue panels. For simplicity we use 20x10 panels.
    // Panel 1 at (2,2)
    vga_draw_panel(2, 2, 20, 10, VGA_ATTR_BLUE_BG_WHITE_FG);
    // Panel 2 at (40,2)
    vga_draw_panel(40, 2, 20, 10, VGA_ATTR_BLUE_BG_WHITE_FG);

    // Halt the CPU – infinite loop (so QEMU stays on screen)
    for (;;) {
        __asm__ volatile ("hlt");
    }
}
