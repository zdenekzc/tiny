/* ToaruOS compatibility: UTF-8 multibyte conversion and wide classification.
 * Only what tiny-ide uses; see toaru/wctype.h. */
#include <wchar.h>
#include <wctype.h>

int fsync(int fd)
{
    (void)fd;
    return 0; /* no fsync in the guest; saves stay as durable as rename() is */
}

/* ---- UTF-8 <-> wchar_t (code point == Unicode scalar value) ---- */

size_t wcrtomb(char *s, wchar_t wc, mbstate_t *state)
{
    (void)state;
    if (!s)
        return 0; /* no shift state, nothing to reset */
    if (wc < 0x80) {
        *s = (char)wc;
        return 1;
    }
    if (wc < 0x800) {
        s[0] = (char)(0xC0 | (wc >> 6));
        s[1] = (char)(0x80 | (wc & 0x3F));
        return 2;
    }
    if (wc >= 0xD800 && wc <= 0xDFFF)
        return (size_t)-1; /* surrogates are not scalar values */
    if (wc < 0x10000) {
        s[0] = (char)(0xE0 | (wc >> 12));
        s[1] = (char)(0x80 | ((wc >> 6) & 0x3F));
        s[2] = (char)(0x80 | (wc & 0x3F));
        return 3;
    }
    if (wc <= 0x10FFFF) {
        s[0] = (char)(0xF0 | (wc >> 18));
        s[1] = (char)(0x80 | ((wc >> 12) & 0x3F));
        s[2] = (char)(0x80 | ((wc >> 6) & 0x3F));
        s[3] = (char)(0x80 | (wc & 0x3F));
        return 4;
    }
    return (size_t)-1;
}

size_t mbrtowc(wchar_t *pwc, const char *s, size_t count, mbstate_t *state)
{
    /* state->bytes: continuation bytes still expected after state->code;
       state->code holds the accumulated bits.  state == NULL means a fresh
       conversion, which is fine because our state is self-contained. */
    mbstate_t fresh;
    if (!state) {
        fresh.bytes = 0;
        state = &fresh;
    }
    if (!s) {
        state->bytes = 0;
        return 0; /* no incomplete sequence pending */
    }
    for (size_t i = 0; i < count; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (!state->bytes) {
            if (!c)
                return 0; /* caller resumes after the NUL */
            if (c < 0x80) {
                if (pwc)
                    *pwc = (wchar_t)c;
                return i + 1;
            }
            if ((c & 0xE0) == 0xC0) {
                state->code = c & 0x1F;
                state->bytes = 1;
            } else if ((c & 0xF0) == 0xE0) {
                state->code = c & 0x0F;
                state->bytes = 2;
            } else if ((c & 0xF8) == 0xF0) {
                state->code = c & 0x07;
                state->bytes = 3;
            } else {
                return (size_t)-1; /* invalid lead byte */
            }
        } else if ((c & 0xC0) == 0x80) {
            state->code = (state->code << 6) | (c & 0x3F);
            state->bytes--;
        } else {
            state->bytes = 0;
            return (size_t)-1; /* broken sequence */
        }
        if (!state->bytes) {
            /* reject overlong and surrogate encodings */
            wchar_t wc = (wchar_t)state->code;
            if ((wc < 0x80 && 0) || (wc >= 0xD800 && wc <= 0xDFFF))
                return (size_t)-1;
            if (pwc)
                *pwc = wc;
            return i + 1;
        }
    }
    return (size_t)-2; /* need more input */
}

size_t mbrlen(const char *s, size_t count, mbstate_t *state)
{
    wchar_t ignored;
    return mbrtowc(&ignored, s, count, state);
}

/* ---- wide classification: enough for editing and highlighting ---- */

int iswprint(wint_t wc)
{
    if (wc >= 0x20 && wc != 0x7F)
        return 1; /* everything from U+00A0 up is printable as far as the
                     text model cares; control-area code points other than
                     the ASCII ones are extremely unlikely in source files */
    return wc >= 0xA0;
}

int iswspace(wint_t wc)
{
    return wc == ' ' || (wc >= 0x09 && wc <= 0x0D) ||
           wc == 0x85 || wc == 0xA0 || wc == 0x2028 || wc == 0x2029;
}

int iswalnum(wint_t wc)
{
    if (wc < 0x80)
        return (wc >= '0' && wc <= '9') || (wc | 0x20) >= 'a' && (wc | 0x20) <= 'z';
    /* treat all non-ASCII letters as word characters; tiny-ide uses this
       only for word motions and the keyword lexer, where a conservative
       answer is harmless */
    return !iswspace(wc) && wc != '(' && wc != ')' && wc != '{' && wc != '}' &&
           wc != '[' && wc != ']' && wc != ',' && wc != ';' && wc != ':' &&
           wc != '.' && wc != '!' && wc != '?' && wc != '"' && wc != '\'';
}
