#include "ulib.h"  /* or just declare prototypes manually */

int strcmp(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, int n) {
    while (n-- && *a && *b && *a == *b) { a++; b++; }
    if (n < 0) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

void shell_help(void) {
    puts("Commands:");
    puts("  help       - show this help");
    puts("  echo TEXT  - print TEXT");
    puts("  exit       - exit shell");
}

int main(void) {
    char line[256];

    puts("Welcome to tiny user shell");

    while (1) {
        printf("> ");
        int n = readline(line, sizeof(line));
        if (n <= 0) continue;

        if (!strcmp(line, "help")) {
            shell_help();
        } else if (!strncmp(line, "echo ", 5)) {
            printf("%s\n", line + 5);
        } else if (!strcmp(line, "exit")) {
            puts("bye");
            exit(0);
        } else {
            printf("unknown command: %s\n", line);
        }
    }
}
