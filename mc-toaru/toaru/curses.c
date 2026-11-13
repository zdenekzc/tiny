/* curses.c - windows, coloring and key reading for ToaruOS (replaces ncursesw)
 *
 * We take the screen update model from ncurses, because miniCommander depends
 * on it: each window draws into its own grid, wnoutrefresh() copies it into
 * newscr and doupdate() compares newscr with curscr (the state the terminal
 * has) and sends only the changes.  Through `dupwin(newscr)` + `overwrite()`
 * the application saves and restores a screen snapshot itself (dialog shadows),
 * which is why newscr MUST be a separate window, not an alias for stdscr.
 *
 * Output: only sequences handled by the emulator in ToaruOS 2.3.2
 * (src/lib/termemu.c): ESC[y;xH, ESC[K, ESC[J, ESC[m with 8 colors + bold as
 * the bright variant, ESC[?25h/l, ESC[?1049h/l, mouse ESC[?1000/1002/1006.
 * No 256 colors, italics or scroll regions - those would end up as ordinary
 * text on the ToaruOS screen.
 *
 * Input: the kernel on a tty CANNOT return -EAGAIN (read(2) returns 0 =
 * "nothing yet"), so poll() is called before every read.  Input on the serial
 * line and in the terminal arrives one key at a time (raw mode), verified by
 * probe/ttyprobe.c.
 */
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/termios.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "ncurses.h"

int mknod(const char *pathname, int mode, int dev) { (void)pathname; (void)mode; (void)dev; return -1; }
int mkfifo(const char *pathname, int mode) { (void)pathname; (void)mode; return -1; }
int getpwuid_r(uid_t uid, struct passwd *pwd, char *buf, size_t buflen, struct passwd **result) { (void)uid; (void)pwd; (void)buf; (void)buflen; *result = NULL; return -1; }

WINDOW *stdscr, *newscr, *curscr;
int LINES = 24;
int COLS = 80;
int ESCDELAY = 100; /* ms ncurses leaves for the rest of a sequence's bytes */
int color_enabled_shim = 1;

/* ---------------------------------------------------------- screen state */
static int started;      /* buffers allocated */
static int active;       /* terminal is in curses mode */
static int alt_screen;   /* ESC[?1049 - return to the original screen on endwin */
static int out_fd = 1;
static int in_fd = 0;
static int visibility = 1;
static int phys_y = -1, phys_x = -1;  /* cursor position as the terminal has it */
static unsigned int last_attr = 0xDEAD;
static int dirty;                     /* something new is in newscr */
static struct termios saved_tio;
static int saved_tio_ok;

#define OBUFSZ 8192
#define CMD_MAX_SHIM 1024
static char obuf[OBUFSZ];
static size_t olen;

/* color pairs: init_pair() stores them, SGR is built when drawing */
static short pair_fg[COLOR_PAIRS];
static short pair_bg[COLOR_PAIRS];
static short pair_used[COLOR_PAIRS];

/* buffered input: the buffer fills from in_fd, decoded keys wait in the queue */
#define IBUFSZ 512
static unsigned char ibuf[IBUFSZ];
static int ilen, ipos;
static int unget[8];
static int nunget;

/* mouse event queue - getmouse() is called even when there is no mouse */
#define MOUSEQ 32
static MEVENT mouseq[MOUSEQ];
static int mouse_head, mouse_tail;
static mmask_t mouse_events;
static int mouse_interval = 100;
static int mouse_press_btn = -1, mouse_press_x, mouse_press_y;
static long mouse_press_ms;
static int mouse_clicks;
static long mouse_last_click_ms;

/* user-defined sequences (define_key) - Alt+letter and F5..F7 */
#define USERKEYS 64
static struct { const char *seq; int code; char *heap; } userkey[USERKEYS];
static int nuserkey;

static long now_ms(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_REALTIME, &ts))
		return 0;
	return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ------------------------------------------------------------- output */
static void out_flush(void)
{
	size_t off = 0;
	while (off < olen) {
		ssize_t n = write(out_fd, obuf + off, olen - off);
		if (n <= 0) {
			if (n < 0 && errno == EINTR)
				continue;
			break;
		}
		off += (size_t)n;
	}
	olen = 0;
}

static void out_put(const char *s, size_t n)
{
	while (n) {
		size_t free_space = OBUFSZ - olen;
		if (!free_space) {
			out_flush();
			free_space = OBUFSZ;
		}
		if (n < free_space)
			free_space = n;
		memcpy(obuf + olen, s, free_space);
		olen += free_space;
		s += free_space;
		n -= free_space;
	}
	if (olen > OBUFSZ - 64)
		out_flush();
}

static void out_str(const char *s)
{
	out_put(s, strlen(s));
}

static void out_fmt(const char *fmt, ...)
{
	char tmp[64];
	va_list ap;
	int n;
	va_start(ap, fmt);
	n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	if (n > 0)
		out_put(tmp, (size_t)n);
}

