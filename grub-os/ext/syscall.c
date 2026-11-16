int syscall_handler(uint32_t num, uint32_t a, uint32_t b, uint32_t c) {
    switch (num) {

    case SYS_WRITE:
        return vga_write((const char*)a, b);

    case SYS_TIME:
        return timer_ticks;

    case SYS_EXIT:
        task_exit(a);

    case SYS_YIELD:
        schedule();
        return 0;

    case SYS_SBRK:
        return process_sbrk(a);

    case SYS_EXEC:
        return k_exec((const char*)a);

    default:
        return -1;
    }
}
