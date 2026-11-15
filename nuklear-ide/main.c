/*
 * nuklear-ide — a deliberately small IDE built on Nuklear.
 *
 * Linux-first backend: GLFW + OpenGL 2.1.  Only Nuklear draws the UI; the
 * editor itself is Nuklear's multiline text edit widget.  Compilation and
 * running use the host C/C++ compiler and are asynchronous, so the UI keeps
 * repainting while a program is running.
 *
 * Build:   make
 * Run:     ./nuklear-ide [project-dir | file]
 *          make run        # opens demo/hello.c
 *
 * Public-domain Nuklear headers are in vendor/.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <GLFW/glfw3.h>

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_IMPLEMENTATION
#include "vendor/nuklear.h"

#define NK_GLFW_GL2_IMPLEMENTATION
#include "vendor/nuklear_glfw_gl2.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MAX_TABS         16
#define MAX_OUTPUT       (128 * 1024)
#define MAX_TREE_ITEMS   512
#define MAX_DIR_CACHE    256
#define MAX_FILE_SIZE    (4 * 1024 * 1024)
#define TREE_CACHE_TTL   1.5
#define RUN_TIMEOUT      15.0
#define KILL_GRACE       1.0

#define COLOR_TEXT       nk_rgb(216, 222, 233)
#define COLOR_MUTED      nk_rgb(125, 137, 155)
#define COLOR_ACCENT     nk_rgb(122, 182, 255)
#define COLOR_OK         nk_rgb(126, 211, 160)
#define COLOR_ERROR      nk_rgb(255, 121, 121)
#define COLOR_EDIT_BG    nk_rgb(15, 18, 22)
#define COLOR_PANEL_BG   nk_rgb(23, 27, 33)

enum build_state {
    BUILD_IDLE = 0,
    BUILD_COMPILING,
    BUILD_RUNNING
};

struct tab {
    bool used;
    bool dirty;
    bool untitled;
    char path[PATH_MAX];
    char title[160];
    struct nk_text_edit edit;
    char *saved;
    size_t saved_len;
};

struct file_item {
    char name[256];
    bool is_dir;
};

struct dir_cache {
    char path[PATH_MAX];
    struct file_item *items;
    int count;
    double loaded_at;
};

struct app {
    char project[PATH_MAX];
    char build_dir[PATH_MAX];
    char exe_path[PATH_MAX];
    char status[320];

    struct tab tabs[MAX_TABS];
    int tab_count;
    int active;

    char output[MAX_OUTPUT];
    size_t output_len;
    int output_lines;
    bool show_output;
    enum build_state build_state;
    bool build_failed;
    bool run_after_build;

    pid_t child;
    int child_fd;
    bool child_active;
    bool child_is_run;
    double child_started;
    double stop_at;
    bool stop_requested;

    char open_path[PATH_MAX];
    char save_path[PATH_MAX];
    bool show_open;
    bool show_save_as;
    bool show_about;
    bool show_quit_confirm;
    bool show_close_confirm;
    int close_tab;

    bool should_quit;
};

/* ------------------------------------------------------------------------- */
/* small helpers                                                             */
/* ------------------------------------------------------------------------- */

static void set_status(struct app *app, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(app->status, sizeof(app->status), fmt, ap);
    va_end(ap);
}

static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static bool is_absolute(const char *path)
{
    return path && path[0] == '/';
}

static bool path_join(char *out, size_t out_size, const char *base,
                      const char *name)
{
    if (!out || !out_size || !base || !name)
        return false;
    if (is_absolute(name))
        return snprintf(out, out_size, "%s", name) < (int)out_size;

    size_t n = strlen(base);
    bool slash = n > 0 && base[n - 1] == '/';
    int written = snprintf(out, out_size, "%s%s%s", base,
                           slash ? "" : "/", name);
    return written >= 0 && written < (int)out_size;
}

static bool make_absolute(char *out, size_t out_size, const char *base,
                          const char *path)
{
    if (is_absolute(path))
        return snprintf(out, out_size, "%s", path) < (int)out_size;
    return path_join(out, out_size, base, path);
}

static bool make_existing_absolute(char *out, size_t out_size,
                                   const char *base, const char *path)
{
    char tmp[PATH_MAX];
    if (!make_absolute(tmp, sizeof(tmp), base, path))
        return false;
    return realpath(tmp, out) != NULL;
}

static char *read_whole_file(const char *path, size_t *length)
{
    *length = 0;
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long end = ftell(f);
    if (end < 0 || end > MAX_FILE_SIZE) {
        fclose(f);
        errno = EFBIG;
        return NULL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }

    size_t size = (size_t)end;
    char *data = malloc(size + 1);
    if (!data) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(data, 1, size, f);
    if (got != size && ferror(f)) {
        free(data);
        fclose(f);
        return NULL;
    }
    fclose(f);
    data[got] = '\0';
    *length = got;
    return data;
}

static bool write_whole_file(const char *path, const char *data, size_t length)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    bool ok = fwrite(data, 1, length, f) == length;
    if (fflush(f) != 0)
        ok = false;
    if (fclose(f) != 0)
        ok = false;
    return ok;
}

static bool has_extension(const char *name, const char *ext)
{
    size_t n = strlen(name);
    size_t e = strlen(ext);
    if (n < e)
        return false;
    for (size_t i = 0; i < e; ++i) {
        if (tolower((unsigned char)name[n - e + i]) !=
            tolower((unsigned char)ext[i]))
            return false;
    }
    return true;
}

static bool is_source_file(const char *name)
{
    static const char *extensions[] = {
        ".c", ".h", ".cc", ".cpp", ".cxx", ".hpp", ".hh", ".inl",
        ".s", ".asm", ".inc", ".py", ".sh", ".rs", ".go", ".js",
        ".ts", ".css", ".html", ".json", ".md", ".txt", ".mk",
        ".cmake", ".toml", ".yaml", ".yml", ".ini", ".conf", ".sql",
        ".lua", NULL
    };
    if (strcmp(name, "Makefile") == 0 ||
        strcmp(name, "makefile") == 0 ||
        strcmp(name, "CMakeLists.txt") == 0 ||
        strcmp(name, "meson.build") == 0 ||
        strcmp(name, "Dockerfile") == 0 ||
        strcmp(name, "LICENSE") == 0)
        return true;
    for (int i = 0; extensions[i]; ++i) {
        if (has_extension(name, extensions[i]))
            return true;
    }
    return false;
}

static bool is_cpp_file(const char *name)
{
    return has_extension(name, ".cc") || has_extension(name, ".cpp") ||
           has_extension(name, ".cxx");
}

static bool looks_like_build_dir(const char *name)
{
    static const char *skip[] = {
        ".git", ".hg", ".svn", "node_modules", "__pycache__",
        "build", "target", "dist", "obj", "_build", "_output",
        "_tmp", NULL
    };
    for (int i = 0; skip[i]; ++i) {
        if (strcmp(name, skip[i]) == 0)
            return true;
    }
    return false;
}

/* ------------------------------------------------------------------------- */
/* embedded terminal-ish output buffer                                       */
/* ------------------------------------------------------------------------- */

static int count_text_lines(const char *text, size_t length)
{
    if (!length)
        return 0;
    int lines = 1;
    for (size_t i = 0; i < length; ++i) {
        if (text[i] == '\n')
            lines++;
    }
    return lines;
}

static void output_clear(struct app *app)
{
    app->output_len = 0;
    app->output_lines = 0;
    app->output[0] = '\0';
}

static void output_append(struct app *app, const char *text, size_t length)
{
    if (!length)
        return;

    if (length >= MAX_OUTPUT) {
        text += length - (MAX_OUTPUT - 1);
        length = MAX_OUTPUT - 1;
    }
    if (app->output_len + length >= MAX_OUTPUT) {
        size_t drop = app->output_len + length - (MAX_OUTPUT - 1);
        if (drop > app->output_len)
            drop = app->output_len;
        memmove(app->output, app->output + drop, app->output_len - drop);
        app->output_len -= drop;
    }
    memcpy(app->output + app->output_len, text, length);
    app->output_len += length;
    app->output[app->output_len] = '\0';
    app->output_lines = count_text_lines(app->output, app->output_len);
}

