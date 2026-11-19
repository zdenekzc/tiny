/* Nonblocking TinyCC/C++ build and run, with clickable compiler locations. */
#include "tiny_ide.h"

static void close_child_fd(struct app *app)
{
    if (app->child_fd >= 0) {
        close(app->child_fd);
        app->child_fd = -1;
    }
}

static void child_message(const char *fmt, ...)
{
    char buffer[512];
    va_list ap;
    va_start(ap, fmt);
    int length = vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);
    if (length > 0) {
        size_t n = (size_t)length < sizeof(buffer) ? (size_t)length : sizeof(buffer) - 1;
        size_t done = 0;
        while (done < n) {
            ssize_t written = write(STDERR_FILENO, buffer + done, n - done);
            if (written < 0 && errno == EINTR)
                continue;
            if (written <= 0)
                break;
            done += (size_t)written;
        }
    }
}

static pid_t spawn_capture(struct app *app, char *const argv[], bool is_run)
{
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        set_status(app, "pipe: %s", strerror(errno));
        return -1;
    }
    int flags = fcntl(pipefd[0], F_GETFL, 0);
    if (flags >= 0)
        fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

    pid_t pid = fork();
    if (pid < 0) {
        int error = errno;
        close(pipefd[0]);
        close(pipefd[1]);
        errno = error;
        set_status(app, "fork: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        setpgid(0, 0);
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            close(devnull);
        }
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        if (chdir(app->project) != 0) {
            child_message("chdir(%s): %s\n", app->project, strerror(errno));
            _exit(127);
        }
        execvp(argv[0], argv);
        child_message("exec %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    setpgid(pid, pid);
    close(pipefd[1]);
    app->child = pid;
    app->child_fd = pipefd[0];
    app->child_active = true;
    app->child_is_run = is_run;
    app->child_started = now_seconds();
    app->stop_at = 0.0;
    app->stop_requested = false;
    return pid;
}

static bool executable(const char *path)
{
    return path && access(path, X_OK) == 0;
}

static bool search_path(const char *name, char *out, size_t size)
{
    const char *path = getenv("PATH");
    if (!path)
        return false;
    while (*path) {
        const char *colon = strchr(path, ':');
        size_t length = colon ? (size_t)(colon - path) : strlen(path);
        char directory[PATH_MAX];
        if (!length)
            snprintf(directory, sizeof(directory), ".");
        else if (length < sizeof(directory))
            snprintf(directory, sizeof(directory), "%.*s", (int)length, path);
        else
            directory[0] = '\0';
        char candidate[PATH_MAX];
        if (directory[0] && path_join(candidate, sizeof(candidate), directory, name) &&
            executable(candidate)) {
            char resolved[PATH_MAX];
            const char *found = realpath(candidate, resolved) ? resolved : candidate;
            if (snprintf(out, size, "%s", found) < (int)size)
                return true;
        }
        if (!colon)
            break;
        path = colon + 1;
    }
    return false;
}

static bool candidate_compiler(const char *candidate, char *out, size_t size)
{
    if (!candidate || !candidate[0])
        return false;
    if (!strchr(candidate, '/'))
        return search_path(candidate, out, size);
    char resolved[PATH_MAX];
    const char *path = realpath(candidate, resolved) ? resolved : candidate;
    return executable(path) && snprintf(out, size, "%s", path) < (int)size;
}

static bool find_tcc(struct app *app, char *out, size_t size)
{
    const char *configured = getenv("IDE_TCC");
    if (!configured || !configured[0])
        configured = getenv("TCC");
    if (candidate_compiler(configured, out, size))
        return true;

    char self[PATH_MAX];
    ssize_t length = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (length > 0) {
        self[length] = '\0';
        char *slash = strrchr(self, '/');
        if (slash) {
            *slash = '\0';
            const char *const relative[] = {
                "../_build/host-tcc/tcc", "../_build/download/tinycc/tcc",
                "../_import/download/tinycc/tcc", "tcc", NULL
            };
            for (int i = 0; relative[i]; ++i) {
                char candidate[PATH_MAX];
                if (path_join(candidate, sizeof(candidate), self, relative[i]) &&
                    candidate_compiler(candidate, out, size))
                    return true;
            }
        }
    }

    const char *const relative[] = {
        "_build/host-tcc/tcc", "../_build/host-tcc/tcc",
        "_build/download/tinycc/tcc", "../_build/download/tinycc/tcc", NULL
    };
    for (int i = 0; relative[i]; ++i) {
        char candidate[PATH_MAX];
        if (path_join(candidate, sizeof(candidate), app->project, relative[i]) &&
            candidate_compiler(candidate, out, size))
            return true;
    }
    return search_path("tcc", out, size);
}

static bool tcc_needs_b(const char *compiler, char *out, size_t size)
{
    const char *configured = getenv("TCC_B");
    if (configured && configured[0]) {
        size_t length = strlen(configured);
        if (length + 3 <= size) {
            out[0] = '-';
            out[1] = 'B';
            memcpy(out + 2, configured, length + 1);
            return true;
        }
        return false;
    }
    char directory[PATH_MAX];
    snprintf(directory, sizeof(directory), "%s", compiler);
    char *slash = strrchr(directory, '/');
    if (!slash)
        return false;
    *slash = '\0';
    char include[PATH_MAX], runtime[PATH_MAX];
    if (!path_join(include, sizeof(include), directory, "include/stddef.h") ||
        !path_join(runtime, sizeof(runtime), directory, "libtcc1.a"))
        return false;
    if (access(include, R_OK) == 0 && access(runtime, R_OK) == 0 &&
        strlen(directory) + 3 <= size) {
        size_t length = strlen(directory);
        out[0] = '-';
        out[1] = 'B';
        memcpy(out + 2, directory, length + 1);
        return true;
    }
    return false;
}

static bool start_run(struct app *app)
{
    char *argv[] = { app->exe_path, NULL };
    output_printf(app, "\n=== run %s ===\n", app->exe_path);
    if (spawn_capture(app, argv, true) < 0) {
        app->build_state = BUILD_IDLE;
        app->build_failed = true;
        output_printf(app, "[could not start program]\n");
        return false;
    }
    app->build_state = BUILD_RUNNING;
    set_status(app, "Running %s - Esc stops it", base_name(app->exe_path));
    return true;
}

bool start_build(struct app *app, bool then_run)
{
    if (app->build_state != BUILD_IDLE) {
        set_status(app, "A build or program is already running");
        return false;
    }
    if (app->active < 0 || app->active >= app->tab_count) {
        set_status(app, "Nothing to build - open a source file first");
        return false;
    }
    struct tab *tab = &app->tabs[app->active];
    if (tab->untitled || !tab->path[0] ||
        tab->ed.revision != tab->ed.saved_revision) {
        set_status(app, "Save the file before building it");
        return false;
    }
    snprintf(app->build_source, sizeof(app->build_source), "%s", tab->path);

    bool cpp = is_cpp_file(tab->path);
    char compiler[PATH_MAX], tcc_b[PATH_MAX];
    bool has_tcc_b = false;
    if (cpp) {
        const char *cxx = getenv("CXX");
        snprintf(compiler, sizeof(compiler), "%s", cxx && cxx[0] ? cxx : "c++");
    } else if (!find_tcc(app, compiler, sizeof(compiler))) {
        output_clear(app);
        app->show_output = true;
        app->build_failed = true;
        output_printf(app,
            "TinyCC (tcc) was not found.\n\n"
            "Install tcc, run 'make -C tiny-ide tcc', or use:\n"
            "  TCC=/path/to/tcc ./tiny-ide/tiny-ide.bin\n");
        set_status(app, "TinyCC not found - run make tcc or set TCC");
        return false;
    } else {
        has_tcc_b = tcc_needs_b(compiler, tcc_b, sizeof(tcc_b));
    }

    char stem[160];
    snprintf(stem, sizeof(stem), "%s", base_name(tab->path));
    char *dot = strrchr(stem, '.');
    if (dot)
        *dot = '\0';
    for (char *p = stem; *p; ++p)
        if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_')
            *p = '_';
    if (!stem[0])
        snprintf(stem, sizeof(stem), "program");
    if (!path_join(app->exe_path, sizeof(app->exe_path), app->build_dir, stem)) {
        set_status(app, "Executable path is too long");
        return false;
    }

    char *argv[16];
    int argc = 0;
    argv[argc++] = compiler;
    if (!cpp && has_tcc_b)
        argv[argc++] = tcc_b;
    argv[argc++] = cpp ? "-std=c++17" : "-std=c11";
    argv[argc++] = "-Wall";
    argv[argc++] = "-Wextra";
    argv[argc++] = "-O2";
    argv[argc++] = "-g";
    argv[argc++] = "-o";
    argv[argc++] = app->exe_path;
    argv[argc++] = tab->path;
    argv[argc] = NULL;

    output_clear(app);
    output_printf(app, "$ %s", compiler);
    for (int i = 1; argv[i]; ++i)
        output_printf(app, " %s", argv[i]);
    output_printf(app, "\n");
    if (cpp)
        output_printf(app, "# TinyCC has no C++ frontend; using CXX (%s)\n", compiler);
    output_printf(app, "\n");
    app->build_failed = false;
    app->run_after_build = then_run;
    app->build_state = BUILD_COMPILING;
    app->show_output = true;
    set_status(app, then_run ? "Compiling, then running %s ..." : "Compiling %s ...",
               base_name(tab->path));
    if (spawn_capture(app, argv, false) < 0) {
        app->build_state = BUILD_IDLE;
        app->build_failed = true;
        output_printf(app, "[could not start compiler]\n");
        return false;
    }
    return true;
}

void stop_child(struct app *app)
{
    if (!app->child_active)
        return;
    app->stop_requested = true;
    app->stop_at = now_seconds();
    kill(-app->child, SIGTERM);
    kill(app->child, SIGTERM);
    set_status(app, "Stopping ...");
}

static void reap_child(struct app *app, int status)
{
    close_child_fd(app);
    app->child_active = false;
    app->child = -1;
    app->stop_requested = false;
    output_flush(app);
    bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (WIFEXITED(status))
        output_printf(app, "\n[exit %d]\n", WEXITSTATUS(status));
    else if (WIFSIGNALED(status))
        output_printf(app, "\n[killed by signal %d]\n", WTERMSIG(status));
    else
        output_printf(app, "\n[process ended]\n");
    output_flush(app);

    if (!app->child_is_run) {
        if (ok && app->run_after_build) {
            app->run_after_build = false;
            if (!start_run(app))
                app->build_state = BUILD_IDLE;
        } else {
            app->build_state = BUILD_IDLE;
            app->build_failed = !ok;
            app->run_after_build = false;
            set_status(app, ok ? "Build finished" : "Build failed - Enter on an error jumps to it");
        }
    } else {
        app->build_state = BUILD_IDLE;
        app->build_failed = !ok;
        set_status(app, ok ? "Program finished" : "Program failed");
    }
}

static void drain_child(struct app *app)
{
    char buffer[8192];
    for (;;) {
        ssize_t got = read(app->child_fd, buffer, sizeof(buffer));
        if (got > 0) {
            output_append(app, buffer, (size_t)got);
        } else if (got < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
}

void pump_child(struct app *app)
{
    if (!app->child_active)
        return;
    drain_child(app);
    int status = 0;
    pid_t result = waitpid(app->child, &status, WNOHANG);
    if (result == app->child) {
        drain_child(app);
        reap_child(app, status);
        return;
    }
    if (result < 0 && errno != EINTR)
        return;

    double now = now_seconds();
    if (app->stop_requested && now - app->stop_at > KILL_GRACE) {
        kill(-app->child, SIGKILL);
        kill(app->child, SIGKILL);
        app->stop_requested = false;
    } else if (app->child_is_run && !app->stop_requested &&
               now - app->child_started > RUN_TIMEOUT) {
        output_printf(app, "\n[timeout after %.0f s - sending SIGKILL]\n", RUN_TIMEOUT);
        kill(-app->child, SIGKILL);
        kill(app->child, SIGKILL);
    }
}

void kill_child_now(struct app *app)
{
    if (!app->child_active)
        return;
    kill(-app->child, SIGKILL);
    kill(app->child, SIGKILL);
    int status;
    while (waitpid(app->child, &status, 0) < 0 && errno == EINTR)
        ;
    close_child_fd(app);
    app->child_active = false;
    app->child = -1;
}

void remove_build_dir(struct app *app)
{
    if (!app->build_dir[0])
        return;
    DIR *directory = opendir(app->build_dir);
    if (directory) {
        struct dirent *entry;
        while ((entry = readdir(directory)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                continue;
            char full[PATH_MAX];
            if (path_join(full, sizeof(full), app->build_dir, entry->d_name))
                unlink(full);
        }
        closedir(directory);
    }
    rmdir(app->build_dir);
}

/* Accept tcc/gcc diagnostics, including "In file included from ...:N:". */
static bool parse_location(const char *line, char *path, size_t size, int *number)
{
    int length = (int)strlen(line);
    for (int i = 0; i < length; ++i) {
        if (line[i] != ':')
            continue;
        int j = i + 1, value = 0, digits = 0;
        while (j < length && isdigit((unsigned char)line[j])) {
            if (value < 10000000)
                value = value * 10 + line[j] - '0';
            digits++;
            j++;
        }
        if (!digits || j >= length || line[j] != ':')
            continue;
        const char *start = line;
        int path_length = i;
        static const char prefix[] = "In file included from ";
        if ((size_t)path_length > sizeof(prefix) - 1 &&
            strncmp(line, prefix, sizeof(prefix) - 1) == 0) {
            start += sizeof(prefix) - 1;
            path_length -= (int)sizeof(prefix) - 1;
        } else {
            const char *from = NULL;
            for (int k = 0; k + 5 < i; ++k)
                if (strncmp(line + k, "from ", 5) == 0) {
                    from = line + k + 5;
                    break;
                }
            if (from) {
                start = from;
                path_length = (int)(line + i - from);
            }
        }
        while (path_length > 0 && isspace((unsigned char)*start)) {
            start++;
            path_length--;
        }
        while (path_length > 0 && isspace((unsigned char)start[path_length - 1]))
            path_length--;
        if (path_length > 0 && (size_t)path_length < size) {
            memcpy(path, start, (size_t)path_length);
            path[path_length] = '\0';
            *number = value;
            return true;
        }
    }
    return false;
}

static bool resolve_diagnostic_path(struct app *app, const char *path,
                                    char *out, size_t size)
{
    if (make_existing_absolute(out, size, app->project, path))
        return true;
    if (app->build_source[0]) {
        char directory[PATH_MAX];
        snprintf(directory, sizeof(directory), "%s", app->build_source);
        char *slash = strrchr(directory, '/');
        if (slash) {
            *slash = '\0';
            if (make_existing_absolute(out, size, directory, path))
                return true;
        }
    }
    return false;
}

bool jump_to_diagnostic(struct app *app, const char *line)
{
    char path[PATH_MAX], absolute[PATH_MAX];
    int number;
    if (!parse_location(line, path, sizeof(path), &number)) {
        set_status(app, "No file:line location on this output line");
        return false;
    }
    if (!resolve_diagnostic_path(app, path, absolute, sizeof(absolute))) {
        set_status(app, "Cannot find diagnostic file %s", path);
        return false;
    }
    if (!open_file(app, absolute))
        return false;
    struct tab *tab = &app->tabs[app->active];
    editor_goto_line(&tab->ed, number, false);
    int row, column;
    editor_position(&tab->ed, tab->ed.cursor, &row, &column);
    tab->ed.top_line = row > 4 ? (size_t)(row - 4) : 0;
    tab->ed.left_column = 0;
    app->focus = FOCUS_EDITOR;
    set_status(app, "Jumped to %s:%d", base_name(absolute), number);
    return true;
}
