#pragma once
#include <stdint.h>

int tftp_get(const char *filename, uint8_t *dest, uint32_t maxlen, uint32_t *out_size);
