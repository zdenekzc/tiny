/* Linux test harness for the portable editor core (grub-os/editor_core.c).
 *
 * Build:   gcc tests/edit-linux.c grub-os/editor_core.c -I grub-os -o /tmp/edit-test
 * Selftest (automated):   /tmp/edit-test
 * Interactive (in a tty): /tmp/edit-test interactive
 *
 * The interactive mode maps terminal keys to the PS/2 scancodes the
 * bare-metal kernel uses, renders the 80x25 framebuffer, and is what
 * the user sees — the exact same core then runs in the kernel. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "editor_core.h"

/* ---------- selftest ---------- */

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (cond) printf("PASS: %s\n", (msg)); \
    else { printf("FAIL: %s\n", (msg)); failures++; } \
} while (0)

static void press(int sc) { ec_key(sc); }

static void type(const char *s) {
    for (const char *p = s; *p; p++)
        press(ec_scancode_for_char(*p));
}

static void test_edit_typing(void) {
    char buf[256];
    strcpy(buf, "alpha\nbeta\ngamma");
    ec_open_edit(buf, strlen(buf), sizeof(buf), "t1");
    type("X");
    CHECK(ec_result_len() == strlen("alpha\nbeta\ngamma") + 1, "typing increases length");
    CHECK(strncmp(buf, "xalpha", 6) == 0, "typed char lands at cursor");
    press(EC_KEY_ESC);
    CHECK(ec_done(), "ESC finishes session");
}

static void test_enter_split(void) {
    char buf[64];
    strcpy(buf, "ab");
    ec_open_edit(buf, 2, sizeof(buf), "t2");
    /* move to end of line, split */
    press(EC_KEY_END);
    press(EC_KEY_ENTER);
    CHECK(ec_result_len() == 3, "Enter inserts newline");
    type("cd");
    buf[ec_result_len()] = 0;
    CHECK(strcmp(buf, "ab\ncd") == 0, "typing after Enter goes to new line");
    /* backspace at column 0 merges the lines back */
    press(EC_KEY_HOME);
    press(EC_KEY_BACK);
    buf[ec_result_len()] = 0;
    CHECK(strcmp(buf, "abcd") == 0 && ec_result_len() == 4, "Backspace merges lines");
    press(EC_KEY_ESC);
}

static void test_del_join(void) {
    char buf[64];
    strcpy(buf, "ab\ncd");
    ec_open_edit(buf, 5, sizeof(buf), "t3");
    press(EC_KEY_END);   /* end of "ab" */
    press(EC_KEY_DEL);   /* join next line */
    buf[ec_result_len()] = 0;
    CHECK(strcmp(buf, "abcd") == 0, "Del joins lines");
    press(EC_KEY_ESC);
}

static void test_autoindent(void) {
    char buf[128];
    strcpy(buf, "  abc\n  xyz");
    ec_open_edit(buf, 11, sizeof(buf), "t4");
    press(EC_KEY_END);     /* end of "  abc" */
    press(EC_KEY_ENTER);   /* new empty line inherits the indent */
    buf[ec_result_len()] = 0;
    CHECK(strncmp(buf, "  abc\n  ", 8) == 0, "Enter auto-indents new line");
    press(EC_KEY_ESC);
}

static void test_tab(void) {
    char buf[64];
    buf[0] = 0;
    ec_open_edit(buf, 0, sizeof(buf), "t5");
    press(EC_KEY_TAB);
    CHECK(ec_result_len() == 1 && buf[0] == '\t', "Tab inserts tab");
    press(EC_KEY_ESC);
}

static void test_viewer_scroll(void) {
    static char big[2048];
    int pos = 0;
    for (int i = 1; i <= 40; i++)
        pos += sprintf(big + pos, "line%02d\n", i);
    ec_open_view(big, pos, "t6");
    for (int i = 0; i < 22; i++) press(EC_KEY_DOWN); /* one page down */
    ec_cell scr[EC_W * EC_H];
    ec_render(scr);
    /* after 22 downs: cy=22, top=1 -> first visible line is line02 */
    char first[80];
    for (int x = 0; x < 79; x++) first[x] = (char)scr[EC_W + x].ch;
    first[79] = 0;
    CHECK(strstr(first, "line02") != NULL, "PgDown scrolls viewport");
    press(EC_KEY_ESC);
}

