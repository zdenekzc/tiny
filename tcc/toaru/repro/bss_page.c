/* .bss past the page boundary — before the fix in tcc-toaru.patch this work
 * killed the whole VM (kernel panic in elf_exec, not a SIGSEGV of the program).
 *
 * Reason: the loadsric of ToaruOS 2.3.2 maps PT_LOAD page by page so that it
 * counts the 0x1000 step FROM p_vaddr (kernel/misc/elf64.c:339). If p_vaddr is
 * not page-aligned, the loop runs ceil(p_memsz/0x1000) times and skips the last
 * page; right afterwards the kernel zeroes .bss into an unmapped address => panic.
 * The previous text in ../README.md attributed the crash of this work to the
 * order of constructors — it had nothing to do with that, only the size of .bss
 * decided.
 *
 * This program has a deliberately large .bss (12 kB), so the RW segment must
 * exceed more than one page. In addition it verifies that zero really is zero
 * (not overwritten by one of the preceding segments) and that .data stayed its own.
 *
 * Expected output: bss_page: zeroed=ok initialized=0x5a5a last=7, return 0.
 * See ../README.md and toaru-elfpad.py --check. */
#include <stdio.h>

static char zeroed[12 * 1024];        /* .bss: not a single byte of it in the file */
static int initialized = 0x5a5a;      /* .data: on the page before .bss, must survive */

int main(void) {
    int i, first = -1;

    for (i = 0; i < (int)sizeof zeroed; ++i) {
        if (zeroed[i]) {
            first = i;
            break;
        }
    }
    zeroed[sizeof zeroed - 1] = 7;    /* .bss must be writable too */

    printf("bss_page: zeroed=%s initialized=0x%x last=%d\n",
           first < 0 ? "ok" : "WRONG", initialized, zeroed[sizeof zeroed - 1]);
    return first < 0 && initialized == 0x5a5a && zeroed[sizeof zeroed - 1] == 7 ? 0 : 1;
}