/* code point => UTF-8; ToaruOS decodes UTF-8 in the tty layer (kernel/vfs/tty.c) */
static int out_utf8(unsigned int c)
{
	char b[4];
	int n = 0;
	if (c < 0x80) {
		b[n++] = (char)c;
	} else if (c < 0x800) {
		b[n++] = (char)(0xC0 | (c >> 6));
		b[n++] = (char)(0x80 | (c & 0x3F));
	} else if (c < 0x10000) {
		b[n++] = (char)(0xE0 | (c >> 12));
		b[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
		b[n++] = (char)(0x80 | (c & 0x3F));
	} else {
		b[n++] = (char)(0xF0 | (c >> 18));
		b[n++] = (char)(0x80 | ((c >> 12) & 0x3F));
		b[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
		b[n++] = (char)(0x80 | (c & 0x3F));
	}
	out_put(b, (size_t)n);
	return 1;
}

static void move_to(int y, int x)
{
	if (phys_y == y && phys_x == x)
		return;
	out_fmt("\033[%d;%dH", y + 1, x + 1);
	phys_y = y;
	phys_x = x;
}

/* SGR: 0 always at the start - the 2.3.2 emulator keeps colors between
 * commands, and if not enough of them are sent, colors stay wrong on screen */
static void apply_attr(unsigned int attr)
{
	char seq[48];
	int n = 0;
	int fg, bg;

	if (attr == last_attr)
		return;

	n += snprintf(seq + n, sizeof(seq) - (size_t)n, "\033[0");
	if (attr & A_BOLD)
		n += snprintf(seq + n, sizeof(seq) - (size_t)n, ";1");
	if (attr & A_DIM)
		n += snprintf(seq + n, sizeof(seq) - (size_t)n, ";2");
	if (attr & A_UNDERLINE)
		n += snprintf(seq + n, sizeof(seq) - (size_t)n, ";4");
	if (attr & A_BLINK)
		n += snprintf(seq + n, sizeof(seq) - (size_t)n, ";5");
	if (attr & A_REVERSE)
		n += snprintf(seq + n, sizeof(seq) - (size_t)n, ";7");
	if (attr & A_INVIS)
		n += snprintf(seq + n, sizeof(seq) - (size_t)n, ";8");

	if (color_enabled_shim) {
		int pair = PAIR_NUMBER(attr);
		if (pair > 0 && pair < COLOR_PAIRS && pair_used[pair]) {
			fg = pair_fg[pair];
			bg = pair_bg[pair];
			if (fg >= 0 && fg < 8)
				n += snprintf(seq + n, sizeof(seq) - (size_t)n, ";%d", 30 + fg);
			else if (fg >= 8 && fg < 16)
				n += snprintf(seq + n, sizeof(seq) - (size_t)n, ";%d", 90 + fg - 8);
			if (bg >= 0 && bg < 8)
				n += snprintf(seq + n, sizeof(seq) - (size_t)n, ";%d", 40 + bg);
			else if (bg >= 8 && bg < 16)
				n += snprintf(seq + n, sizeof(seq) - (size_t)n, ";%d", 100 + bg - 8);
		}
	}
	/* SGR must be terminated with 'm'. Without it the terminal takes the first
	 * character following the sequence as its final byte - the character is not
	 * drawn (and CSI even interprets it as a command, e.g. 'C' = cursor move).
	 * That is exactly what caused the missing first letter of button labels
	 * (Sort -> "ort", Quit -> "uit"). */
	if (n < 0)
		n = 0;
	if ((size_t)n > sizeof(seq) - 2)
		n = (int)(sizeof(seq) - 2);
	seq[n++] = 'm';
	seq[n] = '\0';
	out_str(seq);
	last_attr = attr;
}

/* ------------------------------------------------------------- windows */
static WINDOW *win_new(int nlines, int ncols, int begin_y, int begin_x)
{
	WINDOW *w;
	size_t cells = (size_t)(nlines > 0 ? nlines : 1) * (size_t)(ncols > 0 ? ncols : 1);

	if (nlines <= 0 || ncols <= 0)
		return NULL; /* ncurses accepts that too, but we do not want a zero grid */
	w = calloc(1, sizeof(WINDOW));
	if (!w)
		return NULL;
	w->ch = calloc(cells, sizeof(unsigned int));
	w->attr = calloc(cells, sizeof(unsigned int));
	if (!w->ch || !w->attr) {
		free(w->ch);
		free(w->attr);
		free(w);
		return NULL;
	}
	w->maxy = (short)nlines;
	w->maxx = (short)ncols;
	w->begin_y = (short)begin_y;
	w->begin_x = (short)begin_x;
	w->bgch = ' ';
	return w;
}

static void win_fill(WINDOW *w, unsigned int ch, unsigned int attr)
{
	size_t cells = (size_t)w->maxy * (size_t)w->maxx;
	size_t i;
	for (i = 0; i < cells; i++) {
		w->ch[i] = ch;
		w->attr[i] = attr;
	}
	w->touched = 1;
}

static void win_copy_into(WINDOW *dst, WINDOW *src)
{
	int y, x;
	int y0 = src->begin_y, x0 = src->begin_x;
	if (y0 < 0)
		y0 = 0;
	if (x0 < 0)
		x0 = 0;
	for (y = y0; y < src->maxy; y++) {
		int dy = y - src->begin_y;
		for (x = x0; x < src->maxx; x++) {
			size_t s = (size_t)y * (size_t)src->maxx + (size_t)x;
			int dx = x - src->begin_x;
			size_t d;
			if (y >= dst->maxy || x >= dst->maxx)
				continue;
			d = (size_t)dy * (size_t)dst->maxx + (size_t)dx;
			if (dst->ch[d] != src->ch[s] || dst->attr[d] != src->attr[s]) {
				dst->ch[d] = src->ch[s];
				dst->attr[d] = src->attr[s];
				dst->touched = 1;
			}
		}
	}
	dst->cury = src->cury;
	dst->curx = src->curx;
}

static void win_free(WINDOW *w)
{
	if (!w)
		return;
	free(w->ch);
	free(w->attr);
	free(w);
}

/* cell written to a window: the background color applies only where the
 * application has not set a color itself - just like wbkgd() does in ncurses */
static void cell_put(WINDOW *w, int y, int x, unsigned int ch, unsigned int attr)
{
	size_t i;
	if (y < 0 || y >= w->maxy || x < 0 || x >= w->maxx)
		return;
	i = (size_t)y * (size_t)w->maxx + (size_t)x;
	if (!(attr & A_COLOR))
		attr |= (w->bgattr & A_COLOR);
	attr |= (w->bgattr & A_BOLD) & (attr & A_BOLD ? A_BOLD : 0);
	if (w->ch[i] == ch && w->attr[i] == attr)
		return;
	w->ch[i] = ch;
	w->attr[i] = attr;
	w->touched = 1;
	dirty = 1;
}

static void cell_get(WINDOW *w, int y, int x, unsigned int *ch, unsigned int *attr)
{
	size_t i;
	if (y < 0 || y >= w->maxy || x < 0 || x >= w->maxx) {
		*ch = ' ';
		*attr = 0;
		return;
	}
	i = (size_t)y * (size_t)w->maxx + (size_t)x;
	*ch = w->ch[i];
	*attr = w->attr[i];
}

/* --------------------------------------------------------- initialization */
static void screen_size(void)
{
	struct winsize ws;
	const char *e;

	if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
		LINES = ws.ws_row;
		COLS = ws.ws_col;
		return;
	}
	e = getenv("LINES");
	if (e && *e)
		LINES = atoi(e);
	e = getenv("COLUMNS");
	if (e && *e)
		COLS = atoi(e);
	if (LINES < 2)
		LINES = 24;
	if (COLS < 2)
		COLS = 80;
}

static void term_raw(void)
{
	struct termios t;
	if (!saved_tio_ok) {
		if (tcgetattr(in_fd, &saved_tio) == 0)
			saved_tio_ok = 1;
		else
			return;
	}
	t = saved_tio;
	t.c_iflag &= ~(unsigned)(IXON | IXOFF | IXANY | INLCR | ICRNL | ISTRIP | BRKINT | IGNCR);
	t.c_oflag &= ~(unsigned)OPOST;
	t.c_lflag &= ~(unsigned)(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
	t.c_cflag &= ~(unsigned)(CSIZE | PARENB);
	t.c_cflag |= CS8;
	t.c_cc[VMIN] = 1;
	t.c_cc[VTIME] = 0;
	tcsetattr(in_fd, TCSANOW, &t);
	active = 1;
}

static void term_restore(void)
{
	if (saved_tio_ok)
		tcsetattr(in_fd, TCSANOW, &saved_tio);
	active = 0;
}

static void alt_screen_enable(void);
static void alloc_screens(void)
{
	screen_size();
	stdscr = win_new(LINES, COLS, 0, 0);
	newscr = win_new(LINES, COLS, 0, 0);
	curscr = win_new(LINES, COLS, 0, 0);
	if (!stdscr || !newscr || !curscr)
		return;
	win_fill(stdscr, ' ', 0);
	win_fill(newscr, ' ', 0);
	/* curscr represents what the terminal has on screen - "unknown" = repaint */
	win_fill(curscr, ' ', 0xFFFFFFFFu);
	started = 1;
}
static void alt_screen_enable(void) {
	out_str("\033[?1049h");
}

WINDOW *initscr(void)
{
	if (!started)
		alloc_screens();
	return stdscr;
}

SCREEN *newterm(const char *type, FILE *out, FILE *in)
{
	static SCREEN marker;
	const char *t;

	(void)type;
	if (out)
		out_fd = fileno(out);
	if (in)
		in_fd = fileno(in);
	/* Colorless terminal (TERM=vt100 and similar historical types): pairs are
	 * ignored and bold/reverse remains - ToaruOS has no terminfo, so the type
	 * is taken only as the user's free choice */
	t = getenv("TERM");
	if (t && (!strcmp(t, "vt100") || !strcmp(t, "vt52") || !strcmp(t, "linux-mono")))
		color_enabled_shim = 0;
	initscr();
	if (!started)
		return NULL;
	return &marker;
}

static void paint_all(void)
{
	if (!started)
		return;
	out_str("\033[2J");
	phys_y = phys_x = -1;
	last_attr = 0xDEAD;
}

int endwin(void)
{
	if (!started)
		return OK;
	doupdate();
	out_str("\033[?25h\033[?1000l\033[?1002l\033[?1006l");
	if (alt_screen) {
		out_str("\033[?1049l");
		alt_screen = 0;
	}
	out_flush();
	term_restore();
	phys_y = phys_x = -1;
	last_attr = 0xDEAD;
	return OK;
}

int isendwin(void)
{
	return !active;
}

/* --------------------------------------------------------------- drawing */
int wnoutrefresh(WINDOW *win)
{
	if (!started || !win)
		return ERR;
	if (win == newscr) {
		dirty = 1;
		return OK;
	}
	win_copy_into(newscr, win);
	return OK;
}

int doupdate(void)
{
	int y, x;

	if (!started)
		return ERR;

	for (y = 0; y < newscr->maxy && y < LINES; y++) {
		int wrote = 0;
		for (x = 0; x < newscr->maxx && x < COLS; x++) {
			unsigned int nc, na, oc, oa;
			cell_get(newscr, y, x, &nc, &na);
			cell_get(curscr, y, x, &oc, &oa);
			if (nc == oc && na == oa)
				continue;
			if (!wrote)
				move_to(y, x);
			apply_attr(na);
			out_utf8(nc ? nc : ' ');
			phys_x++;
			wrote = 1;
			cell_get(newscr, y, x, &nc, &na);
			curscr->ch[(size_t)y * (size_t)curscr->maxx + (size_t)x] = nc;
			curscr->attr[(size_t)y * (size_t)curscr->maxx + (size_t)x] = na;
		}
	}

	/* place the terminal cursor at the logical cursor of the windows */
	if (newscr->cury < LINES && newscr->curx < COLS)
		move_to(newscr->cury, newscr->curx);
	out_flush();
	dirty = 0;
	newscr->touched = 0;
	curscr->touched = 0;
	return OK;
}

int refresh(void)
{
	wnoutrefresh(stdscr);
	return doupdate();
}

int wrefresh(WINDOW *win)
{
	wnoutrefresh(win);
	return doupdate();
}

int redrawwin(WINDOW *win)
{
	if (!win)
		return ERR;
	win->touched = 1;
	return OK;
}

int touchwin(WINDOW *win)
{
	return redrawwin(win);
}

int is_wintouched(WINDOW *win)
{
	if (win == newscr || win == curscr)
		return dirty;
	return win ? win->touched : 0;
}

/* --------------------------------------------------------- window operations */
WINDOW *newwin(int nlines, int ncols, int begin_y, int begin_x)
{
	WINDOW *w = win_new(nlines, ncols, begin_y, begin_x);
	if (w)
		win_fill(w, ' ', 0);
	return w;
}

WINDOW *dupwin(WINDOW *win)
{
	WINDOW *w;
	size_t cells;
	if (!win)
		return NULL;
	w = win_new(win->maxy, win->maxx, win->begin_y, win->begin_x);
	if (!w)
		return NULL;
	cells = (size_t)win->maxy * (size_t)win->maxx;
	memcpy(w->ch, win->ch, cells * sizeof(unsigned int));
	memcpy(w->attr, win->attr, cells * sizeof(unsigned int));
	w->cury = win->cury;
	w->curx = win->curx;
	w->bgch = win->bgch;
	w->bgattr = win->bgattr;
	return w;
}

int delwin(WINDOW *win)
{
	if (win == stdscr || win == newscr || win == curscr)
		return OK; /* these live as long as the terminal */
	win_free(win);
	return OK;
}

int wresize(WINDOW *win, int lines, int columns)
{
	WINDOW *w;
	int y;
	if (!win || lines <= 0 || columns <= 0)
		return ERR;
	if (win == stdscr || win == newscr || win == curscr) {
		/* the screen size changes all three at once */
		return ERR;
	}
	w = win_new(lines, columns, win->begin_y, win->begin_x);
	if (!w)
		return ERR;
	win_fill(w, ' ', win->bgattr);
	for (y = 0; y < lines && y < win->maxy; y++) {
		int x;
		for (x = 0; x < columns && x < win->maxx; x++) {
			size_t s = (size_t)y * (size_t)win->maxx + (size_t)x;
			size_t d = (size_t)y * (size_t)columns + (size_t)x;
			w->ch[d] = win->ch[s];
			w->attr[d] = win->attr[s];
		}
	}
	w->bgch = win->bgch;
	w->bgattr = win->bgattr;
	w->cury = win->cury < (short)lines ? win->cury : (short)(lines - 1);
	w->curx = win->curx < (short)columns ? win->curx : (short)(columns - 1);
	win_free(win);
	/* the caller thinks it still has the window - replace contents, not pointer */
	win->maxy = w->maxy;
	win->maxx = w->maxx;
	win->ch = w->ch;
	win->attr = w->attr;
	free(w);
	win->touched = 1;
	return OK;
}

int overwrite(WINDOW *src, WINDOW *dst)
{
	int y, x;
	if (!src || !dst)
		return ERR;
	for (y = 0; y < src->maxy && y < dst->maxy; y++) {
		for (x = 0; x < src->maxx && x < dst->maxx; x++) {
			size_t s = (size_t)y * (size_t)src->maxx + (size_t)x;
			size_t d = (size_t)y * (size_t)dst->maxx + (size_t)x;
			if (src->ch[s] == ' ' && (src->attr[s] & A_COLOR) == 0)
				continue; /* we cannot overwrite with a space differently than ncurses */
			dst->ch[d] = src->ch[s];
			dst->attr[d] = src->attr[s];
		}
	}
	dst->touched = 1;
	dirty = 1;
	return OK;
}

int werase(WINDOW *win)
{
	if (!win)
		return ERR;
	win_fill(win, win->bgch ? win->bgch : ' ', win->bgattr);
	return OK;
}

int erase(void)
{
	return werase(stdscr);
}

int wclrtoeol(WINDOW *win)
{
	int x;
	if (!win)
		return ERR;
	for (x = win->curx; x < win->maxx; x++)
		cell_put(win, win->cury, x, win->bgch ? win->bgch : ' ', win->bgattr);
	return OK;
}

int clrtoeol(void)
{
	return wclrtoeol(stdscr);
}

int wbkgdset(WINDOW *win, chtype bg)
{
	if (!win)
		return ERR;
	win->bgch = (unsigned int)(bg & A_CHARTEXT);
	win->bgattr = (unsigned int)(bg & A_ATTRIBUTES);
	return OK;
}

int wbkgd(WINDOW *win, chtype bg)
{
	if (!win)
		return ERR;
	wbkgdset(win, bg);
	win_fill(win, win->bgch ? win->bgch : ' ', win->bgattr);
	return OK;
}

int wborder(WINDOW *win, chtype ls, chtype rs, chtype ts, chtype bs, chtype tl,
	chtype tr, chtype bl, chtype br)
{
	int x, y;
	if (!win)
		return ERR;
	if (!ls)
		ls = '|';
	if (!rs)
		rs = '|';
	if (!ts)
		ts = '-';
	if (!bs)
		bs = '-';
	if (!tl)
		tl = '+';
	if (!tr)
		tr = '+';
	if (!bl)
		bl = '+';
	if (!br)
		br = '+';
	for (x = 1; x + 1 < win->maxx; x++) {
		cell_put(win, 0, x, (unsigned int)ts, win->bgattr);
		cell_put(win, win->maxy - 1, x, (unsigned int)bs, win->bgattr);
	}
	for (y = 1; y + 1 < win->maxy; y++) {
		cell_put(win, y, 0, (unsigned int)ls, win->bgattr);
		cell_put(win, y, win->maxx - 1, (unsigned int)rs, win->bgattr);
	}
	cell_put(win, 0, 0, (unsigned int)tl, win->bgattr);
	cell_put(win, 0, win->maxx - 1, (unsigned int)tr, win->bgattr);
	cell_put(win, win->maxy - 1, 0, (unsigned int)bl, win->bgattr);
	cell_put(win, win->maxy - 1, win->maxx - 1, (unsigned int)br, win->bgattr);
	return OK;
}

/* ------------------------------------------------------ movement and write */
int wmove(WINDOW *win, int y, int x)
{
	if (!win)
		return ERR;
	if (y < 0 || y >= win->maxy || x < 0 || x >= win->maxx)
		return ERR;
	win->cury = (short)y;
	win->curx = (short)x;
	return OK;
}

int move(int y, int x)
{
	return wmove(stdscr, y, x);
}

int waddch(WINDOW *win, chtype ch)
{
	if (!win)
		return ERR;
	cell_put(win, win->cury, win->curx, (unsigned int)(ch & 0x10FFFF),
		(unsigned int)(ch & A_ATTRIBUTES));
	win->curx++;
	if (win->curx >= win->maxx)
		win->curx = (short)(win->maxx - 1);
	return OK;
}

int addch(chtype ch)
{
	return waddch(stdscr, ch);
}

int mvaddch(int y, int x, chtype ch)
{
	if (wmove(stdscr, y, x) == ERR)
		return ERR;
	return waddch(stdscr, ch);
}

int mvwaddch(WINDOW *win, int y, int x, chtype ch)
{
	if (wmove(win, y, x) == ERR)
		return ERR;
	return waddch(win, ch);
}

int winsch(WINDOW *win, chtype ch)
{
	return waddch(win, ch);
}

int mvwinsch(WINDOW *win, int y, int x, chtype ch)
{
	return mvwaddch(win, y, x, ch);
}

chtype winch(WINDOW *win)
{
	unsigned int ch, attr;
	cell_get(win, win->cury, win->curx, &ch, &attr);
	return (chtype)((ch & A_CHARTEXT) | (attr & A_ATTRIBUTES));
}

chtype mvwinch(WINDOW *win, int y, int x)
{
	unsigned int ch, attr;
	chtype r;
	if (!win)
		return (chtype)' ';
	cell_get(win, y, x, &ch, &attr);
	r = (chtype)((ch & A_CHARTEXT) | (attr & A_ATTRIBUTES));
	wmove(win, y, x);
	return r;
}

/* UTF-8 string into a window; we count columns using wcwidth so that the
 * panel length calculations and the cursor position after output match
 * (getcurx() is used in mc to measure text width) */
int waddstr(WINDOW *win, const char *s)
{
	if (!win || !s)
		return ERR;
	while (*s) {
		unsigned int wc = (unsigned char)*s;
		int n = 1;
		if (wc >= 0x80) {
			unsigned char c0 = (unsigned char)s[0];
			unsigned char c1 = (unsigned char)s[1];
			if ((c0 & 0xE0) == 0xC0 && (c1 & 0xC0) == 0x80) {
				wc = ((c0 & 0x1F) << 6) | (c1 & 0x3F);
				n = 2;
			} else {
				unsigned char c2 = (unsigned char)s[2];
				if ((c0 & 0xF0) == 0xE0 && (c1 & 0xC0) == 0x80 && (c2 & 0xC0) == 0x80) {
					wc = ((c0 & 0x0F) << 12) | ((c1 & 0x3F) << 6) | (c2 & 0x3F);
					n = 3;
				} else {
					unsigned char c3 = (unsigned char)s[3];
					if ((c0 & 0xF8) == 0xF0 && (c1 & 0xC0) == 0x80 && (c2 & 0xC0) == 0x80 && (c3 & 0xC0) == 0x80) {
						wc = ((c0 & 0x07) << 18) | ((c1 & 0x3F) << 12) | ((c2 & 0x3F) << 6) | (c3 & 0x3F);
						n = 4;
					}
				}
			}
		}
		s += n;
		int w = wcwidth((wchar_t)wc);
		if (w < 1)
			w = 1;
		cell_put(win, win->cury, win->curx, wc, 0);
		win->curx = (short)(win->curx + w);
		if (win->curx >= win->maxx) {
			win->curx = (short)(win->maxx - 1);
			break;
		}
	}
	return OK;
}

int addstr(const char *s)
{
	return waddstr(stdscr, s);
}

int mvaddstr(int y, int x, const char *s)
{
	if (wmove(stdscr, y, x) == ERR)
		return ERR;
	return waddstr(stdscr, s);
}

int mvwaddstr(WINDOW *win, int y, int x, const char *s)
{
	if (wmove(win, y, x) == ERR)
		return ERR;
	return waddstr(win, s);
}

int waddnstr(WINDOW *win, const char *s, int n)
{
	char tmp[CMD_MAX_SHIM];
	if (!s)
		return ERR;
	if (n < 0 || (size_t)n >= sizeof(tmp))
		return waddstr(win, s);
	memcpy(tmp, s, (size_t)n);
	tmp[n] = 0;
	return waddstr(win, tmp);
}

int mvaddnstr(int y, int x, const char *s, int n)
{
	if (wmove(stdscr, y, x) == ERR)
		return ERR;
	return waddnstr(stdscr, s, n);
}

int mvwaddnstr(WINDOW *win, int y, int x, const char *s, int n)
{
	if (wmove(win, y, x) == ERR)
		return ERR;
	return waddnstr(win, s, n);
}

int whline(WINDOW *win, chtype ch, int n)
{
	int i;
	for (i = 0; i < n && win->curx < win->maxx; i++)
		waddch(win, ch);
	return OK;
}

int mvwhline(WINDOW *win, int y, int x, chtype ch, int n)
{
	if (wmove(win, y, x) == ERR)
		return ERR;
	return whline(win, ch, n);
}

int mvhline(int y, int x, chtype ch, int n)
{
	return mvwhline(stdscr, y, x, ch, n);
}

int wvline(WINDOW *win, chtype ch, int n)
{
	int i;
	for (i = 0; i < n; i++) {
		waddch(win, ch);
		if (win->cury + 1 >= win->maxy)
			break;
		win->cury = (short)(win->cury + 1);
	}
	return OK;
}

int mvwvline(WINDOW *win, int y, int x, chtype ch, int n)
{
	if (wmove(win, y, x) == ERR)
		return ERR;
	return wvline(win, ch, n);
}

int mvvline(int y, int x, chtype ch, int n)
{
	return mvwvline(stdscr, y, x, ch, n);
}

int wprintw(WINDOW *win, const char *fmt, ...)
{
	char buf[CMD_MAX_SHIM];
	va_list ap;
	int n;
	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n < 0)
		return ERR;
	return waddstr(win, buf);
}

int mvwprintw(WINDOW *win, int y, int x, const char *fmt, ...)
{
	char buf[CMD_MAX_SHIM];
	va_list ap;
	int n;
	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n < 0)
		return ERR;
	if (wmove(win, y, x) == ERR)
		return ERR;
	return waddstr(win, buf);
}

