/* Portable editor/viewer core for the mini commander.
 * Features (inspired by _import/mini-commander view_edit.c):
 *   viewer: PgUp/PgDn/Home/End, search '/' + repeat 'n', percent display
 *   editor: everything above minus search, plus typing, Enter with
 *           auto-indent, Tab, Backspace line merge, Del line join.
 * Lines are ptr+len slices of the caller's buffer, so the buffer can be
 * written back unchanged. Rendering goes to an 80x25 cell framebuffer;
 * redraw happens only after key events. */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "editor_core.h"

#define COLOR(fg,bg) ((unsigned char)((fg) | ((bg) << 4)))
#define A_TITLE  COLOR(0xF,0x1)
#define A_TEXT   COLOR(0xF,0x0)
#define A_STATUS COLOR(0x0,0x3)
#define A_CURSOR COLOR(0xE,0x0)

#define MAX_LINES 256
#define TAB_STOP 4
#define SCR_TEXT_W (EC_W - 2)

static char        *line_ptr[MAX_LINES];
static int          line_len[MAX_LINES];
static int          line_count;
static int          cx, cy;      /* cursor in content, not screen */
static int          top;         /* first visible line */
static char        *ebuf;        /* edit buffer (writable) */
static const char  *cur;         /* active buffer */
static size_t       buf_used, buf_cap;
static int          readonly;
static int          done_flag;
static char         title_buf[64];

/* search state (viewer) */
static char  search_buf[48];
static int   search_len;
static int   search_prompt;      /* prompt input in progress */
static int   have_search;

/* ---------- scancode -> character (US layout) ---------- */

static char sc_char(int sc) {
    static const signed char table[128] = {
        [0x02]='1',[0x03]='2',[0x04]='3',[0x05]='4',[0x06]='5',[0x07]='6',
        [0x08]='7',[0x09]='8',[0x0A]='9',[0x0B]='0',
        [0x0C]='-',[0x0D]='=',
        [0x10]='q',[0x11]='w',[0x12]='e',[0x13]='r',[0x14]='t',[0x15]='y',
        [0x16]='u',[0x17]='i',[0x18]='o',[0x19]='p',
        [0x1A]='[',[0x1B]=']',
        [0x1E]='a',[0x1F]='s',[0x20]='d',[0x21]='f',[0x22]='g',[0x23]='h',
        [0x24]='j',[0x25]='k',[0x26]='l',
        [0x27]=';',[0x28]='\'',[0x29]='`',
        [0x2B]='\\',
        [0x2C]='z',[0x2D]='x',[0x2E]='c',[0x2F]='v',[0x30]='b',[0x31]='n',
        [0x32]='m',
        [0x33]=',',[0x34]='.',[0x35]='/',[0x39]=' '
    };
    if (sc < 0 || sc > 127) return 0;
    if (sc & 0x80) return 0;   /* key release */
    return table[sc];
}

int ec_scancode_for_char(char c) {
    if (c >= 'A' && c <= 'Z') c = (char)(c + 32); /* no shift tracking */
    for (int sc = 0; sc < 128; sc++)
        if (sc_char(sc) == c && sc_char(sc) != 0) return sc;
    return 0;
}

/* ---------- buffer / lines ---------- */

static void rebuild(void) {
    line_count = 0;
    size_t pos = 0;
    while (pos < buf_used && line_count < MAX_LINES) {
        size_t l = 0;
        while (pos + l < buf_used && cur[pos + l] != '\n') l++;
        line_ptr[line_count] = (char*)(cur + pos);
        line_len[line_count] = (int)l;
        line_count++;
        pos += l + 1;
    }
    if (line_count == 0) {
        line_ptr[0] = (char*)cur;
        line_len[0] = 0;
        line_count = 1;
    } else if (cur[buf_used - 1] == '\n' && line_count < MAX_LINES) {
        /* trailing newline: keep the final empty line */
        line_ptr[line_count] = (char*)(cur + buf_used);
        line_len[line_count] = 0;
        line_count++;
    }
    if (cy >= line_count) cy = line_count - 1;
    if (cy < 0) cy = 0;
    if (top > line_count - 1) top = line_count - 1 > 0 ? line_count - 1 : 0;
    if (top < 0) top = 0;
}

