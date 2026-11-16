else if (!strncmp(line, "run ", 4)) {
    const char *path = line + 4;
    int r = exec(path);
    if (r < 0)
        printf("exec failed (%d)\n", r);
}