int printw(const char *fmt, ...)
{
	char buf[CMD_MAX_SHIM];
	va_list ap;
	int n;
	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n < 0)
		return ERR;
	return waddstr(stdscr, buf);
}

int mvprintw(int y, int x, const char *fmt, ...)
{
	char buf[CMD_MAX_SHIM];
	va_list ap;
	int n;
	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n < 0)
		return ERR;
	if (wmove(stdscr, y, x) == ERR)
		return ERR;
	return waddstr(stdscr, buf);
}

int mvcur(int oldrow, int oldcol, int newrow, int newcol)
{
	(void)oldrow;
	(void)oldcol;
	move_to(newrow, newcol);
	out_flush();
	return OK;
}

/* ----------------------------------------------------- wide characters */
int setcchar(cchar_t *wcval, const wchar_t *wch, const attr_t attrs,
	short color_pair, void *opts)
{
	int i;
	(void)opts;
	if (!wcval || !wch)
		return ERR;
	memset(wcval, 0, sizeof(*wcval));
	for (i = 0; i < CCHARW_MAX && wch[i]; i++)
		wcval->wc[i] = wch[i];
	wcval->attr = attrs & ~A_COLOR;
	wcval->color = color_pair;
	return OK;
}

int getcchar(const cchar_t *wcval, wchar_t *wch, attr_t *attrs,
	short *color_pair, void *opts)
{
	int i;
	(void)opts;
	if (!wcval)
		return ERR;
	if (wch)
		for (i = 0; i < CCHARW_MAX; i++)
			wch[i] = wcval->wc[i];
	if (attrs)
		*attrs = wcval->attr;
	if (color_pair)
		*color_pair = wcval->color;
	return OK;
}

