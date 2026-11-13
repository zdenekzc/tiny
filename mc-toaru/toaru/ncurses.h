/* ncurses.h - minimal replacement for ncursesw on ToaruOS
 *
 * The covered API is exactly what miniCommander calls (no panels, no subwindows,
 * no scrolling regions).  Properties that must match reality:
 *
 *   * the terminal in ToaruOS 2.3.2 is the libtoaru_termemu emulator
 *     (apps/terminal.c and apps/terminal-vga.c) - it supports
 *     CUP/CUD/CUU/CUF/CUB/CHA, ED, EL, SGR with 8 colors
 *     + bold as the bright variant, cursor hiding ESC[?25l, alt screen
 *     ESC[?1049 and mouse ?1000/?1002/?1006.  The terminfo database is NOT
 *     in the image, so all sequences are written literally here.
 *   * the window model is as in ncurses: wnoutrefresh(window) copies into newscr,
 *     doupdate() compares newscr with curscr (what the terminal "sees") and sends
 *     only the changes.  miniCommander depends on that - it controls which panel
 *     is drawn itself.
 *   * screen dimensions: ioctl(TIOCGWINSZ), otherwise LINES/COLUMNS, otherwise 24x80.
 *     The kernel sends SIGWINCH (20) only on TIOCSWINSZ, so a resize without a
 *     SIGWINCH handler never arrives - that is why we generate KEY_RESIZE
 *     ourselves when the size changes, which we detect while reading input.
 *
 * All cells store a code point (wchar_t) + attribute, so that UTF-8 file names
 * get through; the chtype return (mvwinch) truncates the character to 8 bits -
 * exactly like ncurses.
 */
#ifndef TOARU_NCURSES_H
#define TOARU_NCURSES_H

#include <stdarg.h>
#include <stdio.h>
#include <wchar.h>

typedef unsigned int attr_t;
typedef unsigned long chtype;
typedef unsigned long mmask_t;
typedef struct _SCREEN SCREEN;
struct _SCREEN { int dummy; };

#ifndef FALSE
#define FALSE 0
#endif
#ifndef TRUE
#define TRUE 1
#endif

#define ERR (-1)
#define OK 0

/* ------------------------------------------------------------- attributes */
#define A_NORMAL 0U
#define A_STANDOUT 0x100U
#define A_UNDERLINE 0x200U
#define A_REVERSE 0x400U
#define A_BLINK 0x800U
#define A_DIM 0x1000U
#define A_BOLD 0x2000U
#define A_INVIS 0x4000U
#define A_PROTECT 0x8000U
#define A_HORIZONTAL 0x10000U
#define A_LEFT 0x20000U
#define A_RIGHT 0x40000U
#define A_LOW 0x80000U
#define A_TOP 0x100000U
#define A_ATTRIBUTES 0xFFFFFF00U
#define A_CHARTEXT 0x000000FFU
#define A_COLOR 0x00FF0000U
#define COLOR_PAIR(n) (((attr_t)(n) << 16) & A_COLOR)
#define PAIR_NUMBER(a) (int)(((a) & A_COLOR) >> 16)

#define COLOR_BLACK 0
#define COLOR_RED 1
#define COLOR_GREEN 2
#define COLOR_YELLOW 3
#define COLOR_BLUE 4
#define COLOR_MAGENTA 5
#define COLOR_CYAN 6
#define COLOR_WHITE 7
#define COLORS 8
#define COLOR_PAIRS 64
#define COLOR_DEFAULT (-1)

