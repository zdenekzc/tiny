/* Small fallbacks for the freestanding ToaruOS build.
 *
 * The ToaruOS <limits.h> is a placeholder, but the ToaruOS library sources
 * (text.c, the menu code) use INT_MAX/INT_MIN and PATH_MAX from it. */
#ifndef NK_TOARU_COMPAT_H
#define NK_TOARU_COMPAT_H

#ifndef INT_MAX
#define INT_MAX __INT_MAX__
#endif
#ifndef INT_MIN
#define INT_MIN (-__INT_MAX__ - 1)
#endif
#ifndef UINT_MAX
#define UINT_MAX (__INT_MAX__ * 2U + 1U)
#endif
#ifndef LONG_MAX
#define LONG_MAX __LONG_MAX__
#endif
#ifndef LONG_MIN
#define LONG_MIN (-__LONG_MAX__ - 1L)
#endif
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_PI_2
#define M_PI_2 1.57079632679489661923
#endif
#ifndef M_PI_4
#define M_PI_4 0.78539816339744830962
#endif
#ifndef M_E
#define M_E 2.7182818284590452354
#endif

#endif