int wadd_wch(WINDOW *win, const cchar_t *wch)
{
	if (!win || !wch)
		return ERR;
	cell_put(win, win->cury, win->curx, (unsigned int)wch->wc[0],
		(unsigned int)(wch->attr | COLOR_PAIR(wch->color)));
	win->curx++;
	if (win->curx >= win->maxx)
		win->curx = (short)(win->maxx - 1);
	return OK;
}

int mvwadd_wch(WINDOW *win, int y, int x, const cchar_t *wch)
{
	if (wmove(win, y, x) == ERR)
		return ERR;
	return wadd_wch(win, wch);
}

int mvwadd_wchnstr(WINDOW *win, int y, int x, const cchar_t *wch, int n)
{
	int i;
	if (wmove(win, y, x) == ERR)
		return ERR;
	if (n < 0)
		n = 1;
	for (i = 0; i < n && wch[i].wc[0]; i++)
		wadd_wch(win, &wch[i]);
	return OK;
}

/* ------------------------------------------------------------- attributes */
int wattron(WINDOW *win, attr_t a)
{
	if (!win)
		return ERR;
	win->bgattr = win->bgattr; /* attributes are composed for individual cells below */
	(void)a;
	return OK;
}

/* Simple model as in PDCurses: the "current attribute" is part of the window
 * background until the caller turns it off.  We keep it in a separate field so
 * that wbkgd() stays preserved. */
