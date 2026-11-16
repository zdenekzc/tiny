#include "ext2.h"
#include <string.h>
#include <stdint.h>

static int is_sep(char c) {
    return c == '/' || c == '\\';
}

static const char *next_token(const char *p, char *out, int max) {
    while (is_sep(*p)) p++;        // skip leading slashes
    if (!*p) return NULL;

    int i = 0;
    while (*p && !is_sep(*p) && i < max-1)
        out[i++] = *p++;

    out[i] = 0;
    return p;
}

/* Walk a path like "/bin/sh" and return inode number */
uint32_t ext2_inode_by_path(const char *path) {
    char token[64];
    const char *p = path;

    uint32_t ino = 2;  // root directory

    while ((p = next_token(p, token, sizeof(token)))) {
        uint32_t child_ino = ext2_find_in_dir(ino, token);
        if (!child_ino)
            return 0;  // not found
        ino = child_ino;
    }

    return ino;
}