/* ------------------------------------------------------------------ keys */
#define KEY_MIN 0400
#define KEY_BREAK 0400
#define KEY_DOWN 0401
#define KEY_UP 0402
#define KEY_LEFT 0403
#define KEY_RIGHT 0404
#define KEY_HOME 0405
#define KEY_BACKSPACE 0406
#define KEY_F0 0410
#define KEY_F(n) (KEY_F0 + (n))
#define KEY_DL 0510
#define KEY_IL 0511
#define KEY_DC 0512
#define KEY_IC 0513
#define KEY_EIC 0514
#define KEY_CLEAR 0515
#define KEY_EOS 0516
#define KEY_EOL 0517
#define KEY_SF 0520
#define KEY_SR 0521
#define KEY_NPAGE 0522
#define KEY_PPAGE 0523
#define KEY_STAB 0524
#define KEY_CTAB 0525
#define KEY_CATAB 0526
#define KEY_ENTER 0527
#define KEY_SRESET 0530
#define KEY_RESET 0531
#define KEY_PRINT 0532
#define KEY_LL 0533
#define KEY_A1 0534
#define KEY_A3 0535
#define KEY_B2 0536
#define KEY_B1 0537
#define KEY_B3 0540
#define KEY_C1 0541
#define KEY_C3 0542
#define KEY_BTAB 0543
#define KEY_BEG 0544
#define KEY_CANCEL 0545
#define KEY_CLOSE 0546
#define KEY_COMMAND 0547
#define KEY_COPY 0550
#define KEY_CREATE 0551
#define KEY_END 0552
#define KEY_EXIT 0553
#define KEY_FIND 0554
#define KEY_HELP 0555
#define KEY_MARK 0556
#define KEY_MESSAGE 0557
#define KEY_MOVE 0560
#define KEY_NEXT 0561
#define KEY_OPEN 0562
#define KEY_OPTIONS 0563
#define KEY_PREVIOUS 0564
#define KEY_REDO 0565
#define KEY_REFERENCE 0566
#define KEY_REFRESH 0567
#define KEY_REPLACE 0570
#define KEY_RESTART 0571
#define KEY_RESUME 0572
#define KEY_SAVE 0573
#define KEY_SBUY 0575
#define KEY_SDOWN 0576
#define KEY_SECONOMY 0577
#define KEY_SREDO 0600
#define KEY_SREPLACE 0601
#define KEY_SRESET2 0602
#define KEY_SRIGHT 0603
#define KEY_SRSUME 0604
#define KEY_SSAVE 0605
#define KEY_SSUSPEND 0606
#define KEY_SUNDO 0607
#define KEY_SUSPEND 0610
#define KEY_UNDO 0611
#define KEY_MOUSE 0631
#define KEY_RESIZE 0632
#define KEY_MAX 0777

#define KEY_CODE_YES 1

/* --------------------------------------------------------------- mouse
 * The values are our own (not as in ncurses) - what matters is that each event
 * has its own bit and that they are filled in by the same sequence decoder that
 * checks them.  miniCommander works only with BUTTON1_* and the wheel (4/5) and
 * REPORT_MOUSE_POSITION.
 */
#define BUTTON1_PRESSED 0x00000001UL
#define BUTTON1_RELEASED 0x00000002UL
#define BUTTON1_CLICKED 0x00000004UL
#define BUTTON1_DOUBLE_CLICKED 0x00000008UL
#define BUTTON1_TRIPLE_CLICKED 0x00000010UL
#define BUTTON1_MOVED 0x00000020UL
#define BUTTON1_RESERVED_EVENT 0x00000040UL
#define BUTTON2_PRESSED 0x00000080UL
#define BUTTON2_RELEASED 0x00000100UL
#define BUTTON2_CLICKED 0x00000200UL
#define BUTTON2_DOUBLE_CLICKED 0x00000400UL
#define BUTTON2_TRIPLE_CLICKED 0x00000800UL
#define BUTTON2_MOVED 0x00001000UL
#define BUTTON2_RESERVED_EVENT 0x00002000UL
#define BUTTON3_PRESSED 0x00004000UL
#define BUTTON3_RELEASED 0x00008000UL
#define BUTTON3_CLICKED 0x00010000UL
#define BUTTON3_DOUBLE_CLICKED 0x00020000UL
#define BUTTON3_TRIPLE_CLICKED 0x00040000UL
#define BUTTON3_MOVED 0x00080000UL
#define BUTTON3_RESERVED_EVENT 0x00100000UL
#define BUTTON4_PRESSED 0x00200000UL
#define BUTTON4_RELEASED 0x00400000UL
#define BUTTON4_CLICKED 0x00800000UL
#define BUTTON5_PRESSED 0x01000000UL
#define BUTTON5_RELEASED 0x02000000UL
#define BUTTON5_CLICKED 0x04000000UL
#define BUTTON_MOVED 0x08000000UL
#define BUTTON_CLICKED 0x10000000UL
#define BUTTON_DOUBLE_CLICKED 0x20000000UL
#define BUTTON_TRIPLE_CLICKED 0x40000000UL
#define BUTTON_RESERVED_EVENT 0x70000000UL /* sum of the three above - just for checking */
#define BUTTON_CONTROL 0U
#define BUTTON_SHIFT 0U
#define BUTTON_ALT 0U
#define REPORT_MOUSE_POSITION 0x80000000UL
#define ALL_MOUSE_EVENTS 0x7FFFFFFFUL
#define NO_MOUSE_EVENTS 0UL