static attr_t curattr(WINDOW *w)
{
	return w->bgattr & ~A_COLOR;
}

int wattron_real(WINDOW *win, attr_t a);

int attron(attr_t a)
{
	return wattron_real(stdscr, a);
}

int wattroff(WINDOW *win, attr_t a)
{
	if (!win)
		return ERR;
	win->bgattr &= ~(unsigned int)a;
	return OK;
}

int attroff(attr_t a)
{
	return wattroff(stdscr, a);
}

int wattron_real(WINDOW *win, attr_t a)
{
	if (!win)
		return ERR;
	win->bgattr |= (unsigned int)a;
	return OK;
}

int attr_off(attr_t a, void *opts)
{
	(void)opts;
	return attroff(a);
}

int wattr_off(WINDOW *win, attr_t a, void *opts)
{
	(void)opts;
	return wattroff(win, a);
}

int attr_on(attr_t a, void *opts)
{
	(void)opts;
	return attron(a);
}

int wattr_on(WINDOW *win, attr_t a, void *opts)
{
	(void)opts;
	return wattron_real(win, a);
}

int attrset(attr_t a)
{
	return wattr_set(stdscr, a, PAIR_NUMBER((chtype)a), NULL);
}

int wattrset(WINDOW *win, attr_t a)
{
	return wattr_set(win, a, PAIR_NUMBER((chtype)a), NULL);
}

