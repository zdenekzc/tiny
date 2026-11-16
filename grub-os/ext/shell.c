void shell() {
    char line[256];

    puts("Welcome to your OS shell\n");

    while (1) {
        puts("> ");
        readline(line);

        if (!strcmp(line, "ls")) cmd_ls();
        else if (!strncmp(line, "run ", 4)) cmd_run(line+4);
        else if (!strcmp(line, "time")) {
            int t = syscall(SYS_TIME,0,0,0);
            printf("ticks=%d\n", t);
        }
    }
}
