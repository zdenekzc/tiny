#include "libtcc.h"
#include <stdint.h>

extern void vga_puts_at(const char*, int, int, uint8_t);
extern void vga_putc_at(char, int, int, uint8_t);

static TCCState *S = NULL;

static void tcc_register_symbols(void) {
    tcc_add_symbol(S, "vga_puts_at", vga_puts_at);
    tcc_add_symbol(S, "vga_putc_at", vga_putc_at);
    /* add more kernel functions here */
    
    /*
    tcc_add_symbol(S, "vga_puts_at", vga_puts_at);
    tcc_add_symbol(S, "vga_putc_at", vga_putc_at);
    
    tcc_add_symbol(S, "memcpy", memcpy);
    tcc_add_symbol(S, "memset", memset);
    tcc_add_symbol(S, "strlen", strlen);
    
    tcc_add_symbol(S, "ext2_read_file", ext2_read_file);
    tcc_add_symbol(S, "ext2_write_file_overwrite", ext2_write_file_overwrite);

    tcc_add_symbol(S, "putpixel", putpixel);
    tcc_add_symbol(S, "line", line);
    tcc_add_symbol(S, "draw_triangle", draw_triangle);
    
    tcc_add_symbol(S, "ipv4_send_udp", ipv4_send_udp);
    
    
    tcc_add_symbol(S, "puts", puts);
    tcc_add_symbol(S, "putc", putc);
    
    tcc_add_symbol(S, "puts", puts);
    tcc_add_symbol(S, "write", write);
    tcc_add_symbol(S, "exit", exit);
    
    */
}

void tcc_init_jit(void) {
    S = tcc_new();
    tcc_set_output_type(S, TCC_OUTPUT_MEMORY);
    tcc_register_symbols();
}

int tcc_run_string(const char *code) {
    if (!S)
        tcc_init_jit();

    if (tcc_compile_string(S, code) < 0)
        return -1;

    int (*entry)(void) = tcc_get_symbol(S, "main");
    if (!entry)
        return -2;

    return entry();
}