static void clamp_cx(void) {
    if (cx > line_len[cy]) cx = line_len[cy];
    if (cx < 0) cx = 0;
}

static void scroll_fix(void) {
    if (cy < top) top = cy;
    if (cy >= top + EC_TEXT_ROWS) top = cy - EC_TEXT_ROWS + 1;
    if (top < 0) top = 0;
}

static void insert_byte(uint32_t pos, char c) {
    if (buf_used >= buf_cap) return;
    memmove(ebuf + pos + 1, ebuf + pos, buf_used - pos);
    ebuf[pos] = c;
    buf_used++;
}

static void delete_byte(uint32_t pos) {
    if (pos >= buf_used) return;
    memmove(ebuf + pos, ebuf + pos + 1, buf_used - pos - 1);
    buf_used--;
}

/* ---------- search ---------- */

static void do_search(void) {
    if (!have_search || search_len == 0) return;
    int qlen = search_len;
    int start_line = cy, start_col = cx + 1;
    for (int pass = 0; pass < 2; pass++) {
        int from_line = (pass == 0) ? start_line : 0;
        for (int i = from_line; i < line_count; i++) {
            int from = (pass == 0 && i == start_line) ? start_col : 0;
            const char *line = line_ptr[i];
            for (int j = from; j + qlen <= line_len[i]; j++) {
                if (memcmp(line + j, search_buf, (size_t)qlen) == 0) {
                    cy = i; cx = j;
                    scroll_fix();
                    return;
                }
            }
            if (pass == 1 && from_line == 0 && i == start_line)
                break;  /* searched everything */
        }
    }
    /* not found: stay */
}

/* ---------- key handling ---------- */

static void key_edit(int sc) {
    uint32_t pos = (uint32_t)(line_ptr[cy] - ebuf) + (uint32_t)cx;
    if (sc == EC_KEY_ESC || sc == EC_KEY_F10) { done_flag = 1; return; }
    if (sc == EC_KEY_LEFT && cx > 0) cx--;
    else if (sc == EC_KEY_RIGHT) { if (cx < line_len[cy]) cx++; }
    else if (sc == EC_KEY_UP && cy > 0) cy--;
    else if (sc == EC_KEY_DOWN && cy < line_count - 1) cy++;
    else if (sc == EC_KEY_HOME) cx = 0;
    else if (sc == EC_KEY_END) cx = line_len[cy];
    else if (sc == EC_KEY_PGUP) { cy -= EC_TEXT_ROWS - 1; if (cy < 0) cy = 0; }
    else if (sc == EC_KEY_PGDN) {
        cy += EC_TEXT_ROWS - 1;
        if (cy > line_count - 1) cy = line_count - 1;
    } else if (sc == EC_KEY_BACK) {
        if (cx > 0) {
            delete_byte(pos - 1);
            cx--;
            rebuild();
        } else if (cy > 0) {
            delete_byte(pos - 1);   /* the '\n' before this line */
            cy--;
            rebuild();
            cx = line_len[cy];
        }
    } else if (sc == EC_KEY_DEL) {
        if (cx < line_len[cy]) {
            delete_byte(pos);
            rebuild();
        } else if (cy < line_count - 1) {
            delete_byte(pos);       /* the '\n' after this line */
            rebuild();
        }
    } else if (sc == EC_KEY_ENTER) {
        insert_byte(pos, '\n');
        /* auto-indent: copy the leading spaces of the current line */
        int indent = 0;
        while (indent < cx && line_ptr[cy][indent] == ' ') indent++;
        for (int i = 0; i < indent; i++) insert_byte(pos + 1 + i, ' ');
        rebuild();
        cy++;
        cx = indent;
    } else if (sc == EC_KEY_TAB) {
        insert_byte(pos, '\t');
        rebuild();
        cx++;
    } else {
        char c = sc_char(sc);
        if (c >= 32 && c <= 126) {
            insert_byte(pos, c);
            rebuild();
            cx++;
        }
    }
}

