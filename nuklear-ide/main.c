/*
 * nuklear-ide — a deliberately small IDE built on Nuklear.
 *
 * Linux-first backend: GLFW + OpenGL 2.1.  Only Nuklear draws the UI; the
 * editor state is Nuklear's multiline text edit widget, with a small
 * syntax-highlighting overlay for C/C++.  Compilation uses TinyCC (C++ falls
 * back to CXX because TCC has no C++ frontend) and runs asynchronously, so the
 * UI keeps repainting while a program is running.
 *
 * Build:   make
 * Run:     ./nuklear-ide.bin [project-dir | file]
 *          make run        # opens demo/hello.c
 *
 * Public-domain Nuklear headers are in _import/.
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

#ifdef IDE_TOARU
#include <poll.h>
#include <toaru/yutani.h>
#include <toaru/graphics.h>
#include <toaru/kbd.h>
#include <toaru/mouse.h>
#else
#include <GLFW/glfw3.h>
#endif

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_IMPLEMENTATION
#include "_import/nuklear.h"

#ifdef IDE_TOARU
#include "toaru_backend.h"
#else
#define NK_GLFW_GL2_IMPLEMENTATION
#include "_import/nuklear_glfw_gl2.h"
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#ifndef INT_MAX
#define INT_MAX 2147483647
#endif

#define MAX_TABS         16
#define MAX_OUTPUT       (128 * 1024)
#define MAX_TREE_ITEMS   512
#define MAX_DIR_CACHE    256
#define MAX_FILE_SIZE    (4 * 1024 * 1024)
#define TREE_CACHE_TTL   1.5
#define RUN_TIMEOUT      15.0
#define KILL_GRACE       1.0

struct theme {
    struct nk_color text;
    struct nk_color muted;
    struct nk_color accent;
    struct nk_color ok;
    struct nk_color error;
    struct nk_color edit_bg;
    struct nk_color panel_bg;
    struct nk_color window_bg;
    struct nk_color header;
    struct nk_color border;
    struct nk_color button;
    struct nk_color button_hover;
    struct nk_color button_active;
    struct nk_color select;
    struct nk_color select_active;
    struct nk_color edit_cursor;
    struct nk_color scrollbar;
    struct nk_color scrollbar_cursor;
    struct nk_color scrollbar_hover;
    struct nk_color scrollbar_active;
    struct nk_color syntax_keyword;
    struct nk_color syntax_type;
    struct nk_color syntax_string;
    struct nk_color syntax_comment;
    struct nk_color syntax_number;
    struct nk_color syntax_preproc;
};

static struct theme g_theme;

#define COLOR_TEXT (g_theme.text)
#define COLOR_MUTED (g_theme.muted)
#define COLOR_ACCENT (g_theme.accent)
#define COLOR_OK (g_theme.ok)
#define COLOR_ERROR (g_theme.error)
#define COLOR_EDIT_BG (g_theme.edit_bg)
#define COLOR_PANEL_BG (g_theme.panel_bg)

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
    size_t *line_offsets;
    unsigned char *line_block_state;
    int line_count;
    int line_capacity;
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

    bool light_mode;
    bool theme_dirty;
    int jump_line;
    bool jump_pending;
    bool focus_editor;

    bool shortcut_new;
    bool shortcut_open;
    bool shortcut_save;
    bool shortcut_save_as;
    bool shortcut_close;
    bool shortcut_quit;
    bool shortcut_stop;

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

static bool tab_reserve_lines(struct tab *tab, int needed)
{
    if (needed <= tab->line_capacity)
        return true;

    int capacity = tab->line_capacity ? tab->line_capacity : 128;
    while (capacity < needed) {
        if (capacity > (INT_MAX / 2))
            return false;
        capacity *= 2;
    }

    size_t *offsets = malloc((size_t)capacity * sizeof(*offsets));
    unsigned char *blocks = malloc((size_t)capacity);
    if (!offsets || !blocks) {
        free(offsets);
        free(blocks);
        return false;
    }
    if (tab->line_count > 0) {
        memcpy(offsets, tab->line_offsets,
               (size_t)tab->line_count * sizeof(*offsets));
        memcpy(blocks, tab->line_block_state, (size_t)tab->line_count);
    }
    free(tab->line_offsets);
    free(tab->line_block_state);
    tab->line_offsets = offsets;
    tab->line_block_state = blocks;
    tab->line_capacity = capacity;
    return true;
}

/* Build byte offsets for every line and the block-comment state at line
 * start.  Runs once per frame for the active tab; the files are small enough
 * that this keeps the implementation simple and correct after any edit. */
