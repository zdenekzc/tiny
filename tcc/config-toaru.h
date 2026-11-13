/* config-toaru.h -- config.h for the tcc compiler targeting ToaruOS
 *
 * It is generated on its own (same name, just in _build/gen/) so that a single
 * gcc invocation is enough: there is no make nor configure in the guest.
 * Description of the items: README.md, section "tcc for ToaruOS".
 *
 * Two host variants:
 *   cross compiler     gcc on Linux,        -DTOARU_HOST_LINUX  (x86_64-toaru-tcc)
 *   compiler in guest  gcc 10.3 inside ToaruOS, without it      (tcc.bin)
 */

#define TCC_VERSION "0.9.28-toaru"

#ifdef TOARU_HOST_LINUX
# define CC_NAME CC_gcc
# define GCC_MAJOR 16
# define GCC_MINOR 0
#else
/* host = ToaruOS (gcc 10.3 from msk install build-essential) */
# define CC_NAME CC_gcc
# define GCC_MAJOR 10
# define GCC_MINOR 3
/* no dlfcn(3), no shared libtcc, no backtrace via libunwind */
# define CONFIG_TCC_STATIC 1
#endif

/* ---- target ----------------------------------------------------------- */

#define TCC_TARGET_X86_64 1
#define TARGETOS_Toaru    1       /* new target OS in tcc, see tcc-toaru.patch */

/* ToaruOS has no mmap/mprotect and no dl*, so PIE/PIC, bound checking and the
   builtin backtrace are all unusable here.

   PIE and PIC are therefore not defined AT ALL: tcc tests them with #ifdef, not
   against a value (libtcc.c "#ifdef CONFIG_TCC_PIE -> output_type |=
   TCC_OUTPUT_DYN", and tcc.h turns CONFIG_TCC_PIE into CONFIG_TCC_PIC).  Writing
   "#define CONFIG_TCC_PIE 0" would switch PIE ON and the linker would emit
   ET_DYN based at 0, which the ToaruOS kernel refuses to run - the same failure
   as the GNU ld image in README §5g.  BCHECK and BACKTRACE on the other hand
   have to be 0 (tcc.h drops them itself when the value is 0), and SEMLOCK and
   PREDEFS are value tests as well. */
#define CONFIG_TCC_BCHECK     0
#define CONFIG_TCC_BACKTRACE  0
#define CONFIG_TCC_SEMLOCK    0     /* no threads */

/* tccdefs.h is only loaded during compilation from <tccdir>/include,
   no need to generate tccdefs_.h using c2str.exe */
#define CONFIG_TCC_PREDEFS  0

/* ---- paths (overridden by -B / --sysroot at run time) ------------------- */

/* {B} is replaced by the address from -B; this is how a configure-built tcc handles paths too */
#ifndef CONFIG_TCCDIR
# define CONFIG_TCCDIR "/usr/local/lib/tcc"
#endif

#ifndef CONFIG_TCC_CRTPREFIX
# define CONFIG_TCC_CRTPREFIX "{B}"
#endif

#ifndef CONFIG_TCC_SYSINCLUDEPATHS
# define CONFIG_TCC_SYSINCLUDEPATHS "{B}/include"
#endif

#ifndef CONFIG_TCC_LIBPATHS
# define CONFIG_TCC_LIBPATHS "{B}"
#endif

/* The ONLY valid way to run a program in ToaruOS 2.3.2 is a static
   image; the kernel would hand PT_DYNAMIC to /lib/ld.so.  tcc sticks to that too. */
#ifndef CONFIG_TCC_ELFINTERP
# define CONFIG_TCC_ELFINTERP "-"
#endif