static void key_view(int sc) {
    if (sc == EC_KEY_ESC || sc == EC_KEY_F10 || sc == EC_KEY_F3) {
        done_flag = 1;
        return;
    }
    if (search_prompt) {
        if (sc == EC_KEY_ENTER) {
            search_prompt = 0;
            have_search = 1;
            cx = 0; cy = 0;  /* search from the top */
            do_search();
        } else if (sc == EC_KEY_ESC) {
            search_prompt = 0;
        } else if (sc == EC_KEY_BACK) {
            if (search_len > 0) search_len--;
        } else {
            char c = sc_char(sc);
            if (c >= 32 && c <= 126 && search_len < (int)sizeof(search_buf) - 1) {
                search_buf[search_len++] = c;
            }
        }
        return;
    }
    if (sc == '/') {  /* start search */
        search_len = 0;
        search_prompt = 1;
        return;
    }
    if (sc == 'n') {  /* repeat search */
        do_search();
        return;
    }
    if (sc == EC_KEY_UP && cy > 0) cy--;
    else if (sc == EC_KEY_DOWN && cy < line_count - 1) cy++;
    else if (sc == EC_KEY_PGUP) { cy -= EC_TEXT_ROWS - 1; if (cy < 0) cy = 0; }
    else if (sc == EC_KEY_PGDN) {
        cy += EC_TEXT_ROWS - 1;
        if (cy > line_count - 1) cy = line_count - 1;
    } else if (sc == EC_KEY_HOME) { cy = 0; cx = 0; }
    else if (sc == EC_KEY_END) { cy = line_count - 1; cx = 0; }
    else if (sc == EC_KEY_LEFT && cx > 0) cx--;
    else if (sc == EC_KEY_RIGHT) { if (cx < line_len[cy]) cx++; }
}

void ec_key(int sc) {
    if (done_flag || sc == EC_KEY_NONE) return;
    if (readonly) key_view(sc);
    else key_edit(sc);
    if (cy > line_count - 1) cy = line_count - 1;
    if (cy < 0) cy = 0;
    clamp_cx();
    scroll_fix();
}

/* ---------- rendering ---------- */

static void draw_text_row(ec_cell *scr, int row, int li) {
    int y = row + 1;
    int col = 0;
    const char *s = line_ptr[li];
    int len = line_len[li];
    for (int i = 0; i < len && col < SCR_TEXT_W; i++) {
        if (s[i] == '\t') {
            do { scr[y * EC_W + 1 + col].ch = ' '; } while (0);
            col = (col / TAB_STOP + 1) * TAB_STOP;
        } else {
            scr[y * EC_W + 1 + col].ch = (unsigned char)s[i];
            scr[y * EC_W + 1 + col].attr = A_TEXT;
            col++;
        }
    }
}

