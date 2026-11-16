ENTRY(kernel_main)

SECTIONS
{
    /* Kernel is loaded by GRUB at 1 MB */
    . = 0x00100000;

    /* Multiboot header must be in the first 8 KB */
    .multiboot :
    {
        *(.multiboot)
        *(.multiboot_header)
    }

    /* Text (code) */
    .text ALIGN(4K) :
    {
        *(.text*)
        *(.rodata*)
    }

    /* Initialized data */
    .data ALIGN(4K) :
    {
        *(.data*)
    }

    /* Uninitialized data (zeroed) */
    .bss ALIGN(4K) :
    {
        *(.bss*)
        *(COMMON)
    }

    /* ---------------------------------------------------------
       Heap for TinyCC JIT and malloc()
       --------------------------------------------------------- */
    . = ALIGN(4K);
    __heap_start = .;

    /* Reserve 4 MB for heap */
    . = . + 4M;

    __heap_end = .;

    /* End of kernel image */
    _end = .;
}