static void tab_rebuild_lines(struct tab *tab)
{
    int length = 0;
    const char *text = tab_text(tab, &length);

    int count = 1;
    for (int i = 0; i < length; ++i) {
        if (text[i] == '\n')
            count++;
    }
    if (!tab_reserve_lines(tab, count)) {
        tab->line_count = 0;
        return;
    }

    int line = 0;
    tab->line_offsets[line++] = 0;
    for (int i = 0; i < length; ++i) {
        if (text[i] == '\n' && line < count)
            tab->line_offsets[line++] = (size_t)i + 1;
    }
    tab->line_count = count;

    bool block_comment = false;
    bool in_string = false;
    char quote = 0;
    bool escaped = false;
    for (int current = 0; current < count; ++current) {
        tab->line_block_state[current] = block_comment ? 1 : 0;
        size_t start = tab->line_offsets[current];
        size_t end = current + 1 < count ?
                     tab->line_offsets[current + 1] - 1 : (size_t)length;
        if (end > (size_t)length)
            end = (size_t)length;

        for (size_t i = start; i < end; ++i) {
            char c = text[i];
            if (block_comment) {
                if (c == '*' && i + 1 < end && text[i + 1] == '/') {
                    block_comment = false;
                    ++i;
                }
            } else if (in_string) {
                if (escaped) {
                    escaped = false;
                } else if (c == '\\') {
                    escaped = true;
                } else if (c == quote) {
                    in_string = false;
                }
            } else if (c == '/' && i + 1 < end && text[i + 1] == '/') {
                break;
            } else if (c == '/' && i + 1 < end && text[i + 1] == '*') {
                block_comment = true;
                ++i;
            } else if (c == '"' || c == '\'') {
                in_string = true;
                quote = c;
                escaped = false;
            }
        }
        in_string = false;
        quote = 0;
        escaped = false;
    }
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
    free(tab->line_offsets);
    free(tab->line_block_state);
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

static bool parse_compiler_location(const char *line, int length,
                                    char *path, size_t path_size,
                                    int *line_number)
{
    int i = 0;
    while (i < length) {
        const char *colon = memchr(line + i, ':', (size_t)(length - i));
        if (!colon)
            break;
        int colon_index = (int)(colon - line);
        int j = colon_index + 1;
        int number = 0;
        int digits = 0;
        while (j < length && isdigit((unsigned char)line[j])) {
            number = number * 10 + (line[j] - '0');
            digits++;
            j++;
        }
        if (digits > 0 && j < length && line[j] == ':') {
            const char *start = line;
            int path_length = colon_index;
            static const char prefix[] = "In file included from ";
            int prefix_length = (int)sizeof(prefix) - 1;
            if (path_length > prefix_length &&
                strncmp(line, prefix, (size_t)prefix_length) == 0) {
                start += prefix_length;
                path_length -= prefix_length;
            } else {
                const char *from = NULL;
                for (int k = 0; k + 5 < colon_index; ++k) {
                    if (strncmp(line + k, "from ", 5) == 0) {
                        from = line + k + 5;
                        break;
                    }
                }
                if (from) {
                    path_length = (int)(line + colon_index - from);
                    start = from;
                }
            }
            while (path_length > 0 && (start[0] == ' ' || start[0] == '\t')) {
                start++;
                path_length--;
            }
            while (path_length > 0 &&
                   (start[path_length - 1] == ' ' ||
                    start[path_length - 1] == '\t'))
                path_length--;
            if (path_length > 0 &&
                (size_t)path_length < path_size) {
                memcpy(path, start, (size_t)path_length);
                path[path_length] = '\0';
                *line_number = number;
                return true;
            }
        }
        i = colon_index + 1;
    }
    return false;
}

static bool line_contains(const char *line, const char *needle)
{
    return line && strstr(line, needle) != NULL;
}

static bool resolve_diagnostic_path(struct app *app, const char *path,
                                    char *out, size_t out_size)
{
    if (is_absolute(path))
        return realpath(path, out) != NULL;
    if (make_existing_absolute(out, out_size, app->project, path))
        return true;

    if (app->active >= 0 && app->active < app->tab_count &&
        !app->tabs[app->active].untitled) {
        char dir[PATH_MAX];
        snprintf(dir, sizeof(dir), "%s", app->tabs[app->active].path);
        char *slash = strrchr(dir, '/');
        if (slash) {
            *slash = '\0';
            if (make_existing_absolute(out, out_size, dir, path))
                return true;
        }
    }
    return false;
}

static void jump_to_diagnostic(struct app *app, const char *path, int line)
{
    char absolute[PATH_MAX];
    if (!resolve_diagnostic_path(app, path, absolute, sizeof(absolute))) {
        set_status(app, "Cannot find %s", path);
        return;
    }
    if (!open_file(app, absolute))
        return;

    app->jump_line = line;
    app->jump_pending = true;
    app->focus_editor = true;
    set_status(app, "Jumping to %s:%d", base_name(absolute), line);
}

static void new_tab(struct app *app)
{
    if (!tab_open(app, "", "", 0))
        return;
    set_status(app, "New file - press F3 to choose a path before building");
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
        set_status(app, "Unsaved changes - confirm quitting");
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

static void child_message(const char *fmt, ...)
{
    char buffer[512];
    va_list ap;
    va_start(ap, fmt);
    int length = vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);
    if (length > 0) {
        size_t n = (size_t)length < sizeof(buffer) ? (size_t)length :
                                                     sizeof(buffer) - 1;
        (void)write(STDERR_FILENO, buffer, n);
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

static bool is_executable(const char *path)
{
    return path && access(path, X_OK) == 0;
}

static bool search_path_for(const char *name, char *out, size_t out_size)
{
    const char *path = getenv("PATH");
    if (!path)
        return false;
    while (*path) {
        const char *colon = strchr(path, ':');
        size_t length = colon ? (size_t)(colon - path) : strlen(path);
        char dir[PATH_MAX];
        if (length == 0)
            snprintf(dir, sizeof(dir), ".");
        else if (length >= sizeof(dir))
            dir[0] = '\0';
        else
            snprintf(dir, sizeof(dir), "%.*s", (int)length, path);
        if (dir[0]) {
            char candidate[PATH_MAX];
            if (path_join(candidate, sizeof(candidate), dir, name) &&
                is_executable(candidate)) {
                char resolved[PATH_MAX];
                if (realpath(candidate, resolved))
                    return snprintf(out, out_size, "%s", resolved) < (int)out_size;
                return snprintf(out, out_size, "%s", candidate) < (int)out_size;
            }
        }
        if (!colon)
            break;
        path = colon + 1;
    }
    return false;
}

/* Locate a host TinyCC.  The repository's setup script leaves a build in
 * _build/download/tinycc; a distro package can also be used via PATH or TCC. */
static bool find_tcc(char *out, size_t out_size)
{
    const char *env = getenv("IDE_TCC");
    if (!env || !env[0])
        env = getenv("TCC");
    if (env && env[0]) {
        if (strchr(env, '/')) {
            char resolved[PATH_MAX];
            if (realpath(env, resolved) && is_executable(resolved))
                return snprintf(out, out_size, "%s", resolved) < (int)out_size;
        } else if (search_path_for(env, out, out_size)) {
            return true;
        }
    }

#ifdef IDE_TOARU
    const char *guest_paths[] = {
        "/extras/tcc/bin/tcc",
        "/share/tcc/bin/tcc",
        "/bin/tcc",
        NULL
    };
    for (int i = 0; guest_paths[i]; ++i) {
        if (is_executable(guest_paths[i])) {
            char resolved[PATH_MAX];
            if (realpath(guest_paths[i], resolved))
                return snprintf(out, out_size, "%s", resolved) < (int)out_size;
            return snprintf(out, out_size, "%s", guest_paths[i]) < (int)out_size;
        }
    }
#else
    char self[PATH_MAX];
    ssize_t self_len = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (self_len > 0) {
        self[self_len] = '\0';
        char *slash = strrchr(self, '/');
        if (slash) {
            *slash = '\0';
            const char *relative[] = {
                "../_build/download/tinycc/tcc",
                "../_build/host-tcc/tcc",
                "tcc",
                NULL
            };
            for (int i = 0; relative[i]; ++i) {
                char candidate[PATH_MAX];
                if (path_join(candidate, sizeof(candidate), self, relative[i]) &&
                    is_executable(candidate)) {
                    char resolved[PATH_MAX];
                    if (realpath(candidate, resolved))
                        return snprintf(out, out_size, "%s", resolved) < (int)out_size;
                }
            }
        }
    }
#endif

    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd))) {
        const char *relative[] = {
            "_build/download/tinycc/tcc",
            "../_build/download/tinycc/tcc",
            NULL
        };
        for (int i = 0; relative[i]; ++i) {
            char candidate[PATH_MAX];
            if (path_join(candidate, sizeof(candidate), cwd, relative[i]) &&
                is_executable(candidate)) {
                char resolved[PATH_MAX];
                if (realpath(candidate, resolved))
                    return snprintf(out, out_size, "%s", resolved) < (int)out_size;
            }
        }
    }

    return search_path_for("tcc", out, out_size);
}

static bool tcc_needs_B(const char *compiler, char *out, size_t out_size)
{
    const char *configured = getenv("TCC_B");
    if (configured && configured[0]) {
        snprintf(out, out_size, "-B%s", configured);
        return true;
    }

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s", compiler);
    char *slash = strrchr(path, '/');
    if (!slash)
        return false;
    *slash = '\0';
    char include[PATH_MAX];
    char runtime[PATH_MAX];
    if (!path_join(include, sizeof(include), path, "include/stddef.h") ||
        !path_join(runtime, sizeof(runtime), path, "libtcc1.a"))
        return false;
    if (access(include, R_OK) == 0 && access(runtime, R_OK) == 0 &&
        strlen(path) + 3 <= out_size) {
        snprintf(out, out_size, "-B%s", path);
        return true;
    }
    return false;
}