static void output_printf(struct app *app, const char *fmt, ...)
{
    char tmp[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0)
        output_append(app, tmp, (size_t)n < sizeof(tmp) ? (size_t)n :
                                               sizeof(tmp) - 1);
}

/* ------------------------------------------------------------------------- */
/* editor tabs                                                               */
/* ------------------------------------------------------------------------- */

static const char *tab_text(const struct tab *tab, int *length)
{
    const char *text = nk_str_get_const(&tab->edit.string);
    int len = nk_str_len_char(&tab->edit.string);
    if (!text) {
        text = "";
        len = 0;
    }
    if (length)
        *length = len;
    return text;
}

static bool tab_dirty_now(const struct tab *tab)
{
    int len = 0;
    const char *text = tab_text(tab, &len);
    if ((size_t)len != tab->saved_len)
        return true;
    return len > 0 && (!tab->saved || memcmp(text, tab->saved, (size_t)len));
}

static bool tab_open(struct app *app, const char *path, const char *data,
                     size_t length)
{
    if (app->tab_count >= MAX_TABS) {
        set_status(app, "Too many tabs (max %d)", MAX_TABS);
        return false;
    }

    struct tab *tab = &app->tabs[app->tab_count];
    memset(tab, 0, sizeof(*tab));
    nk_textedit_init_default(&tab->edit);
    if (length)
        nk_str_append_text_char(&tab->edit.string, data, (int)length);

    tab->saved = malloc(length ? length : 1);
    if (!tab->saved) {
        nk_textedit_free(&tab->edit);
        memset(tab, 0, sizeof(*tab));
        set_status(app, "Out of memory");
        return false;
    }
    if (length)
        memcpy(tab->saved, data, length);
    tab->saved_len = length;

    tab->untitled = !path || !path[0];
    if (!tab->untitled)
        snprintf(tab->path, sizeof(tab->path), "%s", path);
    const char *title = tab->untitled ? "untitled.c" : base_name(tab->path);
    snprintf(tab->title, sizeof(tab->title), "%s", title);
    tab->used = true;
    tab->dirty = false;

    app->active = app->tab_count++;
    set_status(app, "Opened %s", tab->untitled ? tab->title : tab->path);
    return true;
}

static void tab_free(struct tab *tab)
{
    if (!tab->used)
        return;
    nk_textedit_free(&tab->edit);
    free(tab->saved);
    memset(tab, 0, sizeof(*tab));
}

static int find_open_tab(struct app *app, const char *path)
{
    for (int i = 0; i < app->tab_count; ++i) {
        if (!app->tabs[i].untitled && strcmp(app->tabs[i].path, path) == 0)
            return i;
    }
    return -1;
}

static bool open_file(struct app *app, const char *path)
{
    char absolute[PATH_MAX];
    if (!make_existing_absolute(absolute, sizeof(absolute), app->project, path)) {
        set_status(app, "Cannot open %s: %s", path, strerror(errno));
        return false;
    }

    int existing = find_open_tab(app, absolute);
    if (existing >= 0) {
        app->active = existing;
        set_status(app, "Switched to %s", base_name(absolute));
        return true;
    }

    size_t length = 0;
    char *data = read_whole_file(absolute, &length);
    if (!data) {
        set_status(app, "Cannot read %s: %s", absolute, strerror(errno));
        return false;
    }
    if (memchr(data, '\0', length)) {
        free(data);
        set_status(app, "%s looks like a binary file", absolute);
        return false;
    }
    if (!tab_open(app, absolute, data, length)) {
        free(data);
        return false;
    }
    free(data);
    return true;
}

static void new_tab(struct app *app)
{
    if (!tab_open(app, "", "", 0))
        return;
    set_status(app, "New file — press F3 to choose a path before building");
}

static bool save_tab_to(struct app *app, int index, const char *path)
{
    if (index < 0 || index >= app->tab_count)
        return false;

    struct tab *tab = &app->tabs[index];
    int length = 0;
    const char *text = tab_text(tab, &length);
    if (!write_whole_file(path, text, (size_t)length)) {
        set_status(app, "Save failed: %s: %s", path, strerror(errno));
        app->build_failed = true;
        return false;
    }

    char *copy = malloc((size_t)length ? (size_t)length : 1);
    if (!copy) {
        set_status(app, "Saved, but out of memory while updating the undo baseline");
        return true;
    }
    if (length)
        memcpy(copy, text, (size_t)length);
    free(tab->saved);
    tab->saved = copy;
    tab->saved_len = (size_t)length;
    tab->dirty = false;

    if (!tab->path[0] || strcmp(tab->path, path) != 0) {
        snprintf(tab->path, sizeof(tab->path), "%s", path);
        snprintf(tab->title, sizeof(tab->title), "%s", base_name(path));
        tab->untitled = false;
    }
    set_status(app, "Saved %s", path);
    app->build_failed = false;
    return true;
}

/* Save the active tab.  Returns false when the caller has to ask for a path. */
static bool save_active(struct app *app)
{
    if (app->active < 0 || app->active >= app->tab_count) {
        set_status(app, "No file to save");
        return false;
    }
    struct tab *tab = &app->tabs[app->active];
    if (tab->untitled || !tab->path[0]) {
        if (!tab->path[0])
            path_join(app->save_path, sizeof(app->save_path), app->project,
                      "untitled.c");
        else
            snprintf(app->save_path, sizeof(app->save_path), "%s", tab->path);
        app->show_save_as = true;
        set_status(app, "Choose a path for the new file");
        return false;
    }
    return save_tab_to(app, app->active, tab->path);
}

static void request_save_as(struct app *app)
{
    if (app->active < 0 || app->active >= app->tab_count) {
        set_status(app, "No file to save");
        return;
    }
    struct tab *tab = &app->tabs[app->active];
    if (tab->path[0])
        snprintf(app->save_path, sizeof(app->save_path), "%s", tab->path);
    else
        path_join(app->save_path, sizeof(app->save_path), app->project,
                  "untitled.c");
    app->show_save_as = true;
}

static void close_tab_index(struct app *app, int index)
{
    if (index < 0 || index >= app->tab_count)
        return;

    char title[160];
    snprintf(title, sizeof(title), "%s", app->tabs[index].title);
    tab_free(&app->tabs[index]);
    for (int i = index; i + 1 < app->tab_count; ++i)
        app->tabs[i] = app->tabs[i + 1];
    app->tab_count--;
    memset(&app->tabs[app->tab_count], 0, sizeof(app->tabs[app->tab_count]));

    if (app->tab_count == 0) {
        app->active = -1;
    } else if (app->active > index) {
        app->active--;
    } else if (app->active == index) {
        app->active = index < app->tab_count ? index : app->tab_count - 1;
    }
    set_status(app, "Closed %s", title);
}

static void request_close_active(struct app *app)
{
    if (app->active < 0)
        return;
    struct tab *tab = &app->tabs[app->active];
    tab->dirty = tab_dirty_now(tab);
    if (tab->dirty) {
        app->close_tab = app->active;
        app->show_close_confirm = true;
        return;
    }
    close_tab_index(app, app->active);
}

static bool any_dirty_tab(struct app *app)
{
    for (int i = 0; i < app->tab_count; ++i) {
        app->tabs[i].dirty = tab_dirty_now(&app->tabs[i]);
        if (app->tabs[i].dirty)
            return true;
    }
    return false;
}

static void request_quit(struct app *app)
{
    if (any_dirty_tab(app)) {
        app->show_quit_confirm = true;
        set_status(app, "Unsaved changes — confirm quitting");
    } else {
        app->should_quit = true;
    }
}

/* ------------------------------------------------------------------------- */
/* asynchronous compiler / program runner                                    */
/* ------------------------------------------------------------------------- */

