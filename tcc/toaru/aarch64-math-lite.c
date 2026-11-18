/* aarch64-math-lite.c — the one libm symbol the aarch64 ToaruOS links miss.
 *
 * The x86_64 IDE build does not need it: gcc on x86 compiles floor() inline
 * (SSE roundsd), while the aarch64 gcc emits a call to the libm symbol — and
 * neither the ToaruOS libc.a nor libgcc defines it (upstream aarch64 userland
 * simply never links libm).  Measured with nuklear-ide (stb_truetype font
 * rasteriser) as the only missing symbol, 2026-11-18.
 *
 * Implemented with plain bit manipulation, no floating-point tricks: truncate
 * toward zero by clearing the fractional mantissa bits, then step down when
 * the value was negative and not already integral.
 */

double floor(double x) {
    union { double d; unsigned long long u; } v = { x };
    int exp = (int)((v.u >> 52) & 0x7FF) - 1023;
    int neg = (int)(v.u >> 63);

    if (exp >= 52) return x;              /* integral, inf or nan */
    if (exp < 0)   return neg ? -0.0 : 0.0; /* |x| < 1 */
    unsigned long long mask = (1ULL << (52 - exp)) - 1;
    if ((v.u & mask) == 0) return x;      /* already integral */
    v.u &= ~mask;                         /* truncate toward zero */
    return neg ? v.d - 1.0 : v.d;
}
