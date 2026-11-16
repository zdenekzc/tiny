#pragma once
#include <stddef.h>

/* edit buf in place (capacity cap); returns new content length */
size_t editor_run(char *buf, size_t len, size_t cap, const char *title);

/* read-only pager over buf */
void editor_view(const char *buf, size_t len, const char *title);
