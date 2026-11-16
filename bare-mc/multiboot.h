/* Minimal multiboot2 header for GRUB
 * This header will be placed in the .multiboot section so GRUB can recognize the image.
 */
#ifndef MULTIBOOT_H
#define MULTIBOOT_H

#include <stdint.h>

/* Multiboot2 constants */
#define MULTIBOOT2_HEADER_MAGIC 0xE85250D6
#define MULTIBOOT2_HEADER_ARCH 0        /* 0 = i386 (works for x86_64 as well) */
#define MULTIBOOT2_HEADER_LENGTH 24
/* Checksum so that magic + arch + length + checksum == 0 */
#define MULTIBOOT2_HEADER_CHECKSUM (-(MULTIBOOT2_HEADER_MAGIC + MULTIBOOT2_HEADER_ARCH + MULTIBOOT2_HEADER_LENGTH))

/* Tag types */
#define MULTIBOOT_TAG_TYPE_END 0

/* Place the header in its own section */
__attribute__((section(".multiboot"), used))
static const uint32_t multiboot_header[] = {
    MULTIBOOT2_HEADER_MAGIC,
    MULTIBOOT2_HEADER_ARCH,
    MULTIBOOT2_HEADER_LENGTH,
    MULTIBOOT2_HEADER_CHECKSUM,
    /* End tag */
    MULTIBOOT_TAG_TYPE_END,
    8,
    0, 0
};

#endif /* MULTIBOOT_H */