int wattr_set(WINDOW *win, attr_t attrs, short pair, void *opts)
{
	(void)opts;
	if (!win)
		return ERR;
	win->bgattr = ((unsigned int)attrs & ~A_COLOR) | (unsigned int)COLOR_PAIR(pair);
	return OK;
}

int wattr_get(WINDOW *win, attr_t *attrs, short *pair, void *opts)
{
	(void)opts;
	if (!win)
		return ERR;
	if (attrs)
		*attrs = curattr(win);
	if (pair)
		*pair = (short)PAIR_NUMBER(win->bgattr);
	return OK;
}

void getattrs(WINDOW *win, attr_t *attrs)
{
	if (attrs)
		*attrs = win ? curattr(win) : 0;
}

/* ------------------------------------------------------------- colors */
int start_color(void)
{
	int i;
	for (i = 0; i < COLOR_PAIRS; i++) {
		pair_fg[i] = COLOR_WHITE;
		pair_bg[i] = COLOR_BLACK;
		pair_used[i] = 0;
	}
	return OK;
}

int init_pair(short pair, short f, short b)
{
	if (pair < 0 || pair >= COLOR_PAIRS)
		return ERR;
	pair_fg[pair] = f;
	pair_bg[pair] = b;
	pair_used[pair] = 1;
	return OK;
}

int init_color(short color, short r, short g, short b)
{
	(void)color;
	(void)r;
	(void)g;
	(void)b;
	return ERR; /* ToaruOS has no user-defined palette */
}

int has_colors(void)
{
	return color_enabled_shim;
}

int can_change_color(void)
{
	return FALSE;
}

int use_default_colors(void)
{
	return OK;
}

/* ---------------------------------------------------------------- modes */
int curs_set(int visibility_new)
{
	int old = visibility;
	if (visibility_new < 0 || visibility_new > 2)
		return ERR;
	if (!started)
		return old;
	if (visibility_new == 0)
		out_str("\033[?25l");
	else
		out_str("\033[?25h");
	visibility = visibility_new;
	out_flush();
	return old;
}

int raw(void)
{
	term_raw();
	return OK;
}

int cbreak(void)
{
	return raw();
}

int nocbreak(void)
{
	return OK;
}

int noecho(void)
{
	term_raw();
	return OK;
}

int echo(void)
{
	return OK;
}

int nl(void)
{
	return OK;
}

int nonl(void)
{
	return OK;
}

int keypad(WINDOW *win, int bf)
{
	if (win)
		win->keypad = bf ? 1 : 0;
	return OK;
}

int scrollok(WINDOW *win, int bf)
{
	(void)bf;
	return win ? OK : ERR;
}

static int key_delay = -1; /* -1 = wait, 0 = do not wait, >0 = MS */

int nodelay(WINDOW *win, int bf)
{
	(void)win;
	key_delay = bf ? 0 : -1;
	return OK;
}

int timeout(int delay)
{
	key_delay = delay;
	return OK;
}

int wtimeout(WINDOW *win, int delay)
{
	return timeout(delay);
}

int napms(int ms)
{
	usleep((useconds_t)ms * 1000);
	return OK;
}

int beep(void)
{
	out_put("\a", 1);
	return OK;
}

int flushinp(void)
{
	ilen = ipos = nunget = 0;
	mouse_head = mouse_tail = 0;
	return OK;
}

/* ------------------------------------------------------------- reading */
static int fill_input(int wait_ms)
{
	struct pollfd p[1];
	int n;

	if (nunget > 0)
		return 1;
	if (ipos < ilen)
		return 1;

	for (;;) {
		p[0].fd = in_fd;
		p[0].events = POLLIN;
		p[0].revents = 0;
		n = poll(p, 1, wait_ms);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return 0;
		}
		if (n == 0)
			return 0;
		ipos = 0;
		ilen = (int)read(in_fd, ibuf, sizeof(ibuf));
		if (ilen <= 0) {
			/* on a tty, 0 means "nothing yet" - we can keep waiting, or make an exception */
			ipos = 0;
			ilen = 0;
			if (wait_ms == 0)
				return 0;
			continue;
		}
		return 1;
	}
}

static int get_byte(int wait_ms)
{
	if (nunget > 0)
		return unget[--nunget];
	if (ipos < ilen)
		return ibuf[ipos++];
	if (!fill_input(wait_ms))
		return -1;
	if (ipos < ilen)
		return ibuf[ipos++];
	return -1;
}

static void unget_byte(int c)
{
	if (c < 0 || nunget >= (int)(sizeof(unget) / sizeof(unget[0])))
		return;
	unget[nunget++] = c;
}

int ungetch(int c)
{
	if (c == -1 || nunget >= (int)(sizeof(unget) / sizeof(unget[0])))
		return ERR;
	unget_byte(c);
	return OK;
}

int unget_wch(const wchar_t wch)
{
	return ungetch((int)wch);
}

/* table of built-in sequences - matches what the 2.3.2 emulator sends
 * (src/lib/termemu.c + apps/toaru_keymap.c): arrows ESC[A/B/C/D, F1-F4 ESCO PQRST,
 * the rest ESC[n~ */