static void test_viewer_search(void) {
    char buf[256];
    strcpy(buf, "one two\nneedle here\nthree\nneedle again\nfive");
    ec_open_view(buf, strlen(buf), "t7");
    press('/');          /* search prompt */
    type("needle");
    press(EC_KEY_ENTER); /* first occurrence -> line 2 */
    ec_cell scr[EC_W * EC_H];
    ec_render(scr);
    press('n');          /* repeat -> line 4 */
    /* verify via a rendered cursor position: cursor '_' must be below row 1 */
    ec_render(scr);
    int cur_row = -1;
    for (int r = 1; r <= EC_TEXT_ROWS; r++)
        for (int x = 0; x < EC_W; x++)
            if (scr[r * EC_W + x].ch == '_') cur_row = r;
    CHECK(cur_row >= 4, "search 'n' jumps to second occurrence");
    press(EC_KEY_ESC);
}

/* helper used above: none yet */
static void run_selftest(void) {
    test_edit_typing();
    test_enter_split();
    test_del_join();
    test_autoindent();
    test_tab();
    test_viewer_scroll();
    test_viewer_search();
    printf(failures ? "== %d FAILURE(S)\n" : "== all tests passed\n", failures);
    exit(failures ? 1 : 0);
}

/* ---------- interactive mode ---------- */

#ifdef __linux__
#include <termios.h>
#include <unistd.h>

static struct termios saved;

static void restore_tty(void) {
    tcsetattr(0, TCSANOW, &saved);
}

static void raw_on(void) {
    struct termios t = saved;
    t.c_lflag &= ~(ICANON | ECHO);
    t.c_cc[VMIN] = 1;
    tcsetattr(0, TCSANOW, &t);
}

static int read_key(void) {  /* returns scancode or -1 EOF */
    char c;
    if (read(0, &c, 1) != 1) return -1;
    if (c != 27) return ec_scancode_for_char(c);
    /* escape sequence */
    char seq[3];
    if (read(0, &seq, 1) != 1) return EC_KEY_ESC;
    if (seq[0] == '[') {
        if (read(0, &seq[1], 1) != 1) return EC_KEY_ESC;
        switch (seq[1]) {
            case 'A': return EC_KEY_UP;
            case 'B': return EC_KEY_DOWN;
            case 'C': return EC_KEY_RIGHT;
            case 'D': return EC_KEY_LEFT;
            case 'H': return EC_KEY_HOME;
            case 'F': return EC_KEY_END;
            case '3': { char t; read(0, &t, 1); return EC_KEY_DEL; }
            case '5': { char t; read(0, &t, 1); return EC_KEY_PGUP; }
            case '6': { char t; read(0, &t, 1); return EC_KEY_PGDN; }
        }
        return EC_KEY_NONE;
    }
    return EC_KEY_ESC;
}

static void render_tty(const ec_cell *scr) {
    printf("\x1b[H");
    for (int r = 0; r < EC_H; r++) {
        for (int x = 0; x < EC_W; x++)
            putchar(scr[r * EC_W + x].ch);
        putchar('\n');
    }
    fflush(stdout);
}

static void run_interactive(void) {
    tcgetattr(0, &saved);
    atexit(restore_tty);
    raw_on();
    char buf[8192];
    size_t n = 0;
    /* load a demo file given on argv or a small default */
    FILE *f = fopen("demo.txt", "r");
    if (f) { n = fread(buf, 1, sizeof(buf) - 1, f); fclose(f); }
    else {
        const char *d = "Demo file for the portable editor.\n"
                        "Try arrows, Home/End, PgUp/PgDn.\n"
                        "Type to edit, Enter for a new line\n"
                        "with auto-indent. ESC quits.\n";
        n = strlen(d);
        memcpy(buf, d, n);
    }
    buf[n] = 0;

    ec_cell scr[EC_W * EC_H];
    printf("\x1b[2J");
    ec_open_edit(buf, n, sizeof(buf), "demo.txt (ESC=quit)");
    ec_render(scr);
    render_tty(scr);
    for (;;) {
        int sc = read_key();
        if (sc < 0) break;
        if (sc == EC_KEY_NONE) continue;
        ec_key(sc);
        if (ec_done()) break;
        ec_render(scr);
        render_tty(scr);
    }
    printf("\x1b[2J\x1b[H[content kept in memory only — demo]\n");
}
#endif

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "interactive")) {
#ifdef __linux__
        run_interactive();
        return 0;
#else
        fprintf(stderr, "interactive mode needs Linux\n");
        return 1;
#endif
    }
    run_selftest();
}
