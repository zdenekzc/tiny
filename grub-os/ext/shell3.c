int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, int n);
int exec(const char *path);
int printf(const char *fmt, ...);
int puts(const char *s);
int readline(char *buf, int maxlen);

int main(void) {
    char line[256];

    puts("Welcome to tiny user shell");

    while (1) {
        printf("> ");
        int n = readline(line, sizeof(line));
        if (n <= 0) continue;

        if (!strcmp(line, "help")) {
            puts("Commands:");
            puts("  help         - show this help");
            puts("  echo TEXT    - print TEXT");
            puts("  run PATH     - exec ELF at PATH");
            puts("  exit         - exit shell");
        } else if (!strncmp(line, "echo ", 5)) {
            printf("%s\n", line + 5);
        } else if (!strncmp(line, "run ", 4)) {
            const char *path = line + 4;
            int r = exec(path);
            if (r < 0)
                printf("exec failed (%d)\n", r);
            /* if exec succeeds, we never come back here */
        } else if (!strcmp(line, "exit")) {
            puts("bye");
            exit(0);
        } else {
            printf("unknown command: %s\n", line);
        }
    }
}