static const struct { const char *seq; int code; } baked[] = {
	{"\033[A", KEY_UP},
	{"\033[B", KEY_DOWN},
	{"\033[C", KEY_RIGHT},
	{"\033[D", KEY_LEFT},
	{"\033OA", KEY_UP},
	{"\033OB", KEY_DOWN},
	{"\033OC", KEY_RIGHT},
	{"\033OD", KEY_LEFT},
	{"\033OH", KEY_HOME},
	{"\033OF", KEY_END},
	{"\033[1~", KEY_HOME},
	{"\033[7~", KEY_HOME},
	{"\033[H", KEY_HOME},
	{"\033[4~", KEY_END},
	{"\033[8~", KEY_END},
	{"\033[5~", KEY_PPAGE},
	{"\033[6~", KEY_NPAGE},
	{"\033[2~", KEY_IC},
	{"\033[3~", KEY_DC},
	{"\033[Z", KEY_BTAB},
	{"\033OP", KEY_F(1)},
	{"\033OQ", KEY_F(2)},
	{"\033OR", KEY_F(3)},
	{"\033OS", KEY_F(4)},
	{"\033OT", KEY_F(5)},
	{"\033[11~", KEY_F(1)},
	{"\033[12~", KEY_F(2)},
	{"\033[13~", KEY_F(3)},
	{"\033[14~", KEY_F(4)},
	{"\033[15~", KEY_F(5)},
	{"\033[17~", KEY_F(6)},
	{"\033[18~", KEY_F(7)},
	{"\033[19~", KEY_F(8)},
	{"\033[20~", KEY_F(9)},
	{"\033[21~", KEY_F(10)},
	{"\033[23~", KEY_F(11)},
	{"\033[24~", KEY_F(12)},
	{NULL, 0}
};

int define_key(const char *definition, int keycode)
{
	int i;
	if (!definition || !*definition)
		return ERR;
	for (i = 0; i < nuserkey; i++) {
		if (!strcmp(userkey[i].seq, definition)) {
			userkey[i].code = keycode;
			return OK;
		}
	}
	if (nuserkey >= USERKEYS)
		return ERR;
	userkey[nuserkey].heap = strdup(definition);
	if (!userkey[nuserkey].heap)
		return ERR;
	userkey[nuserkey].seq = userkey[nuserkey].heap;
	userkey[nuserkey].code = keycode;
	nuserkey++;
	return OK;
}

int keybound(int keycode, int count)
{
	(void)keycode;
	(void)count;
	return FALSE;
}

char *keyname(int keycode)
{
	static char name[16];
	if (keycode >= KEY_F(1) && keycode <= KEY_F(60)) {
		snprintf(name, sizeof(name), "F%d", keycode - KEY_F0);
		return name;
	}
	switch (keycode) {
	case KEY_UP: return "Up";
	case KEY_DOWN: return "Down";
	case KEY_LEFT: return "Left";
	case KEY_RIGHT: return "Right";
	case KEY_HOME: return "Home";
	case KEY_END: return "End";
	case KEY_PPAGE: return "PageUp";
	case KEY_NPAGE: return "PageDown";
	case KEY_IC: return "IC";
	case KEY_DC: return "DC";
	case KEY_BACKSPACE: return "Backspace";
	case KEY_BTAB: return "BTAB";
	case KEY_MOUSE: return "Mouse";
	case KEY_RESIZE: return "RESIZE";
	case KEY_ENTER: return "Enter";
	default: break;
	}
	if (keycode >= 32 && keycode < 127) {
		name[0] = (char)keycode;
		name[1] = 0;
		return name;
	}
	name[0] = 0;
	return name;
}

/* miniCommander asks for kmous to find out whether to send a mouse request
 * to the terminal; ToaruOS supports the mouse (ESC[M / ESC[<), so we answer */
char *tigetstr(const char *cap)
{
	if (cap && !strcmp(cap, "kmous"))
		return "\033[M";
	return NULL;
}

int tigetnum(const char *cap)
{
	if (cap && !strcmp(cap, "colors"))
		return color_enabled_shim ? 8 : 2;
	if (cap && !strcmp(cap, "pairs"))
		return COLOR_PAIRS;
	return -1;
}

/* mouse: X10 ESC[M b x y i SGR ESC[<b;x;yM - ToaruOS supports both, we enable
 * SGR in mousemask(), because it does not have the 223-column limit */
static void mouse_queue(int bx, int by, mmask_t bstate)
{
	MEVENT *m;
	if (((mouse_head + 1) % MOUSEQ) == mouse_tail)
		return;
	m = &mouseq[mouse_head];
	mouse_head = (mouse_head + 1) % MOUSEQ;
	m->id = 0;
	m->x = bx;
	m->y = by;
	m->z = 0;
	m->bstate = bstate;
}

#define MOTION_BIT 32
#define WHEEL_UP 64
#define WHEEL_DOWN 65

static void mouse_sgr(int button, int x, int y, int press)
{
	long now = now_ms();
	mmask_t st = 0;
	int btn = (button & 3);

	if (button & WHEEL_UP)
		st = BUTTON4_PRESSED;
	else if (button & WHEEL_DOWN)
		st = BUTTON5_PRESSED;
	else if (button & MOTION_BIT)
		st = (btn == 3 ? BUTTON_MOVED : (btn == 0 ? BUTTON1_MOVED : btn == 1 ? BUTTON2_MOVED : BUTTON3_MOVED));

	if (!st) {
		if (press) {
			st = btn == 0 ? BUTTON1_PRESSED : btn == 1 ? BUTTON2_PRESSED : BUTTON3_PRESSED;
			mouse_press_btn = btn;
			mouse_press_x = x;
			mouse_press_y = y;
			mouse_press_ms = now;
		} else {
			st = btn == 0 ? BUTTON1_RELEASED : btn == 1 ? BUTTON2_RELEASED : BUTTON3_RELEASED;
			if (mouse_press_btn == btn && mouse_press_x == x && mouse_press_y == y) {
				/* press and release on the same cell = click; repeating
				 * within mouseinterval() gives double and triple click (like ncurses) */
				if (now - mouse_last_click_ms <= mouse_interval && mouse_clicks)
					mouse_clicks++;
				else
					mouse_clicks = 1;
				mouse_last_click_ms = now;
				if (mouse_clicks >= 3)
					st |= btn == 0 ? BUTTON1_TRIPLE_CLICKED : BUTTON2_TRIPLE_CLICKED;
				else if (mouse_clicks == 2)
					st |= btn == 0 ? BUTTON1_DOUBLE_CLICKED : BUTTON2_DOUBLE_CLICKED;
				else
					st |= btn == 0 ? BUTTON1_CLICKED : BUTTON2_CLICKED;
			}
			mouse_press_btn = -1;
		}
	}
	if (button & 4)
		st |= BUTTON_CONTROL;
	if (button & 8)
		st |= BUTTON_SHIFT;
	if (button & 16)
		st |= BUTTON_ALT;
	if (x >= 0 && y >= 0)
		mouse_queue(x - 1, y - 1, st);
}

