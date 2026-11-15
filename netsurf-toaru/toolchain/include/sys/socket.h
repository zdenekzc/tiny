#ifndef TOARU_SHIM_SYS_SOCKET_H
#define TOARU_SHIM_SYS_SOCKET_H
#include_next <sys/socket.h>
/* ToaruOS has no AF_UNIX sockets.  curl only uses the constant as an unused
 * argument to its socketpair fallback (which builds a loopback TCP pair). */
#ifndef AF_UNIX
#define AF_UNIX 3
#endif
#ifndef PF_UNIX
#define PF_UNIX AF_UNIX
#endif
#endif
