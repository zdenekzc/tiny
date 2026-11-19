#pragma once

/* ToaruOS compatibility: the guest libc has wcs* and wcwidth but no
 * multibyte/wctype layer, which tiny-ide's UTF-8 editor needs.  This header
 * (found before the sysroot because of -I toaru) declares the missing
 * pieces; the implementations live in toaru/compat.c.
 *
 *wchar_t, wint_t and the wcs* prototypes come from <wchar.h>. */

#include <stddef.h>
#include <wchar.h>

#ifndef MB_LEN_MAX
#define MB_LEN_MAX 4 /* UTF-8 never needs more */
#endif
#ifndef MB_CUR_MAX
#define MB_CUR_MAX 4
#endif
#ifndef INT_MAX
#define INT_MAX 2147483647
#endif
#ifndef SSIZE_MAX
#define SSIZE_MAX ((long long)(INT_MAX))
#endif

/* The guest libc has no fsync; toaru/compat.c provides the no-op. */
int fsync(int fd);

#if !defined(____mbstate_t_defined) && !defined(__mbstate_t_defined)
typedef struct {
    int bytes; /* continuation bytes expected by the pending UTF-8 sequence */
    unsigned int code;
} mbstate_t;
#define __mbstate_t_defined 1
#define ____mbstate_t_defined 1
#endif

size_t mbrtowc(wchar_t *pwc, const char *s, size_t count, mbstate_t *state);
size_t wcrtomb(char *s, wchar_t wc, mbstate_t *state);
size_t mbrlen(const char *s, size_t count, mbstate_t *state);

int iswprint(wint_t wc);
int iswspace(wint_t wc);
int iswalnum(wint_t wc);
