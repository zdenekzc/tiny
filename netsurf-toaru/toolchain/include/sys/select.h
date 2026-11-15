#ifndef TOARU_SHIM_SYS_SELECT_H
#define TOARU_SHIM_SYS_SELECT_H
#include <sys/types.h>
/* ToaruOS has the fd_set type but no select() and no FD_* macros.  The
 * struct only has room for 32 bits (fd_mask is unsigned int), so keep
 * FD_SETSIZE consistent with that. */
#undef FD_SETSIZE
#define FD_SETSIZE 32
#ifndef FD_ZERO
#define FD_ZERO(set) ((set)->fds_bits[0] = 0)
#define FD_SET(fd, set) ((set)->fds_bits[0] |= 1U << (fd))
#define FD_CLR(fd, set) ((set)->fds_bits[0] &= ~(1U << (fd)))
#define FD_ISSET(fd, set) (((set)->fds_bits[0] & (1U << (fd))) != 0)
#endif
#endif
