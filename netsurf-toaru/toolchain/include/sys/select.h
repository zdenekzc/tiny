#ifndef TOARU_SHIM_SYS_SELECT_H
#define TOARU_SHIM_SYS_SELECT_H
#include <sys/types.h>
#ifndef FD_SETSIZE
#define FD_SETSIZE 64
#endif
#ifndef FD_ZERO
#define FD_ZERO(set) ((set)->fds_bits[0] = 0)
#define FD_SET(fd, set) ((set)->fds_bits[0] |= 1U << (fd))
#define FD_CLR(fd, set) ((set)->fds_bits[0] &= ~(1U << (fd)))
#define FD_ISSET(fd, set) (((set)->fds_bits[0] & (1U << (fd))) != 0)
#endif
#endif