typedef struct {
	int id;
	int x, y, z;
	mmask_t bstate;
} MEVENT;

/* -------------------------------------------------------- types and macros */
#ifndef CCHARW_MAX
#define CCHARW_MAX 5 /* as in ncurses: 1 character + room for a combining mark */
#endif

typedef struct {
	wchar_t wc[CCHARW_MAX]; /* entry 0 is the character, the rest are combining marks */
	attr_t attr;
	short color;
} cchar_t;

typedef struct _WINDOW {
	short begin_y, begin_x;
	short maxy, maxx;
	short cury, curx;
	int keypad;
	unsigned int bgch;
	unsigned int bgattr;
	unsigned int *ch;   /* maxy*maxx code points */
	unsigned int *attr; /* maxy*maxx attributes */
	int touched;
} WINDOW;

extern WINDOW *stdscr;
extern WINDOW *newscr;
extern WINDOW *curscr;
extern int LINES;
extern int COLS;
extern int ESCDELAY;
extern int color_enabled_shim;

#define getyx(w, y, x) ((y) = (w)->cury, (x) = (w)->curx)
#define getcury(w) ((w)->cury)
#define getcurx(w) ((w)->curx)
#define getbegyx(w, y, x) ((y) = (w)->begin_y, (x) = (w)->begin_x)
#define getbegy(w) ((w)->begin_y)
#define getbegx(w) ((w)->begin_x)
#define getmaxyx(w, y, x) ((y) = (w)->maxy, (x) = (w)->maxx)
#define getmaxy(w) ((w)->maxy)
#define getmaxx(w) ((w)->maxx)

/* ----------------------------------------------------------- prototypes */
SCREEN *newterm(const char *type, FILE *out, FILE *in);
WINDOW *initscr(void);
int endwin(void);
int isendwin(void);
int refresh(void);
int doupdate(void);
int wnoutrefresh(WINDOW *win);
int wrefresh(WINDOW *win);
int redrawwin(WINDOW *win);

WINDOW *newwin(int nlines, int ncols, int begin_y, int begin_x);
WINDOW *dupwin(WINDOW *win);
int delwin(WINDOW *win);
int wresize(WINDOW *win, int lines, int columns);
int overwrite(WINDOW *src, WINDOW *dst);
int touchwin(WINDOW *win);
int is_wintouched(WINDOW *win);
int werase(WINDOW *win);
int erase(void);
int wclrtoeol(WINDOW *win);
int clrtoeol(void);
int wborder(WINDOW *win, chtype ls, chtype rs, chtype ts, chtype bs, chtype tl,
	chtype tr, chtype bl, chtype br);
int wbkgd(WINDOW *win, chtype bg);
int wbkgdset(WINDOW *win, chtype bg);
void getbegxy(WINDOW *win, int *y, int *x);

