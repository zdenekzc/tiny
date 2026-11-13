/* exit3.c — returns 3. This is not an attempt at that fatal bug (which is analyzed
 * in ../README.md), but at another older record in the README: "the exit code of a
 * program in the guest cannot be verified via $? in esh, only by the text".
 *
 * Measured 2026-11-12: esh answered `==RC-exit3-...== 0`, even though the program
 * returned 3 — and the code really does get into the kernel, because pre_main() ends
 * with `exit(main(argc, argv))` (libc/main.c) and our crt0.o is from the ISO. So it
 * is not lost on our side; the rule still holds: we cannot find out the exit code of
 * a program in the guest through the shell, only the text of the output is compared. */
#include <stdio.h>

int main(void) {
    printf("exit3: returning 3\n");
    fflush(stdout);
    return 3;
}
