#pragma once
#include <stdint.h>

void chainload_multiboot_elf(void *image, uint32_t size, uint32_t mb_magic, void *mb_info);