/* read the next byte of the sequence: ESC has already been consumed */
static int decode_escape(int *code, wint_t *ch)
{
	int c = get_byte(ESCDELAY < 20 ? 20 : ESCDELAY);
	int user_i, best = -1, bestlen = 0;
	char seq[32];
	int len = 0;

	if (c < 0) {
		*ch = 27; /* lone ESC */
		return OK;
	}
	seq[len++] = (char)c;

	/* first we check whether the user defined an exactly matching string
	 * (Alt+letter "\033s", "\033[15;2~" etc.) */
	for (;;) {
		for (user_i = 0; user_i < nuserkey; user_i++) {
			const char *u = userkey[user_i].seq;
			if (!strncmp(u, "\033", 1))
				u++; /* ESC already consumed */
			if ((int)strlen(u) == len && !memcmp(u, seq + 1, (size_t)len)) {
				*code = userkey[user_i].code;
				return KEY_CODE_YES;
			}
		}
		if (len >= 2) {
			char look[34];
			look[0] = 27;
			memcpy(look + 1, seq + 1, (size_t)len - 1);
			look[len] = 0;
			for (user_i = 0; baked[user_i].seq; user_i++)
				if (!strcmp(baked[user_i].seq, look)) {
					*code = baked[user_i].code;
					return KEY_CODE_YES;
				}
		}
		if (c >= 0x40 && c <= 0x7E && c != '[' && c != 'O' && c != 'M')
			break; /* final letter of CSI/SS3 */
		if (len >= (int)sizeof(seq) - 1)
			break;
		c = get_byte(ESCDELAY < 20 ? 20 : ESCDELAY);
		if (c < 0)
			break;
		seq[len++] = (char)c;
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '~') {
			/* try the whole ESC[...<letter> against the table */
			char look[34];
			int i;
			look[0] = 27;
			memcpy(look + 1, seq + 1, (size_t)len - 1);
			look[len] = 0;
			for (i = 0; baked[i].seq; i++)
				if (!strcmp(baked[i].seq, look)) {
					*code = baked[i].code;
					return KEY_CODE_YES;
				}
			for (i = 0; i < nuserkey; i++) {
				const char *u = userkey[i].seq;
				if (*u == 27)
					u++;
				if ((int)strlen(u) == len - 1 && !memcmp(u, seq + 1, (size_t)(len - 1))) {
					*code = userkey[i].code;
					return KEY_CODE_YES;
				}
			}
			break;
		}
	}
	(void)best;
	(void)bestlen;
	/* unrecognized sequence: discard it and return ESC so the app does not hang */
	{
		int i;
		for (i = len - 1; i >= 1; i--)
			unget_byte((unsigned char)seq[i]);
	}
	*ch = 27;
	return OK;
}

static int check_resize(void)
{
	struct winsize ws;
	if (ioctl(1, TIOCGWINSZ, &ws) != 0)
		return 0;
	if (ws.ws_row < 2 || ws.ws_col < 2)
		return 0;
	if (ws.ws_row == LINES && ws.ws_col == COLS)
		return 0;
	LINES = ws.ws_row;
	COLS = ws.ws_col;
	{
		WINDOW *ns = win_new(LINES, COLS, 0, 0);
		WINDOW *nc = win_new(LINES, COLS, 0, 0);
		WINDOW *ncc = win_new(LINES, COLS, 0, 0);
		if (!ns || !nc || !ncc) {
			win_free(ns);
			win_free(nc);
			win_free(ncc);
			return 0;
		}
		win_fill(ns, ' ', 0);
		win_fill(nc, ' ', 0);
		win_fill(ncc, ' ', 0xFFFFFFFFu);
		win_free(stdscr);
		win_free(newscr);
		win_free(curscr);
		stdscr = ns;
		newscr = nc;
		curscr = ncc;
		phys_y = phys_x = -1;
		last_attr = 0xDEAD;
	}
	return 1;
}

int wget_wch(WINDOW *win, wint_t *result)
{
	int c, wait;

	(void)win;
	if (!started)
		return ERR;

	if (check_resize()) {
		*result = (wint_t)KEY_RESIZE;
		return KEY_CODE_YES;
	}

	if (nunget > 0 || ipos < ilen)
		wait = 0;
	else
		wait = key_delay;

	c = get_byte(wait);
	if (c < 0)
		return ERR;

	if (c == 27) {
		int code = 0;
		wint_t ch = 0;
		int r = decode_escape(&code, &ch);
		if (r == KEY_CODE_YES) {
			*result = (wint_t)code;
			return KEY_CODE_YES;
		}
		*result = ch;
		return OK;
	}

	if (c < 0x20 || c == 0x7F) {
		*result = (wint_t)c;
		return OK;
	}

	if (c < 0x80) {
		*result = (wint_t)c;
		return OK;
	}

	/* UTF-8: the first byte indicates how many bytes follow */
	{
		unsigned int cp;
		int need, i;
		if ((c & 0xE0) == 0xC0) {
			cp = (unsigned)(c & 0x1F);
			need = 1;
		} else if ((c & 0xF0) == 0xE0) {
			cp = (unsigned)(c & 0x0F);
			need = 2;
		} else if ((c & 0xF8) == 0xF0) {
			cp = (unsigned)(c & 0x07);
			need = 3;
		} else {
			*result = (wint_t)'?';
			return OK;
		}
		for (i = 0; i < need; i++) {
			int b = get_byte(50);
			if (b < 0)
				break;
			cp = (cp << 6) | (unsigned)(b & 0x3F);
		}
		*result = (wint_t)cp;
		return OK;
	}
}

int get_wch(wint_t *result)
{
	return wget_wch(stdscr, result);
}

int wgetch(WINDOW *win)
{
	wint_t r = 0;
	int r2 = wget_wch(win, &r);
	if (r2 == ERR)
		return ERR;
	return (int)r;
}

int getch(void)
{
	return wgetch(stdscr);
}

/* ------------------------------------------------------------- mouse */
mmask_t mousemask(mmask_t newmask, mmask_t *oldmask)
{
	mmask_t old = mouse_events;
	if (oldmask)
		*oldmask = old;
	mouse_events = newmask;
	if (!started)
		return old;
	if (newmask) {
		out_str("\033[?1000h\033[?1002h\033[?1006h");
		alt_screen_enable();
	} else {
		out_str("\033[?1000l\033[?1002l\033[?1006l");
	}
	out_flush();
	return old;
}

int mouseinterval(int interval)
{
	int old = mouse_interval;
	if (interval >= 0)
		mouse_interval = interval;
	return old;
}

int getmouse(MEVENT *event)
{
	if (mouse_head == mouse_tail)
		return ERR;
	*event = mouseq[mouse_tail];
	mouse_tail = (mouse_tail + 1) % MOUSEQ;
	return OK;
}

int ungetmouse(MEVENT *event)
{
	if (!event)
		return ERR;
	mouse_queue(event->x, event->y, event->bstate);
	return OK;
}