static bool start_build(struct app *app, bool then_run)
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
    tab->dirty = tab_dirty_now(tab);
    if (tab->dirty && !save_active(app))
        return false;
    if (tab->untitled || !tab->path[0]) {
        request_save_as(app);
        set_status(app, "Save the file before building it");
        return false;
    }

    bool cpp = is_cpp_file(tab->path);
    char compiler[PATH_MAX];
    char tcc_B[PATH_MAX];
    bool has_tcc_B = false;

    if (cpp) {
        /* TinyCC has no C++ frontend; keep the extension highlighting, but
         * compile C++ with CXX (default: c++). */
        const char *cxx = getenv("CXX");
        snprintf(compiler, sizeof(compiler), "%s",
                 cxx && cxx[0] ? cxx : "c++");
    } else {
        if (!find_tcc(compiler, sizeof(compiler))) {
            output_clear(app);
            app->show_output = true;
            app->build_failed = true;
            output_printf(app,
                "TinyCC (tcc) not found.\n\n"
                "Install tcc, or point the IDE at it:\n"
                "  TCC=/path/to/tcc ./nuklear-ide.bin ...\n\n"
                "In this repository you can build it with:\n"
                "  make -C nuklear-ide tcc\n");
            set_status(app, "TinyCC not found - set TCC or run make tcc");
            return false;
        }
        has_tcc_B = tcc_needs_B(compiler, tcc_B, sizeof(tcc_B));
    }

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
    argv[argc++] = compiler;
    if (!cpp && has_tcc_B)
        argv[argc++] = tcc_B;
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
    set_status(app, then_run ? "Compiling (then running) %s ..." :
                               "Compiling %s ...", base_name(tab->path));

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
    set_status(app, "Stopping...");
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
        output_printf(app, "\n[timeout after %.0f s - sending SIGKILL]\n",
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

static void theme_set(bool light)
{
    if (light) {
        g_theme = (struct theme){
            .text              = nk_rgb(36, 41, 46),
            .muted             = nk_rgb(106, 115, 125),
            .accent            = nk_rgb(9, 105, 218),
            .ok                = nk_rgb(26, 127, 55),
            .error             = nk_rgb(207, 34, 46),
            .edit_bg           = nk_rgb(255, 255, 255),
            .panel_bg          = nk_rgb(246, 248, 250),
            .window_bg         = nk_rgb(250, 251, 252),
            .header            = nk_rgb(240, 243, 246),
            .border            = nk_rgb(208, 215, 222),
            .button            = nk_rgb(240, 243, 246),
            .button_hover      = nk_rgb(230, 235, 240),
            .button_active     = nk_rgb(9, 105, 218),
            .select            = nk_rgb(221, 238, 255),
            .select_active     = nk_rgb(187, 222, 251),
            .edit_cursor       = nk_rgb(9, 105, 218),
            .scrollbar         = nk_rgb(240, 243, 246),
            .scrollbar_cursor  = nk_rgb(170, 180, 190),
            .scrollbar_hover   = nk_rgb(140, 152, 164),
            .scrollbar_active  = nk_rgb(100, 115, 130),
            .syntax_keyword    = nk_rgb(130, 80, 223),
            .syntax_type       = nk_rgb(5, 80, 174),
            .syntax_string     = nk_rgb(22, 130, 93),
            .syntax_comment    = nk_rgb(106, 115, 125),
            .syntax_number     = nk_rgb(185, 110, 20),
            .syntax_preproc    = nk_rgb(130, 80, 223),
        };
    } else {
        g_theme = (struct theme){
            .text              = nk_rgb(216, 222, 233),
            .muted             = nk_rgb(125, 137, 155),
            .accent            = nk_rgb(122, 182, 255),
            .ok                = nk_rgb(126, 211, 160),
            .error             = nk_rgb(255, 121, 121),
            .edit_bg           = nk_rgb(15, 18, 22),
            .panel_bg          = nk_rgb(23, 27, 33),
            .window_bg         = nk_rgb(20, 23, 28),
            .header            = nk_rgb(32, 38, 47),
            .border            = nk_rgb(48, 56, 70),
            .button            = nk_rgb(37, 43, 53),
            .button_hover      = nk_rgb(50, 59, 73),
            .button_active     = nk_rgb(59, 130, 246),
            .select            = nk_rgb(39, 47, 60),
            .select_active     = nk_rgb(45, 96, 160),
            .edit_cursor       = nk_rgb(148, 210, 255),
            .scrollbar         = nk_rgb(23, 27, 33),
            .scrollbar_cursor  = nk_rgb(58, 69, 85),
            .scrollbar_hover   = nk_rgb(78, 92, 112),
            .scrollbar_active  = nk_rgb(96, 112, 136),
            .syntax_keyword    = nk_rgb(198, 120, 221),
            .syntax_type       = nk_rgb(86, 182, 194),
            .syntax_string     = nk_rgb(163, 190, 140),
            .syntax_comment    = nk_rgb(106, 115, 125),
            .syntax_number     = nk_rgb(209, 154, 102),
            .syntax_preproc    = nk_rgb(198, 120, 221),
        };
    }
}

static void style_apply(struct nk_context *ctx, bool light)
{
    theme_set(light);

    struct nk_color table[NK_COLOR_COUNT];
    memset(table, 0, sizeof(table));
    table[NK_COLOR_TEXT]                    = g_theme.text;
    table[NK_COLOR_WINDOW]                  = g_theme.window_bg;
    table[NK_COLOR_HEADER]                  = g_theme.header;
    table[NK_COLOR_BORDER]                  = g_theme.border;
    table[NK_COLOR_BUTTON]                  = g_theme.button;
    table[NK_COLOR_BUTTON_HOVER]            = g_theme.button_hover;
    table[NK_COLOR_BUTTON_ACTIVE]           = g_theme.button_active;
    table[NK_COLOR_TOGGLE]                  = g_theme.button;
    table[NK_COLOR_TOGGLE_HOVER]            = g_theme.button_hover;
    table[NK_COLOR_TOGGLE_CURSOR]           = g_theme.button_active;
    table[NK_COLOR_SELECT]                  = g_theme.select;
    table[NK_COLOR_SELECT_ACTIVE]           = g_theme.select_active;
    table[NK_COLOR_SLIDER]                  = g_theme.button;
    table[NK_COLOR_SLIDER_CURSOR]           = g_theme.button_active;
    table[NK_COLOR_SLIDER_CURSOR_HOVER]     = g_theme.accent;
    table[NK_COLOR_SLIDER_CURSOR_ACTIVE]    = g_theme.accent;
    table[NK_COLOR_PROPERTY]                = g_theme.button;
    table[NK_COLOR_EDIT]                    = g_theme.edit_bg;
    table[NK_COLOR_EDIT_CURSOR]             = g_theme.edit_cursor;
    table[NK_COLOR_COMBO]                   = g_theme.button;
    table[NK_COLOR_CHART]                   = g_theme.button;
    table[NK_COLOR_CHART_COLOR]             = g_theme.accent;
    table[NK_COLOR_CHART_COLOR_HIGHLIGHT]   = g_theme.accent;
    table[NK_COLOR_SCROLLBAR]               = g_theme.scrollbar;
    table[NK_COLOR_SCROLLBAR_CURSOR]        = g_theme.scrollbar_cursor;
    table[NK_COLOR_SCROLLBAR_CURSOR_HOVER]  = g_theme.scrollbar_hover;
    table[NK_COLOR_SCROLLBAR_CURSOR_ACTIVE] = g_theme.scrollbar_active;
    table[NK_COLOR_TAB_HEADER]              = g_theme.header;
    table[NK_COLOR_KNOB]                    = g_theme.button;
    table[NK_COLOR_KNOB_CURSOR]             = g_theme.button_active;
    table[NK_COLOR_KNOB_CURSOR_HOVER]       = g_theme.accent;
    table[NK_COLOR_KNOB_CURSOR_ACTIVE]      = g_theme.accent;
    nk_style_from_table(ctx, table);

    ctx->style.window.padding = nk_vec2(8, 8);
    ctx->style.window.spacing = nk_vec2(6, 6);
    ctx->style.window.scrollbar_size = nk_vec2(10, 10);
    ctx->style.window.group_padding = nk_vec2(6, 6);
    ctx->style.window.border = 1;
    ctx->style.window.border_color = g_theme.window_bg;
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
        if (nk_menu_item_label(ctx, "Open path...         Ctrl+O", NK_TEXT_LEFT)) {
            path_join(app->open_path, sizeof(app->open_path), app->project, "");
            app->show_open = true;
        }
        if (nk_menu_item_label(ctx, "Save                 Ctrl+S", NK_TEXT_LEFT))
            save_active(app);
        if (nk_menu_item_label(ctx, "Save as...           F3", NK_TEXT_LEFT))
            request_save_as(app);
        if (nk_menu_item_label(ctx, "Close tab            F8", NK_TEXT_LEFT))
            request_close_active(app);
        /* A short empty row as a separator; the next item must start a new
         * row, otherwise it is drawn into this 6px-high row and clipped. */
        nk_layout_row_dynamic(ctx, 6, 1);
        nk_spacer(ctx);
        nk_layout_row_dynamic(ctx, 22, 1);
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
    if (nk_menu_begin_label(ctx, "View", NK_TEXT_LEFT, nk_vec2(240, 115))) {
        nk_layout_row_dynamic(ctx, 22, 1);
        if (nk_menu_item_label(ctx, app->show_output ? "Hide output          F9" :
                                                     "Show output          F9",
                               NK_TEXT_LEFT))
            app->show_output = !app->show_output;
        if (nk_menu_item_label(ctx, app->light_mode ? "Light mode           ON" :
                                                     "Light mode           OFF",
                               NK_TEXT_LEFT)) {
            app->light_mode = !app->light_mode;
            app->theme_dirty = true;
        }
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
    float used = 60 + 60 + 100 + 68 + 68 + 62 + 120;
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

    nk_layout_row_push(ctx, 100);
    if (nk_button_label(ctx, "Save as..."))
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

    nk_layout_row_push(ctx, 120);
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
        nk_label_colored(ctx, "No file open - use File > New/Open or the Explorer",
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

static bool list_contains_word(const char *word, int length,
                               const char *const *list)
{
    for (int i = 0; list[i]; ++i) {
        if ((int)strlen(list[i]) == length &&
            strncmp(list[i], word, (size_t)length) == 0)
            return true;
    }
    return false;
}

enum syntax_kind {
    SYNTAX_TEXT,
    SYNTAX_KEYWORD,
    SYNTAX_TYPE,
    SYNTAX_STRING,
    SYNTAX_COMMENT,
    SYNTAX_NUMBER,
    SYNTAX_PREPROC
};

static bool tab_syntax_supported(const struct tab *tab)
{
    const char *name = tab->path[0] ? tab->path : tab->title;
    return has_extension(name, ".c") || has_extension(name, ".h") ||
           has_extension(name, ".cc") || has_extension(name, ".cpp") ||
           has_extension(name, ".cxx") || has_extension(name, ".hpp") ||
           has_extension(name, ".hh");
}

static bool syntax_is_keyword(const char *word, int length, bool *is_type)
{
    static const char *const keywords[] = {
        "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand",
        "bitor", "break", "case", "catch", "class", "co_await",
        "co_return", "co_yield", "compl", "concept", "const", "consteval",
        "constexpr", "constinit", "const_cast", "continue", "decltype",
        "default", "delete", "do", "dynamic_cast", "else", "enum",
        "explicit", "export", "extern", "false", "for", "friend", "goto",
        "if", "inline", "mutable", "namespace", "new", "noexcept", "not",
        "not_eq", "nullptr", "operator", "or", "or_eq", "private",
        "protected", "public", "register", "reinterpret_cast", "requires",
        "restrict", "return", "sizeof", "static", "static_assert",
        "static_cast", "struct", "switch", "template", "this",
        "thread_local", "throw", "true", "try", "typedef", "typeid",
        "typename", "union", "using", "virtual", "volatile", "while",
        "xor", "xor_eq", "_Alignas", "_Alignof", "_Atomic", "_Generic",
        "_Noreturn", "_Static_assert", "_Thread_local", NULL
    };
    static const char *const types[] = {
        "bool", "char", "char8_t", "char16_t", "char32_t", "double",
        "float", "int", "long", "short", "signed", "unsigned", "void",
        "wchar_t", "size_t", "ssize_t", "ptrdiff_t", "uint8_t", "uint16_t",
        "uint32_t", "uint64_t", "int8_t", "int16_t", "int32_t", "int64_t",
        NULL
    };
    if (list_contains_word(word, length, types)) {
        *is_type = true;
        return true;
    }
    *is_type = false;
    return list_contains_word(word, length, keywords);
}

static struct nk_color syntax_color(enum syntax_kind kind)
{
    switch (kind) {
    case SYNTAX_KEYWORD: return g_theme.syntax_keyword;
    case SYNTAX_TYPE:    return g_theme.syntax_type;
    case SYNTAX_STRING:  return g_theme.syntax_string;
    case SYNTAX_COMMENT: return g_theme.syntax_comment;
    case SYNTAX_NUMBER:  return g_theme.syntax_number;
    case SYNTAX_PREPROC: return g_theme.syntax_preproc;
    case SYNTAX_TEXT:
    default:             return g_theme.text;
    }
}

static float syntax_emit(struct nk_command_buffer *canvas,
                         const struct nk_user_font *font, float x, float y,
                         float row_height, const char *text, int length,
                         struct nk_color color, struct nk_rect clip)
{
    if (length <= 0)
        return x;
    float width = font->width(font->userdata, font->height, text, length);
    if (width <= 0.0f)
        return x;
    if (x + width >= clip.x && x <= clip.x + clip.w) {
        struct nk_rect box = nk_rect(x, y, width + 1.0f, row_height);
        nk_draw_text(canvas, box, text, length, font,
                     nk_rgba(0, 0, 0, 0), color);
    }
    return x + width;
}

static bool syntax_ident_start(char c)
{
    return isalpha((unsigned char)c) || c == '_';
}

static bool syntax_ident_char(char c)
{
    return isalnum((unsigned char)c) || c == '_';
}

static float syntax_draw_line(struct nk_command_buffer *canvas,
                              const struct nk_user_font *font,
                              const struct tab *tab, const char *text,
                              int line, float x, float y, float row_height,
                              struct nk_rect clip)
{
    size_t start = tab->line_offsets[line];
    size_t end = line + 1 < tab->line_count ?
                 tab->line_offsets[line + 1] - 1 : (size_t)nk_str_len_char(
                     &tab->edit.string);
    int length = (int)(end - start);
    if (length > 0 && text[start + (size_t)length - 1] == '\r')
        length--;
    if (length <= 0)
        return x;

    int last = 0;
    int pos = 0;
    bool block_comment = tab->line_block_state[line] != 0;

    int first = 0;
    while (first < length && (text[start + first] == ' ' ||
                              text[start + first] == '\t'))
        first++;
    if (!block_comment && first < length && text[start + first] == '#') {
        int j = first + 1;
        while (j < length && syntax_ident_char(text[start + j]))
            j++;
        x = syntax_emit(canvas, font, x, y, row_height,
                        text + start, first, g_theme.text, clip);
        x = syntax_emit(canvas, font, x, y, row_height,
                        text + start + first, j - first,
                        g_theme.syntax_preproc, clip);
        last = pos = j;
    }

    while (pos < length) {
        char c = text[start + pos];
        enum syntax_kind kind = SYNTAX_TEXT;
        int next = pos;

        if (block_comment) {
            int close = -1;
            for (int i = pos; i + 1 < length; ++i) {
                if (text[start + i] == '*' && text[start + i + 1] == '/') {
                    close = i + 2;
                    break;
                }
            }
            if (close < 0) {
                next = length;
            } else {
                next = close;
                block_comment = false;
            }
            kind = SYNTAX_COMMENT;
        } else if (c == '/' && pos + 1 < length &&
                   text[start + pos + 1] == '/') {
            next = length;
            kind = SYNTAX_COMMENT;
        } else if (c == '/' && pos + 1 < length &&
                   text[start + pos + 1] == '*') {
            int close = -1;
            for (int i = pos + 2; i + 1 < length; ++i) {
                if (text[start + i] == '*' && text[start + i + 1] == '/') {
                    close = i + 2;
                    break;
                }
            }
            if (close < 0) {
                next = length;
                block_comment = true;
            } else {
                next = close;
            }
            kind = SYNTAX_COMMENT;
        } else if (c == '"' || c == '\'') {
            char quote = c;
            next = pos + 1;
            bool escaped = false;
            while (next < length) {
                char q = text[start + next++];
                if (escaped) {
                    escaped = false;
                } else if (q == '\\') {
                    escaped = true;
                } else if (q == quote) {
                    break;
                }
            }
            kind = SYNTAX_STRING;
        } else if (isdigit((unsigned char)c) ||
                   (c == '.' && pos + 1 < length &&
                    isdigit((unsigned char)text[start + pos + 1]))) {
            next = pos + 1;
            while (next < length) {
                char n = text[start + next];
                if (!(isalnum((unsigned char)n) || n == '.' || n == '_'))
                    break;
                next++;
            }
            kind = SYNTAX_NUMBER;
        } else if (syntax_ident_start(c)) {
            next = pos + 1;
            while (next < length && syntax_ident_char(text[start + next]))
                next++;
            bool is_type = false;
            if (syntax_is_keyword(text + start + pos, next - pos, &is_type))
                kind = is_type ? SYNTAX_TYPE : SYNTAX_KEYWORD;
        }

        if (next <= pos)
            next = pos + 1;

        if (kind != SYNTAX_TEXT) {
            x = syntax_emit(canvas, font, x, y, row_height,
                            text + start + last, pos - last,
                            g_theme.text, clip);
            x = syntax_emit(canvas, font, x, y, row_height,
                            text + start + pos, next - pos,
                            syntax_color(kind), clip);
            last = next;
        }
        pos = next;
    }

    return syntax_emit(canvas, font, x, y, row_height,
                       text + start + last, length - last,
                       g_theme.text, clip);
}

static void syntax_draw_cursor(struct nk_context *ctx,
                               struct nk_command_buffer *canvas,
                               const struct tab *tab, struct nk_rect clip,
                               float area_x, float area_y, float row_height)
{
    if (!tab->edit.active || tab->line_count <= 0)
        return;

    int line = 1, column = 1;
    cursor_line_column(tab, &line, &column);
    int index = line - 1;
    if (index < 0 || index >= tab->line_count)
        return;

    int length = 0;
    const char *text = tab_text(tab, &length);
    nk_rune unicode = 0;
    int glyph = 0;
    const char *at = nk_str_at_const(&tab->edit.string, tab->edit.cursor,
                                     &unicode, &glyph);
    size_t byte = at ? (size_t)(at - text) : (size_t)length;
    size_t start = tab->line_offsets[index];
    if (byte < start)
        byte = start;
    if (byte > (size_t)length)
        byte = (size_t)length;

    const struct nk_user_font *font = ctx->style.font;
    float width = font->width(font->userdata, font->height,
                              text + start, (int)(byte - start));
    float x = area_x - tab->edit.scrollbar.x + width;
    float y = area_y - tab->edit.scrollbar.y + (float)index * row_height;
    if (y + font->height < clip.y || y > clip.y + clip.h)
        return;
    y += (row_height - font->height) * 0.5f;
    nk_fill_rect(canvas, nk_rect(x, y, ctx->style.edit.cursor_size,
                                 font->height),
                 0, g_theme.edit_cursor);
}

static void syntax_draw(struct app *app, struct nk_context *ctx,
                        struct tab *tab, struct nk_rect bounds)
{
    (void)app;
    if (!tab->line_offsets || tab->line_count <= 0)
        return;

    const struct nk_user_font *font = ctx->style.font;
    float row_height = font->height + ctx->style.edit.row_padding;
    float border = ctx->style.edit.border;
    struct nk_rect area = nk_rect(
        bounds.x + ctx->style.edit.padding.x + border,
        bounds.y + ctx->style.edit.padding.y + border,
        bounds.w - (2.0f * ctx->style.edit.padding.x + 2.0f * border) -
            ctx->style.edit.scrollbar_size.x,
        bounds.h - (2.0f * ctx->style.edit.padding.y + 2.0f * border));
    if (area.w <= 0 || area.h <= 0)
        return;

    int first = (int)(tab->edit.scrollbar.y / row_height);
    if (first < 0)
        first = 0;
    if (first >= tab->line_count)
        return;
    int visible = (int)(area.h / row_height) + 3;
    int last = first + visible;
    if (last > tab->line_count)
        last = tab->line_count;

    int length = 0;
    const char *text = tab_text(tab, &length);
    (void)length;

    struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
    struct nk_rect old_clip = canvas->clip;
    nk_push_scissor(canvas, area);

    for (int line = first; line < last; ++line) {
        float y = area.y - tab->edit.scrollbar.y +
                  (float)line * row_height;
        if (y + row_height < area.y || y > area.y + area.h)
            continue;
        syntax_draw_line(canvas, font, tab, text, line,
                         area.x - tab->edit.scrollbar.x, y,
                         row_height, area);
    }
    syntax_draw_cursor(ctx, canvas, tab, area, area.x, area.y, row_height);

    nk_push_scissor(canvas, old_clip);
}

struct saved_edit_colors {
    struct nk_color text_normal;
    struct nk_color text_hover;
    struct nk_color text_active;
    struct nk_color selected_text_normal;
    struct nk_color selected_text_hover;
    struct nk_color cursor_normal;
    struct nk_color cursor_hover;
    struct nk_color cursor_text_normal;
    struct nk_color cursor_text_hover;
};

static void syntax_hide_edit_text(struct nk_context *ctx,
                                  struct saved_edit_colors *saved)
{
    saved->text_normal = ctx->style.edit.text_normal;
    saved->text_hover = ctx->style.edit.text_hover;
    saved->text_active = ctx->style.edit.text_active;
    saved->selected_text_normal = ctx->style.edit.selected_text_normal;
    saved->selected_text_hover = ctx->style.edit.selected_text_hover;
    saved->cursor_normal = ctx->style.edit.cursor_normal;
    saved->cursor_hover = ctx->style.edit.cursor_hover;
    saved->cursor_text_normal = ctx->style.edit.cursor_text_normal;
    saved->cursor_text_hover = ctx->style.edit.cursor_text_hover;

    struct nk_color transparent = nk_rgba(0, 0, 0, 0);
    ctx->style.edit.text_normal = transparent;
    ctx->style.edit.text_hover = transparent;
    ctx->style.edit.text_active = transparent;
    ctx->style.edit.selected_text_normal = transparent;
    ctx->style.edit.selected_text_hover = transparent;
    ctx->style.edit.cursor_normal = transparent;
    ctx->style.edit.cursor_hover = transparent;
    ctx->style.edit.cursor_text_normal = transparent;
    ctx->style.edit.cursor_text_hover = transparent;
}

static void syntax_restore_edit_text(struct nk_context *ctx,
                                     const struct saved_edit_colors *saved)
{
    ctx->style.edit.text_normal = saved->text_normal;
    ctx->style.edit.text_hover = saved->text_hover;
    ctx->style.edit.text_active = saved->text_active;
    ctx->style.edit.selected_text_normal = saved->selected_text_normal;
    ctx->style.edit.selected_text_hover = saved->selected_text_hover;
    ctx->style.edit.cursor_normal = saved->cursor_normal;
    ctx->style.edit.cursor_hover = saved->cursor_hover;
    ctx->style.edit.cursor_text_normal = saved->cursor_text_normal;
    ctx->style.edit.cursor_text_hover = saved->cursor_text_hover;
}

struct saved_selectable {
    struct nk_style_item normal;
    struct nk_style_item hover;
    struct nk_style_item pressed;
    struct nk_style_item normal_active;
    struct nk_style_item hover_active;
    struct nk_style_item pressed_active;
    struct nk_color text_normal;
    struct nk_color text_hover;
    struct nk_color text_pressed;
    struct nk_color text_normal_active;
    struct nk_color text_hover_active;
    struct nk_color text_pressed_active;
};

static void diagnostic_selectable_push(struct nk_context *ctx,
                                       struct saved_selectable *saved,
                                       struct nk_color text)
{
    struct nk_style_selectable *style = &ctx->style.selectable;
    saved->normal = style->normal;
    saved->hover = style->hover;
    saved->pressed = style->pressed;
    saved->normal_active = style->normal_active;
    saved->hover_active = style->hover_active;
    saved->pressed_active = style->pressed_active;
    saved->text_normal = style->text_normal;
    saved->text_hover = style->text_hover;
    saved->text_pressed = style->text_pressed;
    saved->text_normal_active = style->text_normal_active;
    saved->text_hover_active = style->text_hover_active;
    saved->text_pressed_active = style->text_pressed_active;

    struct nk_color hover = g_theme.select;
    hover.a = 70;
    struct nk_color active = g_theme.select_active;
    active.a = 100;

    style->normal = nk_style_item_color(nk_rgba(0, 0, 0, 0));
    style->hover = nk_style_item_color(hover);
    style->pressed = nk_style_item_color(hover);
    style->normal_active = nk_style_item_color(nk_rgba(0, 0, 0, 0));
    style->hover_active = nk_style_item_color(hover);
    style->pressed_active = nk_style_item_color(active);

    style->text_normal = text;
    style->text_hover = text;
    style->text_pressed = text;
    style->text_normal_active = text;
    style->text_hover_active = text;
    style->text_pressed_active = text;
}

static void diagnostic_selectable_pop(struct nk_context *ctx,
                                      const struct saved_selectable *saved)
{
    struct nk_style_selectable *style = &ctx->style.selectable;
    style->normal = saved->normal;
    style->hover = saved->hover;
    style->pressed = saved->pressed;
    style->normal_active = saved->normal_active;
    style->hover_active = saved->hover_active;
    style->pressed_active = saved->pressed_active;
    style->text_normal = saved->text_normal;
    style->text_hover = saved->text_hover;
    style->text_pressed = saved->text_pressed;
    style->text_normal_active = saved->text_normal_active;
    style->text_hover_active = saved->text_hover_active;
    style->text_pressed_active = saved->text_pressed_active;
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
    nk_label_colored(ctx, "File > New / Open    F5 Run    F7 Build    F9 Output",
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
        bool syntax = tab_syntax_supported(tab);
        nk_layout_row_dynamic(ctx, editor_h, 1);
        if (app->focus_editor) {
            nk_edit_focus(ctx, NK_EDIT_ALWAYS_INSERT_MODE);
            app->focus_editor = false;
        }
        struct nk_rect editor_bounds = nk_widget_bounds(ctx);

        struct saved_edit_colors saved_colors;
        if (syntax)
            syntax_hide_edit_text(ctx, &saved_colors);

        /* NK_EDIT_EDITOR alone starts in view mode (Insert toggles editing).
         * For an IDE we want a click to be enough, so force insert mode. */
        nk_edit_buffer(ctx, NK_EDIT_EDITOR | NK_EDIT_ALWAYS_INSERT_MODE,
                       &tab->edit, nk_filter_default);

        if (syntax)
            syntax_restore_edit_text(ctx, &saved_colors);

        if (syntax || app->jump_pending)
            tab_rebuild_lines(tab);
        tab->dirty = tab_dirty_now(tab);

        if (syntax)
            syntax_draw(app, ctx, tab, editor_bounds);

        if (app->jump_pending) {
            int line = app->jump_line;
            if (line < 1)
                line = 1;
            if (line > tab->line_count)
                line = tab->line_count;
            if (line >= 1 && tab->line_count > 0) {
                int length = 0;
                const char *text = tab_text(tab, &length);
                size_t offset = tab->line_offsets[line - 1];
                if (offset > (size_t)length)
                    offset = (size_t)length;
                int rune = nk_utf_len(text, (int)offset);
                tab->edit.cursor = rune;
                tab->edit.select_start = rune;
                tab->edit.select_end = rune;
                tab->edit.has_preferred_x = 0;
                set_status(app, "Jumped to line %d", line);
            }
            app->jump_pending = false;
        }
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
                size_t shown = len;
                if (shown >= 4096)
                    shown = 4095;
                char linebuf[4096];
                if (shown)
                    memcpy(linebuf, app->output + start, shown);
                linebuf[shown] = '\0';

                char diagnostic_path[PATH_MAX];
                int diagnostic_line = 0;
                bool diagnostic = parse_compiler_location(
                    linebuf, (int)shown, diagnostic_path,
                    sizeof(diagnostic_path), &diagnostic_line);
                struct nk_color line_color = g_theme.text;
                if (line_contains(linebuf, "error"))
                    line_color = g_theme.error;
                else if (line_contains(linebuf, "warning"))
                    line_color = g_theme.syntax_number;

                nk_layout_row_dynamic(ctx, (float)row_height, 1);
                if (diagnostic) {
                    struct saved_selectable saved_selectable;
                    diagnostic_selectable_push(ctx, &saved_selectable,
                                               line_color);
                    nk_bool selected = nk_false;
                    if (nk_selectable_label(ctx, linebuf, NK_TEXT_LEFT,
                                            &selected))
                        jump_to_diagnostic(app, diagnostic_path,
                                           diagnostic_line);
                    diagnostic_selectable_pop(ctx, &saved_selectable);
                } else if (shown) {
                    nk_text_colored(ctx, linebuf, (int)shown, NK_TEXT_LEFT,
                                    line_color);
                } else {
                    nk_label(ctx, "", NK_TEXT_LEFT);
                }
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

    char cursor[64] = "Ln -, Col -";
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
#ifdef IDE_TOARU
            nk_label(ctx, "Nuklear - Yutani - software renderer - guest tcc",
                     NK_TEXT_LEFT);
#else
            nk_label(ctx, "Nuklear - GLFW - OpenGL 2.1 - host cc/c++",
                     NK_TEXT_LEFT);
#endif
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
    if (app->theme_dirty) {
        style_apply(ctx, app->light_mode);
        app->theme_dirty = false;
    }

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

static void clear_shortcuts(struct app *app)
{
    app->shortcut_new = false;
    app->shortcut_open = false;
    app->shortcut_save = false;
    app->shortcut_save_as = false;
    app->shortcut_close = false;
    app->shortcut_quit = false;
    app->shortcut_stop = false;
}

static void shortcuts(struct app *app, struct nk_context *ctx)
{
    bool modal = app->show_open || app->show_save_as ||
                 app->show_about || app->show_quit_confirm ||
                 app->show_close_confirm;
    if (modal) {
        clear_shortcuts(app);
        return;
    }

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

    if (app->shortcut_stop)
        stop_child(app);
    if (app->shortcut_new)
        new_tab(app);
    if (app->shortcut_open) {
        path_join(app->open_path, sizeof(app->open_path), app->project, "");
        app->show_open = true;
    }
    if (app->shortcut_save)
        save_active(app);
    if (app->shortcut_save_as)
        request_save_as(app);
    if (app->shortcut_close)
        request_close_active(app);
    if (app->shortcut_quit)
        request_quit(app);
    clear_shortcuts(app);
}

#ifndef IDE_TOARU
static bool key_edge(GLFWwindow *window, int key, bool *previous)
{
    bool down = glfwGetKey(window, key) == GLFW_PRESS;
    bool edge = down && !*previous;
    *previous = down;
    return edge;
}

static void platform_poll_shortcuts(struct app *app, GLFWwindow *window,
                                    bool *ctrl_n, bool *ctrl_o, bool *ctrl_s,
                                    bool *ctrl_w, bool *ctrl_q, bool *escape)
{
    if (key_edge(window, GLFW_KEY_ESCAPE, escape))
        app->shortcut_stop = true;

    bool ctrl = glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
                glfwGetKey(window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
    bool shift = glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
                 glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;

    if (ctrl && key_edge(window, GLFW_KEY_N, ctrl_n))
        app->shortcut_new = true;
    if (ctrl && key_edge(window, GLFW_KEY_O, ctrl_o))
        app->shortcut_open = true;
    if (ctrl && key_edge(window, GLFW_KEY_S, ctrl_s)) {
        if (shift)
            app->shortcut_save_as = true;
        else
            app->shortcut_save = true;
    }
    if (ctrl && key_edge(window, GLFW_KEY_W, ctrl_w))
        app->shortcut_close = true;
    if (ctrl && key_edge(window, GLFW_KEY_Q, ctrl_q))
        app->shortcut_quit = true;
}
#endif

/* ------------------------------------------------------------------------- */
/* lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

#ifndef IDE_TOARU
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

#endif

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
#ifdef IDE_TOARU
        static const char demo_source[] =
            "#include <stdio.h>\n"
            "\n"
            "int main(void) {\n"
            "    puts(\"Hello from Nuklear IDE on ToaruOS\");\n"
            "    return 0;\n"
            "}\n";
        const char *demo_dir = "/tmp/nuklear-ide-demo";
        char demo_file[PATH_MAX];
        mkdir(demo_dir, 0755);
        path_join(demo_file, sizeof(demo_file), demo_dir, "hello.c");
        if (access(demo_file, F_OK) != 0)
            write_whole_file(demo_file, demo_source, strlen(demo_source));
        snprintf(app->project, sizeof(app->project), "%s", demo_dir);
        if (access(demo_file, R_OK) == 0)
            snprintf(start_file, sizeof(start_file), "%s", demo_file);
#else
        snprintf(app->project, sizeof(app->project), "%s", cwd);
#endif
    }

    char template[] = "/tmp/nuklear-ide-XXXXXX";
    if (!mkdtemp(template)) {
        fprintf(stderr, "mkdtemp: %s\n", strerror(errno));
        return false;
    }
    snprintf(app->build_dir, sizeof(app->build_dir), "%s", template);
    set_status(app, "Ready - open a file from the Explorer");

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

#ifndef IDE_TOARU
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
    style_apply(ctx, app.light_mode);
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
        platform_poll_shortcuts(&app, window, &ctrl_n, &ctrl_o, &ctrl_s,
                                &ctrl_w, &ctrl_q, &escape);
        shortcuts(&app, ctx);

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
            snprintf(title, sizeof(title), "Nuklear IDE - %s%s",
                     tab->untitled ? tab->title : tab->path,
                     tab->dirty ? " *" : "");
        } else {
            snprintf(title, sizeof(title), "Nuklear IDE - %s", app.project);
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

#endif

#ifdef IDE_TOARU
static bool toaru_ctrl(kbd_mod_t mods)
{
    return (mods & (KEY_MOD_LEFT_CTRL | KEY_MOD_RIGHT_CTRL)) != 0;
}

static bool toaru_shift(kbd_mod_t mods)
{
    return (mods & (KEY_MOD_LEFT_SHIFT | KEY_MOD_RIGHT_SHIFT)) != 0;
}

static bool toaru_alt(kbd_mod_t mods)
{
    return (mods & (KEY_MOD_LEFT_ALT | KEY_MOD_RIGHT_ALT)) != 0;
}

static void toaru_clipboard_copy(nk_handle userdata, const char *text, int length)
{
    struct nk_toaru *t = userdata.ptr;
    if (!t || !text || length <= 0)
        return;
    char *copy = malloc((size_t)length + 1);
    if (!copy)
        return;
    memcpy(copy, text, (size_t)length);
    copy[length] = '\0';
    yutani_set_clipboard(t->yctx, copy);
    free(copy);
}

static void toaru_clipboard_paste(nk_handle userdata, struct nk_text_edit *edit)
{
    struct nk_toaru *t = userdata.ptr;
    if (!t)
        return;
    yutani_special_request(t->yctx, t->win, YUTANI_SPECIAL_REQUEST_CLIPBOARD);
    yutani_msg_t *message = yutani_wait_for(t->yctx, YUTANI_MSG_CLIPBOARD);
    if (!message)
        return;
    struct yutani_msg_clipboard *clipboard = (void *)message->data;
    if (clipboard->content[0] == '\002') {
        int size = atoi(&clipboard->content[2]);
        if (size > 0) {
            FILE *file = yutani_open_clipboard(t->yctx);
            if (file) {
                char *text = malloc((size_t)size);
                if (text) {
                    size_t got = fread(text, 1, (size_t)size, file);
                    if (got)
                        nk_textedit_paste(edit, text, (int)got);
                    free(text);
                }
                fclose(file);
            }
        }
    } else if (clipboard->size) {
        nk_textedit_paste(edit, clipboard->content, (int)clipboard->size);
    }
    free(message);
}

static void toaru_feed_key(struct app *app, struct nk_context *ctx,
                           struct nk_toaru *t, struct yutani_msg_key_event *ke)
{
    (void)t;
    kbd_key_t key = ke->event.keycode;
    kbd_mod_t mods = ke->event.modifiers;
    bool down = ke->event.action == KEY_ACTION_DOWN;
    bool ctrl = toaru_ctrl(mods);
    bool shift = toaru_shift(mods);
    bool alt = toaru_alt(mods);

    nk_input_key(ctx, NK_KEY_CTRL, ctrl);
    nk_input_key(ctx, NK_KEY_SHIFT, shift);
    nk_input_key(ctx, NK_KEY_ALT, alt);

    if (key == KEY_ESCAPE) {
        if (down)
            app->shortcut_stop = true;
        return;
    }

    if (ctrl) {
        kbd_key_t lower = key;
        if (lower >= 'A' && lower <= 'Z')
            lower = lower - 'A' + 'a';

        if (down) {
            switch (lower) {
            case 'n': app->shortcut_new = true; break;
            case 'o': app->shortcut_open = true; break;
            case 's': app->shortcut_save_as = shift;
                      app->shortcut_save = !shift; break;
            case 'w': app->shortcut_close = true; break;
            case 'q': app->shortcut_quit = true; break;
            default: break;
            }
        }

        switch (lower) {
        case 'a': nk_input_key(ctx, NK_KEY_TEXT_SELECT_ALL, down); return;
        case 'c': nk_input_key(ctx, NK_KEY_COPY, down); return;
        case 'x': nk_input_key(ctx, NK_KEY_CUT, down); return;
        case 'v': nk_input_key(ctx, NK_KEY_PASTE, down); return;
        case 'z':
            nk_input_key(ctx, shift ? NK_KEY_TEXT_REDO : NK_KEY_TEXT_UNDO,
                         down);
            return;
        case 'y':
        case 'r': nk_input_key(ctx, NK_KEY_TEXT_REDO, down); return;
        default: break;
        }
        if (lower >= 'a' && lower <= 'z')
            return;
    }

    switch (key) {
    case '\n':
    case '\r':
        nk_input_key(ctx, NK_KEY_ENTER, down);
        return;
    case '\t':
        nk_input_key(ctx, NK_KEY_TAB, down);
        return;
    case KEY_BACKSPACE:
        nk_input_key(ctx, NK_KEY_BACKSPACE, down);
        return;
    case KEY_DEL:
        nk_input_key(ctx, NK_KEY_DEL, down);
        return;
    case KEY_ARROW_UP:
        nk_input_key(ctx, NK_KEY_UP, down);
        return;
    case KEY_ARROW_DOWN:
        nk_input_key(ctx, NK_KEY_DOWN, down);
        return;
    case KEY_ARROW_LEFT:
        nk_input_key(ctx, ctrl ? NK_KEY_TEXT_WORD_LEFT : NK_KEY_LEFT, down);
        return;
    case KEY_ARROW_RIGHT:
        nk_input_key(ctx, ctrl ? NK_KEY_TEXT_WORD_RIGHT : NK_KEY_RIGHT, down);
        return;
    case KEY_HOME:
        nk_input_key(ctx, NK_KEY_TEXT_LINE_START, down);
        nk_input_key(ctx, NK_KEY_SCROLL_START, down);
        return;
    case KEY_END:
        nk_input_key(ctx, NK_KEY_TEXT_LINE_END, down);
        nk_input_key(ctx, NK_KEY_SCROLL_END, down);
        return;
    case KEY_F1:  nk_input_key(ctx, NK_KEY_F1, down); return;
    case KEY_F2:  nk_input_key(ctx, NK_KEY_F2, down); return;
    case KEY_F3:  nk_input_key(ctx, NK_KEY_F3, down); return;
    case KEY_F4:  nk_input_key(ctx, NK_KEY_F4, down); return;
    case KEY_F5:  nk_input_key(ctx, NK_KEY_F5, down); return;
    case KEY_F6:  nk_input_key(ctx, NK_KEY_F6, down); return;
    case KEY_F7:  nk_input_key(ctx, NK_KEY_F7, down); return;
    case KEY_F8:  nk_input_key(ctx, NK_KEY_F8, down); return;
    case KEY_F9:  nk_input_key(ctx, NK_KEY_F9, down); return;
    case KEY_F10: nk_input_key(ctx, NK_KEY_F10, down); return;
    case KEY_F11: nk_input_key(ctx, NK_KEY_F11, down); return;
    case KEY_F12: nk_input_key(ctx, NK_KEY_F12, down); return;
    default:
        break;
    }

    if (down && !ctrl && !alt && ke->event.key >= 32 && ke->event.key != 127)
        nk_input_unicode(ctx, (unsigned int)ke->event.key);
}

static void toaru_feed_mouse(struct nk_context *ctx, struct nk_toaru *t,
                             struct yutani_msg_window_mouse_event *me,
                             int x, int y)
{
    t->mouse_x = x;
    t->mouse_y = y;
    t->mouse_buttons = me->buttons & (YUTANI_MOUSE_BUTTON_LEFT |
                                      YUTANI_MOUSE_BUTTON_RIGHT |
                                      YUTANI_MOUSE_BUTTON_MIDDLE);
    nk_input_motion(ctx, x, y);
    nk_input_button(ctx, NK_BUTTON_LEFT, x, y,
                    (t->mouse_buttons & YUTANI_MOUSE_BUTTON_LEFT) != 0);
    nk_input_button(ctx, NK_BUTTON_RIGHT, x, y,
                    (t->mouse_buttons & YUTANI_MOUSE_BUTTON_RIGHT) != 0);
    nk_input_button(ctx, NK_BUTTON_MIDDLE, x, y,
                    (t->mouse_buttons & YUTANI_MOUSE_BUTTON_MIDDLE) != 0);

    if (me->buttons & YUTANI_MOUSE_SCROLL_UP)
        nk_input_scroll(ctx, nk_vec2(0, 1));
    else if (me->buttons & YUTANI_MOUSE_SCROLL_DOWN)
        nk_input_scroll(ctx, nk_vec2(0, -1));
}

static void toaru_handle_message(struct app *app, struct nk_context *ctx,
                                 struct nk_toaru *t, yutani_msg_t *message)
{
    switch (message->type) {
    case YUTANI_MSG_KEY_EVENT: {
        struct yutani_msg_key_event *ke = (void *)message->data;
        if (ke->wid == t->win->wid)
            toaru_feed_key(app, ctx, t, ke);
        break;
    }
    case YUTANI_MSG_WINDOW_MOUSE_EVENT: {
        struct yutani_msg_window_mouse_event *me = (void *)message->data;
        if (me->wid != t->win->wid)
            break;

        /* Title bar / border: move, resize and close are handled by the
         * decoration library; the UI must not see these events. */
        int decor = decor_handle_event(t->yctx, message);
        if (decor == DECOR_CLOSE) {
            app->should_quit = true;
            break;
        }
        if (decor == DECOR_RIGHT) {
            decor_show_default_menu(t->win,
                                    t->win->x + me->new_x,
                                    t->win->y + me->new_y);
            break;
        }

        int x = me->new_x - t->off_x;
        int y = me->new_y - t->off_y;
        if (x >= 0 && y >= 0 && x < t->width && y < t->height) {
            toaru_feed_mouse(ctx, t, me, x, y);
        } else {
            /* Pointer left the content area: release any button Nuklear may
             * still think is held and leave the pointer at its last position. */
            t->mouse_buttons = 0;
            nk_input_motion(ctx, t->mouse_x, t->mouse_y);
            nk_input_button(ctx, NK_BUTTON_LEFT, t->mouse_x, t->mouse_y, 0);
            nk_input_button(ctx, NK_BUTTON_RIGHT, t->mouse_x, t->mouse_y, 0);
            nk_input_button(ctx, NK_BUTTON_MIDDLE, t->mouse_x, t->mouse_y, 0);
        }
        break;
    }
    case YUTANI_MSG_RESIZE_OFFER: {
        struct yutani_msg_window_resize *wr = (void *)message->data;
        if (wr->wid == t->win->wid) {
            struct decor_bounds bounds;
            decor_get_bounds(t->win, &bounds);
            int content_w = wr->width - bounds.width;
            int content_h = wr->height - bounds.height;
            if (content_w < 640 || content_h < 480) {
                uint32_t want_w = content_w < 640
                    ? (uint32_t)(640 + bounds.width) : wr->width;
                uint32_t want_h = content_h < 480
                    ? (uint32_t)(480 + bounds.height) : wr->height;
                yutani_window_resize_offer(t->yctx, t->win, want_w, want_h);
                break;
            }
            yutani_window_resize_accept(t->yctx, t->win, wr->width, wr->height);
            nk_toaru_resize(t, wr->width, wr->height);
            yutani_window_resize_done(t->yctx, t->win);
        }
        break;
    }
    case YUTANI_MSG_WINDOW_FOCUS_CHANGE: {
        struct yutani_msg_window_focus_change *wf = (void *)message->data;
        if (wf->wid == t->win->wid)
            t->win->focused = (uint8_t)wf->focused;
        break;
    }
    case YUTANI_MSG_WINDOW_CLOSE:
    case YUTANI_MSG_SESSION_END:
        app->should_quit = true;
        break;
    default:
        break;
    }
}

int main(int argc, char **argv)
{
    struct app app;
    if (!app_init(&app, argc, argv))
        return 1;

    struct nk_context ctx;
    nk_init_default(&ctx, NULL);
    style_apply(&ctx, app.light_mode);

    struct nk_toaru backend;
    if (!nk_toaru_init(&backend, &ctx, 1400, 900)) {
        fprintf(stderr, "could not create the ToaruOS window\n");
        nk_free(&ctx);
        app_cleanup(&app);
        return 1;
    }
    ctx.clip.copy = toaru_clipboard_copy;
    ctx.clip.paste = toaru_clipboard_paste;
    ctx.clip.userdata = nk_handle_ptr(&backend);

    int yutani_fd = fileno(backend.yctx->sock);
    while (!app.should_quit) {
        double frame_started = now_seconds();
        nk_input_begin(&ctx);
        nk_input_motion(&ctx, backend.mouse_x, backend.mouse_y);
        nk_input_button(&ctx, NK_BUTTON_LEFT, backend.mouse_x,
                        backend.mouse_y,
                        (backend.mouse_buttons & YUTANI_MOUSE_BUTTON_LEFT) != 0);
        nk_input_button(&ctx, NK_BUTTON_RIGHT, backend.mouse_x,
                        backend.mouse_y,
                        (backend.mouse_buttons & YUTANI_MOUSE_BUTTON_RIGHT) != 0);
        nk_input_button(&ctx, NK_BUTTON_MIDDLE, backend.mouse_x,
                        backend.mouse_y,
                        (backend.mouse_buttons & YUTANI_MOUSE_BUTTON_MIDDLE) != 0);

        struct pollfd fds[2];
        fds[0].fd = yutani_fd;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        int nfds = 1;
        if (app.child_active && app.child_fd >= 0) {
            fds[1].fd = app.child_fd;
            fds[1].events = POLLIN;
            fds[1].revents = 0;
            nfds = 2;
        }
        int timeout = app.child_active ? 20 : 100;
        int ready = poll(fds, (nfds_t)nfds, timeout);
        if (ready > 0 && (fds[0].revents & POLLIN)) {
            yutani_msg_t *message;
            while ((message = yutani_poll_async(backend.yctx)) != NULL) {
                toaru_handle_message(&app, &ctx, &backend, message);
                free(message);
            }
        }

        pump_child(&app);
        nk_input_end(&ctx);
        shortcuts(&app, &ctx);

        draw_ui(&app, &ctx, backend.width, backend.height);
        nk_toaru_render(&backend, &ctx);

        /* Mouse motion can arrive much faster than the software renderer;
         * cap the window at ~30 fps so flips do not flood the compositor. */
        double elapsed = now_seconds() - frame_started;
        const double target = 1.0 / 30.0;
        if (elapsed < target)
            usleep((useconds_t)((target - elapsed) * 1000000.0));
    }

    nk_toaru_shutdown(&backend);
    nk_free(&ctx);
    app_cleanup(&app);
    return 0;
}
#endif
