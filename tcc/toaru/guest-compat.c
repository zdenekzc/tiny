/* guest-compat.c -- two functions that the ToaruOS 2.3.2 libc does not have, but
 * tcc needs them when linking its own image for running inside ToaruOS.
 *
 *   strtold  tccpp.c calls it when converting a numeric literal to long double
 *   ldexpl   tccpp.c calls it for the long double exponent
 *
 * The ToaruOS libc knows only the double variants (strtod, ldexp), long double is
 * the same as double there (x86_64 ABI, but without 80-bit instructions in the
 * library), so the cast is exactly what the local gcc would do too.  The file is
 * appended only to the image of tcc that runs in ToaruOS (mktcc.sh, target
 * "tcc-guest"); the cross compiler on Linux has both functions from the host libc.
 */
#include <stdlib.h>
#include <math.h>

long double strtold(const char *nptr, char **endptr)
{
    return (long double)strtod(nptr, endptr);
}

long double ldexpl(long double x, int exp)
{
    return (long double)ldexp((double)x, exp);
}