int wmove(WINDOW *win, int y, int x);
int move(int y, int x);
int waddch(WINDOW *win, chtype ch);
int addch(chtype ch);
int mvaddch(int y, int x, chtype ch);
int mvwaddch(WINDOW *win, int y, int x, chtype ch);
int waddstr(WINDOW *win, const char *s);
int addstr(const char *s);
int mvaddstr(int y, int x, const char *s);
int mvwaddstr(WINDOW *win, int y, int x, const char *s);
int mvaddnstr(int y, int x, const char *s, int n);
int mvwaddnstr(WINDOW *win, int y, int x, const char *s, int n);
int waddnstr(WINDOW *win, const char *s, int n);
int winsch(WINDOW *win, chtype ch);
int mvwinsch(WINDOW *win, int y, int x, chtype ch);
 chtype mvwinch(WINDOW *win, int y, int x);
 chtype winch(WINDOW *win);
int mvhline(int y, int x, chtype ch, int n);
int mvwhline(WINDOW *win, int y, int x, chtype ch, int n);
int whline(WINDOW *win, chtype ch, int n);
int mvvline(int y, int x, chtype ch, int n);
int mvwvline(WINDOW *win, int y, int x, chtype ch, int n);
int wvline(WINDOW *win, chtype ch, int n);
int wprintw(WINDOW *win, const char *fmt, ...);
int mvwprintw(WINDOW *win, int y, int x, const char *fmt, ...);
int printw(const char *fmt, ...);
int mvprintw(int y, int x, const char *fmt, ...);
int mvcur(int oldrow, int oldcol, int newrow, int newcol);

int wadd_wch(WINDOW *win, const cchar_t *wch);
int mvwadd_wch(WINDOW *win, int y, int x, const cchar_t *wch);
int mvwadd_wchnstr(WINDOW *win, int y, int x, const cchar_t *wch, int n);
int setcchar(cchar_t *wcval, const wchar_t *wch, const attr_t attrs,
	short color_pair, void *opts);
int getcchar(const cchar_t *wcval, wchar_t *wch, attr_t *attrs,
	short *color_pair, void *opts);

int attron(attr_t a);
int wattroff(WINDOW *win, attr_t a);
int attroff(attr_t a);
int wattron(WINDOW *win, attr_t a);
int attr_off(attr_t a, void *opts);
int wattr_off(WINDOW *win, attr_t a, void *opts);
int attr_on(attr_t a, void *opts);
int wattr_on(WINDOW *win, attr_t a, void *opts);
int attrset(attr_t a);
int wattr_set(WINDOW *win, attr_t attrs, short pair, void *opts);
int wattr_get(WINDOW *win, attr_t *attrs, short *pair, void *opts);
void getattrs(WINDOW *win, attr_t *attrs);

int start_color(void);
int init_pair(short pair, short f, short b);
int init_color(short color, short r, short g, short b);
int has_colors(void);
int can_change_color(void);
int use_default_colors(void);

int curs_set(int visibility);
int raw(void);
int cbreak(void);
int nocbreak(void);
int noecho(void);
int echo(void);
int nl(void);
int nonl(void);
int keypad(WINDOW *win, int bf);
int nodelay(WINDOW *win, int bf);
int scrollok(WINDOW *win, int bf);
int timeout(int delay);
int wtimeout(WINDOW *win, int delay);
int napms(int ms);
int beep(void);
int flushinp(void);

int wget_wch(WINDOW *win, wint_t *result);
int get_wch(wint_t *result);
int wgetch(WINDOW *win);
int getch(void);
int ungetch(int ch);
int unget_wch(const wchar_t wch);

mmask_t mousemask(mmask_t newmask, mmask_t *oldmask);
int mouseinterval(int interval);
int getmouse(MEVENT *event);
int ungetmouse(MEVENT *event);

int define_key(const char *definition, int keycode);
int keybound(int keycode, int count);
char *keyname(int keycode);
char *tigetstr(const char *cap);
int tigetnum(const char *cap);

#endif /* TOARU_NCURSES_H */
