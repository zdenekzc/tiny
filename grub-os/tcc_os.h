#include "libtcc.h"
#include <stdint.h>
#include <stddef.h>

static TCCState *S = NULL;

void tcc_init_jit(void) {
    S = tcc_new();
    tcc_set_output_type(S, TCC_OUTPUT_MEMORY);
}

int tcc_run_string(const char *code) {
    if (!S) tcc_init_jit();

    if (tcc_compile_string(S, code) < 0)
        return -1;

    int (*entry)(void) = tcc_get_symbol(S, "main");
    if (!entry)
        return -2;

    return entry();
}
