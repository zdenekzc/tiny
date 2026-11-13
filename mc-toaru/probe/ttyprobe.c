/* ttyprobe.c - verifies whether curses can be done on ToaruOS: raw mode,
 * terminal size, non-blocking reads.  Compiled with the cross tcc
 * (mc-toaru/build.sh), transferred into the guest via fetch
 * (mc-toaru/run-in-guest.sh).
 *
 * Output on the serial line after switching to raw mode has no ONLCR, so the
 * characters "\r\n" are written by hand - the custom curses shim must do the
 * same.
 */
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/termios.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>

static void dump(const char *tag, struct termios *t) {
	printf("%s iflag=%x oflag=%x cflag=%x lflag=%x vm=%x vt=%x%s\r\n", tag,
		(unsigned)t->c_iflag, (unsigned)t->c_oflag, (unsigned)t->c_cflag,
		(unsigned)t->c_lflag, (unsigned)t->c_cc[VMIN], (unsigned)t->c_cc[VTIME],
		(t->c_lflag & ICANON) ? " CANON" : " RAW");
}

/* poll() with a zero timeout: this is what timeout() in curses needs (mc calls
 * it because of KEY_RESIZE and panel switching).  ToaruOS libc has no select,
 * but it has poll. */
static void probe_poll(void) {
	struct pollfd p[1];
	int r;
	p[0].fd = 0;
	p[0].events = POLLIN;
	p[0].revents = 0;
	errno = 0;
	r = poll(p, 1, 0);
	printf("poll(0ms) r=%d revents=%x errno=%d%s\r\n", p[0].revents, r, errno,
		r < 0 ? " => timeout does not work" : "");
	p[0].events = POLLIN;
	r = poll(p, 1, 200);
	printf("poll(200ms) r=%d revents=%x (0=no input, 1=someone is writing)\r\n",
		r, p[0].revents);
}

int main(void) {
	struct termios t, orig;
	struct winsize ws;
	int i, r;

	printf("isatty(0)=%d isatty(1)=%d TERM=%s\r\n", isatty(0), isatty(1),
		getenv("TERM") ? getenv("TERM") : "(none)");

	if (tcgetattr(0, &orig)) {
		printf("tcgetattr failed errno=%d - curses will not work on this line\r\n", errno);
		return 1;
	}
	dump("original", &orig);

	memset(&ws, 0, sizeof(ws));
	errno = 0;
	r = ioctl(0, TIOCGWINSZ, &ws);
	printf("TIOCGWINSZ(0) r=%d %ux%u errno=%d\r\n", r, ws.ws_row, ws.ws_col, errno);
	memset(&ws, 0, sizeof(ws));
	r = ioctl(1, TIOCGWINSZ, &ws);
	printf("TIOCGWINSZ(1) r=%d %ux%u errno=%d\r\n", r, ws.ws_row, ws.ws_col, errno);

	/* cfmakeraw by hand - ToaruOS libc does not have it */
	t = orig;
	t.c_iflag &= ~(unsigned)(IXON | IXOFF | IXANY | INLCR | ICRNL | ISTRIP | BRKINT | IGNCR);
	t.c_oflag &= ~(unsigned)OPOST;
	t.c_lflag &= ~(unsigned)(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
	t.c_cflag &= ~(unsigned)(CSIZE | PARENB);
	t.c_cflag |= CS8;
	t.c_cc[VMIN] = 1;
	t.c_cc[VTIME] = 0;
	errno = 0;
	if (tcsetattr(0, TCSANOW, &t))
		printf("tcsetattr failed errno=%d\r\n", errno);
	tcgetattr(0, &t);
	dump("after RAW", &t);

	printf("READY - send characters one at a time, a keypress without Enter must be enough\r\n");
	fflush(stdout);

	for (i = 0; i < 4; i++) {
		char c = 0;
		int n = (int)read(0, &c, 1);
		printf("GOT[%d] n=%d char=%d\r\n", i, n, (unsigned char)c);
		fflush(stdout);
	}

	probe_poll();

	tcsetattr(0, TCSANOW, &orig);
	printf("RESTORED\r\n");
	return 0;
}