static void close_child_fd(struct app *app)
{
    if (app->child_fd >= 0) {
        close(app->child_fd);
        app->child_fd = -1;
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
        int saved = errno;
        close(pipefd[0]);
        close(pipefd[1]);
        errno = saved;
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
            dprintf(STDERR_FILENO, "chdir(%s): %s\n", app->project,
                    strerror(errno));
            _exit(127);
        }
        execvp(argv[0], argv);
        dprintf(STDERR_FILENO, "exec %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }

    setpgid(pid, pid);
    close(pipefd[1]);
    app->child = pid;
    app->child_fd = pipefd[0];
    app->child_active = true;
    app->child_is_run = is_run;
    app->child_started = now_seconds();
    app->stop_requested = false;
    app->stop_at = 0.0;
    return pid;
}

static bool start_run(struct app *app)
{
    char *argv[] = { app->exe_path, NULL };
    output_printf(app, "\n=== run %s ===\n", app->exe_path);
    pid_t pid = spawn_capture(app, argv, true);
    if (pid < 0) {
        app->build_state = BUILD_IDLE;
        app->build_failed = true;
        output_printf(app, "[could not start program]\n");
        return false;
    }
    app->build_state = BUILD_RUNNING;
    return true;
}

static bool start_build(struct app *app, bool then_run)
{
    if (app->build_state != BUILD_IDLE) {
        set_status(app, "A build or program is already running");
        return false;
    }
    if (app->active < 0 || app->active >= app->tab_count) {
        set_status(app, "Nothing to build — open a source file first");
        return false;
    }

    struct tab *tab = &app->tabs[app->active];
    tab->dirty = tab_dirty_now(tab);
    if (tab->dirty && !save_active(app))
        return false;
    if (tab->untitled || !tab->path[0]) {
        request_save_as(app);
        set_status(app, "Save the file before building it");
        return false;
    }

    bool cpp = is_cpp_file(tab->path);
    const char *compiler = getenv(cpp ? "CXX" : "CC");
    if (!compiler || !compiler[0])
        compiler = cpp ? "c++" : "cc";

    char stem[160];
    snprintf(stem, sizeof(stem), "%s", base_name(tab->path));
    char *dot = strrchr(stem, '.');
    if (dot)
        *dot = '\0';
    for (char *p = stem; *p; ++p) {
        if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_')
            *p = '_';
    }
    if (!stem[0])
        snprintf(stem, sizeof(stem), "program");
    path_join(app->exe_path, sizeof(app->exe_path), app->build_dir, stem);

    char *argv[16];
    int argc = 0;
    argv[argc++] = (char *)compiler;
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
    output_printf(app, "\n\n");

    app->build_failed = false;
    app->run_after_build = then_run;
    app->build_state = BUILD_COMPILING;
    app->show_output = true;
    set_status(app, then_run ? "Compiling (then running) %s …" :
                               "Compiling %s …", base_name(tab->path));

    if (spawn_capture(app, argv, false) < 0) {
        app->build_state = BUILD_IDLE;
        app->build_failed = true;
        output_printf(app, "[could not start compiler]\n");
        return false;
    }
    return true;
}

static void stop_child(struct app *app)
{
    if (!app->child_active)
        return;
    app->stop_requested = true;
    app->stop_at = now_seconds();
    kill(-app->child, SIGTERM);
    kill(app->child, SIGTERM);
    set_status(app, "Stopping…");
}

static void reap_child(struct app *app, int status)
{
    close_child_fd(app);
    app->child_active = false;
    app->child = -1;
    app->stop_requested = false;

    bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (WIFEXITED(status)) {
        output_printf(app, "\n[exit %d]\n", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        output_printf(app, "\n[killed by signal %d]\n", WTERMSIG(status));
    } else {
        output_printf(app, "\n[process ended]\n");
    }

    if (!app->child_is_run) {
        if (ok && app->run_after_build) {
            app->run_after_build = false;
            if (!start_run(app))
                app->build_state = BUILD_IDLE;
        } else {
            app->build_state = BUILD_IDLE;
            app->build_failed = !ok;
            app->run_after_build = false;
            set_status(app, ok ? "Build finished" : "Build failed");
        }
    } else {
        app->build_state = BUILD_IDLE;
        app->build_failed = !ok;
        set_status(app, ok ? "Program finished" : "Program failed");
    }
}

static void pump_child(struct app *app)
{
    if (!app->child_active)
        return;

    char buffer[8192];
    for (;;) {
        ssize_t n = read(app->child_fd, buffer, sizeof(buffer));
        if (n > 0) {
            output_append(app, buffer, (size_t)n);
        } else if (n == 0) {
            break;
        } else if (errno == EINTR) {
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        } else {
            break;
        }
    }

    int status = 0;
    pid_t got = waitpid(app->child, &status, WNOHANG);
    if (got == app->child) {
        /* Drain whatever the child wrote just before exiting. */
        for (;;) {
            ssize_t n = read(app->child_fd, buffer, sizeof(buffer));
            if (n > 0)
                output_append(app, buffer, (size_t)n);
            else
                break;
        }
        reap_child(app, status);
        return;
    }
    if (got < 0 && errno != EINTR)
        return;

    double now = now_seconds();
    if (app->stop_requested && now - app->stop_at > KILL_GRACE) {
        kill(-app->child, SIGKILL);
        kill(app->child, SIGKILL);
        app->stop_requested = false;
    } else if (app->child_is_run && !app->stop_requested &&
               now - app->child_started > RUN_TIMEOUT) {
        output_printf(app, "\n[timeout after %.0f s — sending SIGKILL]\n",
                      RUN_TIMEOUT);
        kill(-app->child, SIGKILL);
        kill(app->child, SIGKILL);
    }
}

static void kill_child_now(struct app *app)
{
    if (!app->child_active)
        return;
    kill(-app->child, SIGKILL);
    kill(app->child, SIGKILL);
    int status;
    waitpid(app->child, &status, 0);
    close_child_fd(app);
    app->child_active = false;
    app->child = -1;
}

/* ------------------------------------------------------------------------- */
/* project file tree (small per-directory cache, refreshed by F5-ish button)  */
/* ------------------------------------------------------------------------- */

static struct dir_cache dir_caches[MAX_DIR_CACHE];
static int dir_cache_count;

static void tree_cache_clear(void)
{
    for (int i = 0; i < dir_cache_count; ++i)
        free(dir_caches[i].items);
    memset(dir_caches, 0, sizeof(dir_caches));
    dir_cache_count = 0;
}

static int compare_items(const void *a, const void *b)
{
    const struct file_item *fa = a;
    const struct file_item *fb = b;
    if (fa->is_dir != fb->is_dir)
        return fa->is_dir ? -1 : 1;
    return strcmp(fa->name, fb->name);
}

static struct dir_cache *find_dir_cache(const char *path, bool *fresh)
{
    double now = now_seconds();
    for (int i = 0; i < dir_cache_count; ++i) {
        if (strcmp(dir_caches[i].path, path) == 0) {
            *fresh = now - dir_caches[i].loaded_at < TREE_CACHE_TTL;
            return &dir_caches[i];
        }
    }
    return NULL;
}

static struct dir_cache *load_dir(const char *path)
{
    bool fresh = false;
    struct dir_cache *cache = find_dir_cache(path, &fresh);
    if (cache && fresh)
        return cache;

    if (!cache) {
        if (dir_cache_count < MAX_DIR_CACHE) {
            cache = &dir_caches[dir_cache_count++];
        } else {
            int oldest = 0;
            for (int i = 1; i < dir_cache_count; ++i) {
                if (dir_caches[i].loaded_at < dir_caches[oldest].loaded_at)
                    oldest = i;
            }
            cache = &dir_caches[oldest];
            free(cache->items);
        }
    } else {
        free(cache->items);
    }
    memset(cache, 0, sizeof(*cache));
    snprintf(cache->path, sizeof(cache->path), "%s", path);
    cache->loaded_at = now_seconds();

    DIR *dir = opendir(path);
    if (!dir)
        return cache;

    struct file_item *items = calloc(MAX_TREE_ITEMS, sizeof(*items));
    if (!items) {
        closedir(dir);
        return cache;
    }

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && count < MAX_TREE_ITEMS) {
        const char *name = entry->d_name;
        if (name[0] == '.' || looks_like_build_dir(name))
            continue;

        char full[PATH_MAX];
        if (!path_join(full, sizeof(full), path, name))
            continue;

        struct stat st;
        if (lstat(full, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            snprintf(items[count].name, sizeof(items[count].name), "%s", name);
            items[count].is_dir = true;
            count++;
        } else if (S_ISREG(st.st_mode) && is_source_file(name)) {
            snprintf(items[count].name, sizeof(items[count].name), "%s", name);
            items[count].is_dir = false;
            count++;
        }
    }
    closedir(dir);

    qsort(items, (size_t)count, sizeof(*items), compare_items);
    cache->items = items;
    cache->count = count;
    return cache;
}

/* ------------------------------------------------------------------------- */
/* UI                                                                        */
/* ------------------------------------------------------------------------- */

static void style_apply(struct nk_context *ctx)
{
    struct nk_color table[NK_COLOR_COUNT];
    memset(table, 0, sizeof(table));
    table[NK_COLOR_TEXT]                    = COLOR_TEXT;
    table[NK_COLOR_WINDOW]                  = nk_rgb(20, 23, 28);
    table[NK_COLOR_HEADER]                  = nk_rgb(32, 38, 47);
    table[NK_COLOR_BORDER]                  = nk_rgb(48, 56, 70);
    table[NK_COLOR_BUTTON]                  = nk_rgb(37, 43, 53);
    table[NK_COLOR_BUTTON_HOVER]            = nk_rgb(50, 59, 73);
    table[NK_COLOR_BUTTON_ACTIVE]           = nk_rgb(59, 130, 246);
    table[NK_COLOR_TOGGLE]                  = nk_rgb(37, 43, 53);
    table[NK_COLOR_TOGGLE_HOVER]            = nk_rgb(50, 59, 73);
    table[NK_COLOR_TOGGLE_CURSOR]           = nk_rgb(59, 130, 246);
    table[NK_COLOR_SELECT]                  = nk_rgb(39, 47, 60);
    table[NK_COLOR_SELECT_ACTIVE]           = nk_rgb(45, 96, 160);
    table[NK_COLOR_SLIDER]                  = nk_rgb(37, 43, 53);
    table[NK_COLOR_SLIDER_CURSOR]           = nk_rgb(59, 130, 246);
    table[NK_COLOR_SLIDER_CURSOR_HOVER]     = nk_rgb(96, 165, 250);
    table[NK_COLOR_SLIDER_CURSOR_ACTIVE]    = nk_rgb(147, 197, 253);
    table[NK_COLOR_PROPERTY]                = nk_rgb(37, 43, 53);
    table[NK_COLOR_EDIT]                    = COLOR_EDIT_BG;
    table[NK_COLOR_EDIT_CURSOR]             = nk_rgb(148, 210, 255);
    table[NK_COLOR_COMBO]                   = nk_rgb(37, 43, 53);
    table[NK_COLOR_CHART]                   = nk_rgb(37, 43, 53);
    table[NK_COLOR_CHART_COLOR]             = nk_rgb(59, 130, 246);
    table[NK_COLOR_CHART_COLOR_HIGHLIGHT]   = nk_rgb(147, 197, 253);
    table[NK_COLOR_SCROLLBAR]               = nk_rgb(23, 27, 33);
    table[NK_COLOR_SCROLLBAR_CURSOR]        = nk_rgb(58, 69, 85);
    table[NK_COLOR_SCROLLBAR_CURSOR_HOVER]  = nk_rgb(78, 92, 112);
    table[NK_COLOR_SCROLLBAR_CURSOR_ACTIVE] = nk_rgb(96, 112, 136);
    table[NK_COLOR_TAB_HEADER]              = nk_rgb(32, 38, 47);
    table[NK_COLOR_KNOB]                    = nk_rgb(37, 43, 53);
    table[NK_COLOR_KNOB_CURSOR]             = nk_rgb(59, 130, 246);
    table[NK_COLOR_KNOB_CURSOR_HOVER]       = nk_rgb(96, 165, 250);
    table[NK_COLOR_KNOB_CURSOR_ACTIVE]      = nk_rgb(147, 197, 253);
    nk_style_from_table(ctx, table);

    ctx->style.window.padding = nk_vec2(8, 8);
    ctx->style.window.spacing = nk_vec2(6, 6);
    ctx->style.window.scrollbar_size = nk_vec2(10, 10);
    ctx->style.window.group_padding = nk_vec2(6, 6);
    ctx->style.window.border = 1;
    ctx->style.window.border_color = nk_rgb(24, 28, 34);
    ctx->style.edit.padding = nk_vec2(8, 6);
    ctx->style.edit.row_padding = 2;
    ctx->style.edit.border = 1;
    ctx->style.edit.scrollbar_size = nk_vec2(10, 10);
    ctx->style.tab.padding = nk_vec2(6, 4);
    ctx->style.selectable.padding = nk_vec2(6, 4);
    ctx->style.button.padding = nk_vec2(7, 4);
    ctx->style.button.border = 1;
    ctx->style.contextual_button.padding = nk_vec2(6, 4);
}

static void menu_bar(struct app *app, struct nk_context *ctx)
{
    nk_menubar_begin(ctx);

    nk_layout_row_begin(ctx, NK_STATIC, 25, 5);
    nk_layout_row_push(ctx, 48);
    if (nk_menu_begin_label(ctx, "File", NK_TEXT_LEFT, nk_vec2(250, 270))) {
        nk_layout_row_dynamic(ctx, 22, 1);
        if (nk_menu_item_label(ctx, "New file             Ctrl+N", NK_TEXT_LEFT))
            new_tab(app);
        if (nk_menu_item_label(ctx, "Open path…           Ctrl+O", NK_TEXT_LEFT)) {
            path_join(app->open_path, sizeof(app->open_path), app->project, "");
            app->show_open = true;
        }
        if (nk_menu_item_label(ctx, "Save                 Ctrl+S", NK_TEXT_LEFT))
            save_active(app);
        if (nk_menu_item_label(ctx, "Save as…             F3", NK_TEXT_LEFT))
            request_save_as(app);
        if (nk_menu_item_label(ctx, "Close tab            F8", NK_TEXT_LEFT))
            request_close_active(app);
        nk_layout_row_dynamic(ctx, 1, 1);
        if (nk_menu_item_label(ctx, "Quit                 Ctrl+Q", NK_TEXT_LEFT))
            request_quit(app);
        nk_menu_end(ctx);
    }

    nk_layout_row_push(ctx, 52);
    if (nk_menu_begin_label(ctx, "Edit", NK_TEXT_LEFT, nk_vec2(210, 145))) {
        nk_layout_row_dynamic(ctx, 22, 1);
        if (app->active >= 0) {
            if (nk_menu_item_label(ctx, "Undo                 Ctrl+Z", NK_TEXT_LEFT))
                nk_textedit_undo(&app->tabs[app->active].edit);
            if (nk_menu_item_label(ctx, "Redo                 Ctrl+Y", NK_TEXT_LEFT))
                nk_textedit_redo(&app->tabs[app->active].edit);
            if (nk_menu_item_label(ctx, "Select all           Ctrl+A", NK_TEXT_LEFT))
                nk_textedit_select_all(&app->tabs[app->active].edit);
        } else {
            nk_widget_disable_begin(ctx);
            nk_menu_item_label(ctx, "Undo                 Ctrl+Z", NK_TEXT_LEFT);
            nk_menu_item_label(ctx, "Redo                 Ctrl+Y", NK_TEXT_LEFT);
            nk_menu_item_label(ctx, "Select all           Ctrl+A", NK_TEXT_LEFT);
            nk_widget_disable_end(ctx);
        }
        nk_menu_end(ctx);
    }

    nk_layout_row_push(ctx, 56);
    if (nk_menu_begin_label(ctx, "Build", NK_TEXT_LEFT, nk_vec2(230, 135))) {
        nk_layout_row_dynamic(ctx, 22, 1);
        if (nk_menu_item_label(ctx, "Build                F7", NK_TEXT_LEFT))
            start_build(app, false);
        if (nk_menu_item_label(ctx, "Run                  F5", NK_TEXT_LEFT))
            start_build(app, true);
        if (nk_menu_item_label(ctx, "Stop                 Esc", NK_TEXT_LEFT))
            stop_child(app);
        if (nk_menu_item_label(ctx, "Clear output", NK_TEXT_LEFT))
            output_clear(app);
        nk_menu_end(ctx);
    }

    nk_layout_row_push(ctx, 52);
    if (nk_menu_begin_label(ctx, "View", NK_TEXT_LEFT, nk_vec2(210, 90))) {
        nk_layout_row_dynamic(ctx, 22, 1);
        if (nk_menu_item_label(ctx, app->show_output ? "Hide output          F9" :
                                                     "Show output          F9",
                               NK_TEXT_LEFT))
            app->show_output = !app->show_output;
        if (nk_menu_item_label(ctx, "Refresh file tree", NK_TEXT_LEFT))
            tree_cache_clear();
        nk_menu_end(ctx);
    }

    nk_layout_row_push(ctx, 48);
    if (nk_menu_begin_label(ctx, "Help", NK_TEXT_LEFT, nk_vec2(180, 80))) {
        nk_layout_row_dynamic(ctx, 22, 1);
        if (nk_menu_item_label(ctx, "About", NK_TEXT_LEFT))
            app->show_about = true;
        nk_menu_end(ctx);
    }

    nk_menubar_end(ctx);
}

static void toolbar(struct app *app, struct nk_context *ctx, float width)
{
    bool busy = app->build_state != BUILD_IDLE;
    float used = 60 + 60 + 82 + 68 + 68 + 62 + 82;
    float gap = ctx->style.window.spacing.x;
    float rest = width - used - gap * 7.0f;
    if (rest < 80)
        rest = 80;

    nk_layout_row_begin(ctx, NK_STATIC, 32, 8);
    nk_layout_row_push(ctx, 60);
    if (nk_button_label(ctx, "New"))
        new_tab(app);

    nk_layout_row_push(ctx, 60);
    if (nk_button_label(ctx, "Save"))
        save_active(app);

    nk_layout_row_push(ctx, 82);
    if (nk_button_label(ctx, "Save as…"))
        request_save_as(app);

    nk_layout_row_push(ctx, 68);
    if (busy) {
        nk_widget_disable_begin(ctx);
        nk_button_label(ctx, "Build");
        nk_widget_disable_end(ctx);
    } else if (nk_button_label(ctx, "Build")) {
        start_build(app, false);
    }

    nk_layout_row_push(ctx, 68);
    if (busy) {
        nk_widget_disable_begin(ctx);
        nk_button_label(ctx, "Run");
        nk_widget_disable_end(ctx);
    } else if (nk_button_label(ctx, "Run")) {
        start_build(app, true);
    }

    nk_layout_row_push(ctx, 62);
    if (!busy) {
        nk_widget_disable_begin(ctx);
        nk_button_label(ctx, "Stop");
        nk_widget_disable_end(ctx);
    } else if (nk_button_label(ctx, "Stop")) {
        stop_child(app);
    }

    nk_layout_row_push(ctx, 82);
    if (nk_button_label(ctx, app->show_output ? "Hide output" : "Output"))
        app->show_output = !app->show_output;

    nk_layout_row_push(ctx, rest);
    nk_label_colored(ctx, app->project, NK_TEXT_LEFT, COLOR_MUTED);
    nk_layout_row_end(ctx);
}

static void draw_tree_dir(struct app *app, struct nk_context *ctx,
                          const char *path, int depth)
{
    if (depth > 12)
        return;
    struct dir_cache *cache = load_dir(path);
    if (!cache)
        return;

    for (int i = 0; i < cache->count; ++i) {
        struct file_item *item = &cache->items[i];
        char full[PATH_MAX];
        if (!path_join(full, sizeof(full), path, item->name))
            continue;

        if (item->is_dir) {
            if (nk_tree_push_hashed(ctx, NK_TREE_NODE, item->name, NK_MINIMIZED,
                                    full, (int)strlen(full), depth)) {
                draw_tree_dir(app, ctx, full, depth + 1);
                nk_tree_pop(ctx);
            }
        } else {
            nk_bool selected = nk_false;
            if (app->active >= 0 && !app->tabs[app->active].untitled)
                selected = strcmp(app->tabs[app->active].path, full) == 0;

            char label[300];
            snprintf(label, sizeof(label), "%s%s", item->name,
                     selected && app->tabs[app->active].dirty ? "  *" : "");
            nk_layout_row_dynamic(ctx, 21, 1);
            if (nk_selectable_label(ctx, label, NK_TEXT_LEFT, &selected))
                open_file(app, full);
        }
    }
}

static void explorer(struct app *app, struct nk_context *ctx)
{
    nk_layout_row_dynamic(ctx, 24, 1);
    nk_label_colored(ctx, "EXPLORER", NK_TEXT_LEFT, COLOR_ACCENT);
    nk_layout_row_dynamic(ctx, 22, 1);
    nk_label_colored(ctx, app->project, NK_TEXT_LEFT, COLOR_MUTED);
    nk_layout_row_dynamic(ctx, 24, 1);
    if (nk_button_label(ctx, "Refresh"))
        tree_cache_clear();
    nk_layout_row_dynamic(ctx, 6, 1);
    nk_label(ctx, "", NK_TEXT_LEFT);
    draw_tree_dir(app, ctx, app->project, 0);
}

static void draw_tabs(struct app *app, struct nk_context *ctx, float width)
{
    if (app->tab_count == 0) {
        nk_layout_row_dynamic(ctx, 30, 1);
        nk_label_colored(ctx, "No file open — use File ▸ New/Open or the Explorer",
                         NK_TEXT_LEFT, COLOR_MUTED);
        return;
    }

    float gap = ctx->style.window.spacing.x;
    float tab_width = (width - gap * (app->tab_count - 1)) /
                      (float)app->tab_count;
    if (tab_width > 220)
        tab_width = 220;
    if (tab_width < 80)
        tab_width = 80;

    nk_layout_row_begin(ctx, NK_STATIC, 28, app->tab_count);
    for (int i = 0; i < app->tab_count; ++i) {
        struct tab *tab = &app->tabs[i];
        char label[220];
        snprintf(label, sizeof(label), "%s%s", tab->title,
                 tab->dirty ? "  *" : "");
        nk_bool selected = i == app->active;
        nk_layout_row_push(ctx, tab_width);
        if (nk_selectable_label(ctx, label, NK_TEXT_LEFT, &selected))
            app->active = i;
    }
    nk_layout_row_end(ctx);
}

static void cursor_line_column(const struct tab *tab, int *line, int *column)
{
    *line = 1;
    *column = 1;
    int length = 0;
    const char *text = tab_text(tab, &length);
    int cursor = tab->edit.cursor;
    int rune = 0;
    int byte = 0;
    while (byte < length && rune < cursor) {
        nk_rune unicode = 0;
        int glyph = nk_utf_decode(text + byte, &unicode, length - byte);
        if (glyph <= 0)
            break;
        if (unicode == '\n') {
            (*line)++;
            *column = 1;
        } else if (unicode != '\r') {
            (*column)++;
        }
        byte += glyph;
        rune++;
    }
}

static void welcome(struct nk_context *ctx)
{
    nk_layout_row_dynamic(ctx, 12, 1);
    nk_label(ctx, "", NK_TEXT_LEFT);
    nk_layout_row_dynamic(ctx, 28, 1);
    nk_label_colored(ctx, "Nuklear IDE", NK_TEXT_LEFT, COLOR_ACCENT);
    nk_layout_row_dynamic(ctx, 22, 1);
    nk_label(ctx, "A small, immediate-mode editor + build/run front end.", NK_TEXT_LEFT);
    nk_layout_row_dynamic(ctx, 22, 1);
    nk_label_colored(ctx, "File ▸ New / Open    F5 Run    F7 Build    F9 Output",
                     NK_TEXT_LEFT, COLOR_MUTED);
    nk_layout_row_dynamic(ctx, 22, 1);
    nk_label(ctx, "Pick a file in the Explorer on the left, or press Ctrl+O.",
             NK_TEXT_LEFT);
}

static void main_pane(struct app *app, struct nk_context *ctx, float height,
                      float width)
{
    float gap = ctx->style.window.spacing.y;
    float padding = ctx->style.window.group_padding.y;
    float inner = height - 2.0f * padding;
    if (inner < 80)
        inner = 80;

    float tabs_h = app->tab_count ? 28.0f : 30.0f;
    float output_h = 0.0f;
    if (app->show_output)
        output_h = inner * 0.30f;
    if (output_h > 220)
        output_h = 220;
    if (output_h > 0 && output_h < 90)
        output_h = 90;

    float rows = output_h > 0 ? 3.0f : 2.0f;
    float editor_h = inner - tabs_h - output_h - gap * (rows - 1.0f);
    if (editor_h < 70) {
        output_h = 0;
        editor_h = inner - tabs_h - gap;
    }
    if (editor_h < 40)
        editor_h = 40;

    draw_tabs(app, ctx, width);

    if (app->active >= 0 && app->active < app->tab_count) {
        struct tab *tab = &app->tabs[app->active];
        nk_layout_row_dynamic(ctx, editor_h, 1);
        /* NK_EDIT_EDITOR alone starts in view mode (Insert toggles editing).
         * For an IDE we want a click to be enough, so force insert mode. */
        nk_edit_buffer(ctx, NK_EDIT_EDITOR | NK_EDIT_ALWAYS_INSERT_MODE,
                       &tab->edit, nk_filter_default);
        tab->dirty = tab_dirty_now(tab);
    } else if (editor_h > 0) {
        nk_layout_row_dynamic(ctx, editor_h, 1);
        if (nk_group_begin(ctx, "welcome", NK_WINDOW_BORDER |
                                             NK_WINDOW_SCROLL_AUTO_HIDE)) {
            welcome(ctx);
            nk_group_end(ctx);
        }
    }

    if (app->show_output && output_h > 0) {
        float copy_w = 52;
        float clear_w = 52;
        nk_layout_row_begin(ctx, NK_STATIC, 22, 3);
        nk_layout_row_push(ctx, width - copy_w - clear_w - gap * 2.0f);
        nk_label_colored(ctx, "OUTPUT", NK_TEXT_LEFT,
                         app->build_failed ? COLOR_ERROR : COLOR_ACCENT);
        nk_layout_row_push(ctx, copy_w);
        if (nk_button_label(ctx, "Copy")) {
            if (app->output_len && ctx->clip.copy) {
                ctx->clip.copy(ctx->clip.userdata, app->output,
                               (int)app->output_len);
                set_status(app, "Output copied to the clipboard");
            }
        }
        nk_layout_row_push(ctx, clear_w);
        if (nk_button_label(ctx, "Clear"))
            output_clear(app);
        nk_layout_row_end(ctx);

        float output_rows_h = output_h - 22.0f - gap;
        if (output_rows_h < 20)
            output_rows_h = 20;
        nk_layout_row_dynamic(ctx, output_rows_h, 1);

        int row_height = (int)(ctx->style.font->height + 2.0f);
        int line_count = app->output_lines > 0 ? app->output_lines : 1;
        struct nk_list_view view;
        if (nk_list_view_begin(ctx, &view, "output",
                               NK_WINDOW_BORDER | NK_WINDOW_SCROLL_AUTO_HIDE,
                               row_height, line_count)) {
            size_t pos = 0;
            int line = 0;
            while (line < view.begin && pos < app->output_len) {
                if (app->output[pos] == '\n')
                    line++;
                pos++;
            }
            for (int i = view.begin; i < view.end; ++i) {
                size_t start = pos;
                while (pos < app->output_len && app->output[pos] != '\n')
                    pos++;
                size_t len = pos - start;
                nk_layout_row_dynamic(ctx, (float)row_height, 1);
                if (len)
                    nk_text(ctx, app->output + start, (int)len, NK_TEXT_LEFT);
                else
                    nk_label(ctx, "", NK_TEXT_LEFT);
                if (pos < app->output_len)
                    pos++;
            }
            nk_list_view_end(&view);
        }
    }
}

static void status_bar(struct app *app, struct nk_context *ctx, float width)
{
    float gap = ctx->style.window.spacing.x;
    float status_w = 280;
    float cursor_w = 120;
    float path_w = width - status_w - cursor_w - gap * 2.0f;
    if (path_w < 120)
        path_w = 120;

    const char *path = "(no file)";
    if (app->active >= 0 && app->active < app->tab_count) {
        struct tab *tab = &app->tabs[app->active];
        path = tab->untitled ? tab->title : tab->path;
    }

    char cursor[64] = "Ln —, Col —";
    if (app->active >= 0 && app->active < app->tab_count) {
        int line, column;
        cursor_line_column(&app->tabs[app->active], &line, &column);
        snprintf(cursor, sizeof(cursor), "Ln %d, Col %d", line, column);
    }

    nk_layout_row_begin(ctx, NK_STATIC, 24, 3);
    nk_layout_row_push(ctx, path_w);
    nk_label_colored(ctx, path, NK_TEXT_LEFT, COLOR_MUTED);
    nk_layout_row_push(ctx, status_w);
    nk_label_colored(ctx, app->status, NK_TEXT_LEFT,
                     app->build_failed ? COLOR_ERROR :
                     app->build_state != BUILD_IDLE ? COLOR_ACCENT : COLOR_TEXT);
    nk_layout_row_push(ctx, cursor_w);
    nk_label_colored(ctx, cursor, NK_TEXT_RIGHT, COLOR_MUTED);
    nk_layout_row_end(ctx);
}

static struct nk_rect popup_rect(int width, int height, float box_w, float box_h)
{
    return nk_rect((width - box_w) * 0.5f, (height - box_h) * 0.5f,
                   box_w, box_h);
}

static void popups(struct app *app, struct nk_context *ctx, int width,
                   int height)
{
    if (app->show_open) {
        struct nk_rect bounds = popup_rect(width, height, 600, 176);
        if (nk_popup_begin(ctx, NK_POPUP_STATIC, "Open file",
                           NK_WINDOW_TITLE | NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR,
                           bounds)) {
            nk_layout_row_dynamic(ctx, 22, 1);
            nk_label(ctx, "Path (absolute, or relative to the project):",
                     NK_TEXT_LEFT);
            nk_layout_row_dynamic(ctx, 28, 1);
            nk_edit_focus(ctx, NK_EDIT_ALWAYS_INSERT_MODE);
            nk_flags ev = nk_edit_string_zero_terminated(ctx,
                NK_EDIT_FIELD | NK_EDIT_SIG_ENTER, app->open_path,
                (int)sizeof(app->open_path), nk_filter_default);
            nk_layout_row_dynamic(ctx, 28, 2);
            bool submit = nk_button_label(ctx, "Open") ||
                          (ev & NK_EDIT_COMMITTED);
            bool cancel = nk_button_label(ctx, "Cancel");
            if (submit) {
                if (open_file(app, app->open_path)) {
                    app->show_open = false;
                    nk_popup_close(ctx);
                }
            } else if (cancel) {
                app->show_open = false;
                nk_popup_close(ctx);
            }
            nk_popup_end(ctx);
        } else {
            app->show_open = false;
        }
    }

    if (app->show_save_as) {
        struct nk_rect bounds = popup_rect(width, height, 600, 176);
        if (nk_popup_begin(ctx, NK_POPUP_STATIC, "Save as",
                           NK_WINDOW_TITLE | NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR,
                           bounds)) {
            nk_layout_row_dynamic(ctx, 22, 1);
            nk_label(ctx, "Path (absolute, or relative to the project):",
                     NK_TEXT_LEFT);
            nk_layout_row_dynamic(ctx, 28, 1);
            nk_edit_focus(ctx, NK_EDIT_ALWAYS_INSERT_MODE);
            nk_flags ev = nk_edit_string_zero_terminated(ctx,
                NK_EDIT_FIELD | NK_EDIT_SIG_ENTER, app->save_path,
                (int)sizeof(app->save_path), nk_filter_default);
            nk_layout_row_dynamic(ctx, 28, 2);
            bool submit = nk_button_label(ctx, "Save") ||
                          (ev & NK_EDIT_COMMITTED);
            bool cancel = nk_button_label(ctx, "Cancel");
            if (submit) {
                char absolute[PATH_MAX];
                bool path_ok = make_absolute(absolute, sizeof(absolute),
                                             app->project, app->save_path);
                if (path_ok && save_tab_to(app, app->active, absolute)) {
                    app->show_save_as = false;
                    nk_popup_close(ctx);
                }
            } else if (cancel) {
                app->show_save_as = false;
                nk_popup_close(ctx);
            }
            nk_popup_end(ctx);
        } else {
            app->show_save_as = false;
        }
    }

    if (app->show_close_confirm) {
        struct nk_rect bounds = popup_rect(width, height, 470, 158);
        if (nk_popup_begin(ctx, NK_POPUP_STATIC, "Unsaved changes",
                           NK_WINDOW_TITLE | NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR,
                           bounds)) {
            char question[260];
            if (app->close_tab >= 0 && app->close_tab < app->tab_count) {
                snprintf(question, sizeof(question),
                         "%s has unsaved changes.",
                         app->tabs[app->close_tab].title);
            } else {
                snprintf(question, sizeof(question), "The file has unsaved changes.");
            }
            nk_layout_row_dynamic(ctx, 24, 1);
            nk_label(ctx, question, NK_TEXT_LEFT);
            nk_layout_row_dynamic(ctx, 22, 1);
            nk_label_colored(ctx, "Save it first if you want to keep the edits.",
                             NK_TEXT_LEFT, COLOR_MUTED);
            nk_layout_row_dynamic(ctx, 28, 2);
            bool discard = nk_button_label(ctx, "Discard");
            bool cancel = nk_button_label(ctx, "Cancel");
            if (discard) {
                close_tab_index(app, app->close_tab);
                app->show_close_confirm = false;
                nk_popup_close(ctx);
            } else if (cancel) {
                app->show_close_confirm = false;
                nk_popup_close(ctx);
            }
            nk_popup_end(ctx);
        } else {
            app->show_close_confirm = false;
        }
    }

    if (app->show_quit_confirm) {
        struct nk_rect bounds = popup_rect(width, height, 470, 158);
        if (nk_popup_begin(ctx, NK_POPUP_STATIC, "Quit",
                           NK_WINDOW_TITLE | NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR,
                           bounds)) {
            nk_layout_row_dynamic(ctx, 24, 1);
            nk_label(ctx, "There are unsaved changes.", NK_TEXT_LEFT);
            nk_layout_row_dynamic(ctx, 22, 1);
            nk_label_colored(ctx, "Quit anyway and lose them?", NK_TEXT_LEFT,
                             COLOR_MUTED);
            nk_layout_row_dynamic(ctx, 28, 2);
            bool discard = nk_button_label(ctx, "Quit without saving");
            bool cancel = nk_button_label(ctx, "Cancel");
            if (discard) {
                app->show_quit_confirm = false;
                app->should_quit = true;
                nk_popup_close(ctx);
            } else if (cancel) {
                app->show_quit_confirm = false;
                nk_popup_close(ctx);
            }
            nk_popup_end(ctx);
        } else {
            app->show_quit_confirm = false;
        }
    }

    if (app->show_about) {
        struct nk_rect bounds = popup_rect(width, height, 480, 190);
        if (nk_popup_begin(ctx, NK_POPUP_STATIC, "About Nuklear IDE",
                           NK_WINDOW_TITLE | NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR,
                           bounds)) {
            nk_layout_row_dynamic(ctx, 26, 1);
            nk_label_colored(ctx, "Nuklear IDE", NK_TEXT_LEFT, COLOR_ACCENT);
            nk_layout_row_dynamic(ctx, 22, 1);
            nk_label(ctx, "Small immediate-mode IDE example.", NK_TEXT_LEFT);
            nk_layout_row_dynamic(ctx, 22, 1);
            nk_label(ctx, "Nuklear • GLFW • OpenGL 2.1 • host cc/c++",
                     NK_TEXT_LEFT);
            nk_layout_row_dynamic(ctx, 22, 1);
            nk_label_colored(ctx, "F5 run  F7 build  F8 close tab  F9 output",
                             NK_TEXT_LEFT, COLOR_MUTED);
            nk_layout_row_dynamic(ctx, 28, 1);
            if (nk_button_label(ctx, "Close")) {
                app->show_about = false;
                nk_popup_close(ctx);
            }
            nk_popup_end(ctx);
        } else {
            app->show_about = false;
        }
    }
}

static void draw_ui(struct app *app, struct nk_context *ctx, int width,
                    int height)
{
    struct nk_rect bounds = nk_rect(0, 0, (float)width, (float)height);
    if (!nk_begin(ctx, "Nuklear IDE", bounds,
                  NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_BORDER)) {
        nk_end(ctx);
        return;
    }

    menu_bar(app, ctx);

    struct nk_rect region = nk_window_get_content_region(ctx);
    float gap = ctx->style.window.spacing.y;
    float body_h = region.h - 32.0f - 24.0f - 2.0f * gap;
    if (body_h < 80)
        body_h = 80;

    toolbar(app, ctx, region.w);

    nk_layout_row_begin(ctx, NK_DYNAMIC, body_h, 2);
    nk_layout_row_push(ctx, 0.235f);
    if (nk_group_begin(ctx, "explorer",
                       NK_WINDOW_BORDER | NK_WINDOW_SCROLL_AUTO_HIDE)) {
        explorer(app, ctx);
        nk_group_end(ctx);
    }

    nk_layout_row_push(ctx, 0.765f);
    if (nk_group_begin(ctx, "main", NK_WINDOW_NO_SCROLLBAR)) {
        float main_w = (region.w - ctx->style.window.spacing.x) * 0.765f -
                       2.0f * ctx->style.window.group_padding.x;
        main_pane(app, ctx, body_h, main_w);
        nk_group_end(ctx);
    }
    nk_layout_row_end(ctx);

    status_bar(app, ctx, region.w);
    popups(app, ctx, width, height);

    nk_end(ctx);
}

/* ------------------------------------------------------------------------- */
/* keyboard shortcuts                                                        */
/* ------------------------------------------------------------------------- */

static bool key_edge(GLFWwindow *window, int key, bool *previous)
{
    bool down = glfwGetKey(window, key) == GLFW_PRESS;
    bool edge = down && !*previous;
    *previous = down;
    return edge;
}

static void shortcuts(struct app *app, struct nk_context *ctx,
                      GLFWwindow *window, bool *ctrl_n, bool *ctrl_o,
                      bool *ctrl_s, bool *ctrl_w, bool *ctrl_q,
                      bool *escape)
{
    bool modal = app->show_open || app->show_save_as ||
                 app->show_about || app->show_quit_confirm ||
                 app->show_close_confirm;
    if (modal)
        return;

    if (nk_input_is_key_pressed(&ctx->input, NK_KEY_F2))
        save_active(app);
    if (nk_input_is_key_pressed(&ctx->input, NK_KEY_F3))
        request_save_as(app);
    if (nk_input_is_key_pressed(&ctx->input, NK_KEY_F4))
        new_tab(app);
    if (nk_input_is_key_pressed(&ctx->input, NK_KEY_F5))
        start_build(app, true);
    if (nk_input_is_key_pressed(&ctx->input, NK_KEY_F6))
        start_build(app, true);
    if (nk_input_is_key_pressed(&ctx->input, NK_KEY_F7))
        start_build(app, false);
    if (nk_input_is_key_pressed(&ctx->input, NK_KEY_F8))
        request_close_active(app);
    if (nk_input_is_key_pressed(&ctx->input, NK_KEY_F9))
        app->show_output = !app->show_output;

    if (key_edge(window, GLFW_KEY_ESCAPE, escape))
        stop_child(app);

    bool ctrl = glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
                glfwGetKey(window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
    bool shift = glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
                 glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;

    if (ctrl && key_edge(window, GLFW_KEY_N, ctrl_n))
        new_tab(app);
    if (ctrl && key_edge(window, GLFW_KEY_O, ctrl_o)) {
        path_join(app->open_path, sizeof(app->open_path), app->project, "");
        app->show_open = true;
    }
    if (ctrl && key_edge(window, GLFW_KEY_S, ctrl_s)) {
        if (shift)
            request_save_as(app);
        else
            save_active(app);
    }
    if (ctrl && key_edge(window, GLFW_KEY_W, ctrl_w))
        request_close_active(app);
    if (ctrl && key_edge(window, GLFW_KEY_Q, ctrl_q))
        request_quit(app);
}

/* ------------------------------------------------------------------------- */
/* lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

static const char *pick_font_path(void)
{
    const char *env = getenv("IDE_FONT");
    if (env && env[0] && access(env, R_OK) == 0)
        return env;

    static const char *candidates[] = {
        "/usr/share/fonts/liberation-mono-fonts/LiberationMono-Regular.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
        "/usr/share/fonts/adwaita-mono-fonts/AdwaitaMono-Regular.ttf",
        "/usr/share/fonts/google-noto-vf/NotoSansMono[wght].ttf",
        NULL
    };
    for (int i = 0; candidates[i]; ++i) {
        if (access(candidates[i], R_OK) == 0)
            return candidates[i];
    }
    return NULL;
}

static void load_font(struct nk_context *ctx)
{
    struct nk_font_atlas *atlas;
    nk_glfw3_font_stash_begin(&atlas);

    struct nk_font_config config = nk_font_config(15.0f);
    config.oversample_h = 1;
    config.oversample_v = 1;
    config.pixel_snap = 1;

    struct nk_font *font = NULL;
    const char *path = pick_font_path();
    if (path)
        font = nk_font_atlas_add_from_file(atlas, path, 15.0f, &config);
    if (!font)
        font = nk_font_atlas_add_default(atlas, 15.0f, &config);

    nk_glfw3_font_stash_end();
    if (font)
        nk_style_set_font(ctx, &font->handle);
}

static bool app_init(struct app *app, int argc, char **argv)
{
    memset(app, 0, sizeof(*app));
    app->active = -1;
    app->show_output = true;
    app->child = -1;
    app->child_fd = -1;

    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) {
        fprintf(stderr, "getcwd: %s\n", strerror(errno));
        return false;
    }

    char start_file[PATH_MAX] = "";
    const char *arg = argc > 1 ? argv[1] : NULL;
    if (arg) {
        struct stat st;
        if (stat(arg, &st) == 0 && S_ISDIR(st.st_mode)) {
            if (!realpath(arg, app->project)) {
                fprintf(stderr, "realpath %s: %s\n", arg, strerror(errno));
                return false;
            }
        } else if (stat(arg, &st) == 0) {
            char absolute[PATH_MAX];
            if (!realpath(arg, absolute)) {
                fprintf(stderr, "realpath %s: %s\n", arg, strerror(errno));
                return false;
            }
            snprintf(app->project, sizeof(app->project), "%s", absolute);
            char *slash = strrchr(app->project, '/');
            if (slash == app->project)
                app->project[1] = '\0';
            else if (slash)
                *slash = '\0';
            snprintf(start_file, sizeof(start_file), "%s", absolute);
        } else {
            fprintf(stderr, "%s: %s\n", arg, strerror(errno));
            return false;
        }
    } else {
        snprintf(app->project, sizeof(app->project), "%s", cwd);
    }

    char template[] = "/tmp/nuklear-ide-XXXXXX";
    if (!mkdtemp(template)) {
        fprintf(stderr, "mkdtemp: %s\n", strerror(errno));
        return false;
    }
    snprintf(app->build_dir, sizeof(app->build_dir), "%s", template);
    set_status(app, "Ready — open a file from the Explorer");

    if (start_file[0])
        open_file(app, start_file);

    return true;
}

static void remove_build_dir(const char *path)
{
    if (!path[0])
        return;
    DIR *dir = opendir(path);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0)
                continue;
            char full[PATH_MAX];
            if (path_join(full, sizeof(full), path, entry->d_name))
                unlink(full);
        }
        closedir(dir);
    }
    rmdir(path);
}

static void app_cleanup(struct app *app)
{
    kill_child_now(app);
    for (int i = 0; i < app->tab_count; ++i)
        tab_free(&app->tabs[i]);
    tree_cache_clear();
    remove_build_dir(app->build_dir);
}

static void glfw_error(int code, const char *description)
{
    fprintf(stderr, "GLFW error %d: %s\n", code, description);
}

int main(int argc, char **argv)
{
    struct app app;
    if (!app_init(&app, argc, argv))
        return 1;

    glfwSetErrorCallback(glfw_error);
    if (!glfwInit()) {
        app_cleanup(&app);
        return 1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    GLFWwindow *window = glfwCreateWindow(1200, 760, "Nuklear IDE", NULL, NULL);
    if (!window) {
        glfwTerminate();
        app_cleanup(&app);
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    glfwSetWindowSizeLimits(window, 760, 480, GLFW_DONT_CARE, GLFW_DONT_CARE);

    struct nk_context *ctx = nk_glfw3_init(window, NK_GLFW3_INSTALL_CALLBACKS);
    style_apply(ctx);
    load_font(ctx);

    bool ctrl_n = false, ctrl_o = false, ctrl_s = false;
    bool ctrl_w = false, ctrl_q = false, escape = false;

    while (!app.should_quit) {
        glfwPollEvents();
        if (glfwWindowShouldClose(window)) {
            glfwSetWindowShouldClose(window, GLFW_FALSE);
            request_quit(&app);
        }

        pump_child(&app);
        nk_glfw3_new_frame();
        shortcuts(&app, ctx, window, &ctrl_n, &ctrl_o, &ctrl_s, &ctrl_w,
                  &ctrl_q, &escape);

        int width = 0, height = 0;
        glfwGetWindowSize(window, &width, &height);
        if (width < 1)
            width = 1;
        if (height < 1)
            height = 1;
        draw_ui(&app, ctx, width, height);

        char title[PATH_MAX + 80];
        if (app.active >= 0 && app.active < app.tab_count) {
            struct tab *tab = &app.tabs[app.active];
            snprintf(title, sizeof(title), "Nuklear IDE — %s%s",
                     tab->untitled ? tab->title : tab->path,
                     tab->dirty ? " *" : "");
        } else {
            snprintf(title, sizeof(title), "Nuklear IDE — %s", app.project);
        }
        glfwSetWindowTitle(window, title);

        int fb_width = 0, fb_height = 0;
        glfwGetFramebufferSize(window, &fb_width, &fb_height);
        glViewport(0, 0, fb_width, fb_height);
        glClearColor(20.0f / 255.0f, 23.0f / 255.0f, 28.0f / 255.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        nk_glfw3_render(NK_ANTI_ALIASING_ON);
        glfwSwapBuffers(window);

        if (app.child_active)
            glfwWaitEventsTimeout(0.02);
        else
            glfwWaitEventsTimeout(0.08);
    }

    nk_glfw3_shutdown();
    glfwDestroyWindow(window);
    glfwTerminate();
    app_cleanup(&app);
    return 0;
}
