#ifndef TOARU_BUILD_COMPAT_H
#define TOARU_BUILD_COMPAT_H
#ifndef CHAR_BIT
#define CHAR_BIT 8
#endif
#ifndef SCHAR_MIN
#define SCHAR_MIN (-128)
#define SCHAR_MAX 127
#define UCHAR_MAX 255
#endif
#ifndef SHRT_MIN
#define SHRT_MIN (-32768)
#define SHRT_MAX 32767
#define USHRT_MAX 65535
#endif
#ifndef INT_MIN
#define INT_MIN (-__INT_MAX__ - 1)
#define INT_MAX __INT_MAX__
#define UINT_MAX (__INT_MAX__ * 2U + 1U)
#endif
#ifndef LONG_MIN
#define LONG_MIN (-__LONG_MAX__ - 1L)
#define LONG_MAX __LONG_MAX__
#define ULONG_MAX (__LONG_MAX__ * 2UL + 1UL)
#endif
#ifndef LLONG_MIN
#define LLONG_MIN (-__LONG_LONG_MAX__ - 1LL)
#define LLONG_MAX __LONG_LONG_MAX__
#define ULLONG_MAX (__LONG_LONG_MAX__ * 2ULL + 1ULL)
#endif
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
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
#ifndef PRIX32
#define PRIX32 "X"
#define PRIX64 "lX"
#define PRIX16 "X"
#define PRIX8 "X"
#define PRIo32 "o"
#define PRIo64 "lo"
#define SCNx32 "x"
#define SCNx64 "lx"
#define SCNu32 "u"
#define SCNu64 "lu"
#define SCNd32 "d"
#define SCNd64 "ld"
#endif
#ifndef PRIxPTR
#define PRIxPTR "lx"
#endif