void ec_render(ec_cell *scr) {
    for (int i = 0; i < EC_W * EC_H; i++) {
        scr[i].ch = ' ';
        scr[i].attr = COLOR(0x7,0x0);
    }

    /* title row */
    for (int x = 0; x < EC_W; x++) scr[x].attr = A_TITLE;
    {
        char t[80];
        int i = 0;
        const char *m = readonly ? "[view] " : "[edit] ";
        while (*m) t[i++] = *m++;
        const char *s = title_buf;
        while (*s && i < 78) t[i++] = *s++;
        t[i] = 0;
        for (int k = 0; k < i && k < EC_W; k++) {
            scr[k].ch = (unsigned char)t[k];
        }
    }

    /* text rows */
    for (int r = 0; r < EC_TEXT_ROWS; r++) {
        int li = top + r;
        for (int x = 0; x < EC_W; x++) {
            scr[(r + 1) * EC_W + x].ch = ' ';
            scr[(r + 1) * EC_W + x].attr = A_TEXT;
        }
        if (li < line_count) draw_text_row(scr, r, li);
    }

    /* status row */
    {
        char st[96];
        int i = 0;
        const char *m;
        if (search_prompt) {
            m = "Search: ";
            while (*m && i < 90) st[i++] = *m++;
            for (int k = 0; k < search_len && i < 90; k++) st[i++] = search_buf[k];
        } else {
            m = readonly ? "[view] " : "[edit] ";
            while (*m && i < 90) st[i++] = *m++;
            const char *s = title_buf;
            while (*s && i < 90) st[i++] = *s++;
        }
        st[i] = 0;
        for (int x = 0; x < EC_W; x++) {
            scr[EC_STATUS_ROW * EC_W + x].ch = ' ';
            scr[EC_STATUS_ROW * EC_W + x].attr = A_STATUS;
        }
        for (int k = 0; k < i && k < EC_W; k++) {
            scr[EC_STATUS_ROW * EC_W + k].ch = (unsigned char)st[k];
        }
        if (!search_prompt) {
            /* right-aligned "line X/Y  NN%" */
            char tail[32];
            int ti = 0;
            char nb[8];
            int v = cy + 1, j = 0;
            if (!v) nb[j++] = '0';
            while (v) { nb[j++] = (char)('0' + v % 10); v /= 10; }
            while (j) tail[ti++] = nb[--j];
            tail[ti++] = '/';
            v = line_count; j = 0;
            if (!v) nb[j++] = '0';
            while (v) { nb[j++] = (char)('0' + v % 10); v /= 10; }
            while (j) tail[ti++] = nb[--j];
            tail[ti++] = ' ';
            v = (line_count > 0) ? ((cy + 1) * 100) / line_count : 0; j = 0;
            if (!v) nb[j++] = '0';
            while (v) { nb[j++] = (char)('0' + v % 10); v /= 10; }
            while (j) tail[ti++] = nb[--j];
            tail[ti++] = '%';
            if (have_search && readonly) {
                const char *h = "  n=next";
                while (*h) tail[ti++] = *h++;
            }
            tail[ti] = 0;
            int start = EC_W - ti - 1;
            for (int k = 0; k < ti; k++) {
                scr[EC_STATUS_ROW * EC_W + start + k].ch = (unsigned char)tail[k];
            }
        }
    }

    /* cursor */
    if (cy >= top && cy < top + EC_TEXT_ROWS) {
        int dcol = 0;
        for (int i = 0; i < cx && i < line_len[cy]; i++) {
            dcol = (line_ptr[cy][i] == '\t')
                       ? (dcol / TAB_STOP + 1) * TAB_STOP
                       : dcol + 1;
        }
        if (dcol < SCR_TEXT_W)
            scr[(1 + cy - top) * EC_W + 1 + dcol].ch = '_';
    }
}

/* ---------- session management ---------- */

static void common_init(const char *title) {
    strncpy(title_buf, title ? title : "", sizeof(title_buf) - 1);
    title_buf[sizeof(title_buf) - 1] = 0;
    cx = cy = top = 0;
    done_flag = 0;
    search_prompt = 0;
    have_search = 0;
    search_len = 0;
    search_buf[0] = 0;
}

void ec_open_edit(char *buf, size_t len, size_t cap, const char *title) {
    common_init(title);
    readonly = 0;
    ebuf = buf;
    cur = buf;
    buf_used = len;
    buf_cap = cap;
    rebuild();
}

void ec_open_view(const char *buf, size_t len, const char *title) {
    common_init(title);
    readonly = 1;
    ebuf = 0;
    cur = buf;
    buf_used = len;
    buf_cap = 0;
    rebuild();
}

int ec_done(void) { return done_flag; }

size_t ec_result_len(void) { return buf_used; }
