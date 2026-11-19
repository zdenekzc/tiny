#ifndef TINY_IDE_H
#define TINY_IDE_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#elif _XOPEN_SOURCE < 700
#undef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#ifndef _XOPEN_SOURCE_EXTENDED
#define _XOPEN_SOURCE_EXTENDED 1
#endif

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <locale.h>
#include <ncurses.h>
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
#include <wchar.h>
#include <wctype.h>

#include "editor.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MAX_TABS 16
#define MAX_TREE_ITEMS 2048
#define MAX_TREE_DEPTH 12
#define OUTPUT_MAX_LINES 2000
#define OUTPUT_LINE_MAX 8192
#define RUN_TIMEOUT 15.0
#define KILL_GRACE 1.0
#define SEARCH_MAX 512

/* Extra xterm sequences are mapped below KEY_MAX on both ncurses and the
 * small ToaruOS curses shim. */
#define IDE_KEY_SHIFT_UP    (KEY_MAX - 24)
#define IDE_KEY_SHIFT_DOWN  (KEY_MAX - 23)
#define IDE_KEY_SHIFT_LEFT  (KEY_MAX - 22)
#define IDE_KEY_SHIFT_RIGHT (KEY_MAX - 21)
#define IDE_KEY_SHIFT_HOME  (KEY_MAX - 20)
#define IDE_KEY_SHIFT_END   (KEY_MAX - 19)
#define IDE_KEY_CTRL_LEFT   (KEY_MAX - 18)
#define IDE_KEY_CTRL_RIGHT  (KEY_MAX - 17)
#define IDE_KEY_CTRL_HOME   (KEY_MAX - 16)
#define IDE_KEY_CTRL_END    (KEY_MAX - 15)
#define IDE_KEY_ALT_LEFT    (KEY_MAX - 14)
#define IDE_KEY_ALT_RIGHT   (KEY_MAX - 13)
#define IDE_KEY_ALT_1       (KEY_MAX - 12)
#define IDE_KEY_ALT_2       (KEY_MAX - 11)
#define IDE_KEY_ALT_3       (KEY_MAX - 10)
#define IDE_KEY_CTRL_PGUP   (KEY_MAX - 9)
#define IDE_KEY_CTRL_PGDN   (KEY_MAX - 8)

struct tab {
    struct editor ed;
    char path[PATH_MAX];
    char title[256];
    bool untitled;
};

struct output_line {
    char *text;
    size_t length;
    size_t capacity;
    bool complete;
    struct output_line *next;
};

struct tree_item {
    char path[PATH_MAX];
    char name[256];
    int depth;
    bool is_dir;
    bool expanded;
};

enum focus { FOCUS_EXPLORER, FOCUS_EDITOR, FOCUS_OUTPUT };
enum build_state { BUILD_IDLE, BUILD_COMPILING, BUILD_RUNNING };

struct app {
    char project[PATH_MAX];
    char build_dir[PATH_MAX];
    char build_source[PATH_MAX];
    char exe_path[PATH_MAX];
    char status[512];

    struct tab tabs[MAX_TABS];
    int tab_count;
    int active;

    struct tree_item *tree;
    int tree_count;
    int tree_selected;
    int tree_scroll;
    char **expanded;
    int expanded_count;
    int expanded_capacity;

    struct output_line *output_head;
    struct output_line *output_tail;
    int output_count;
    int output_selected;
    int output_scroll;
    int output_left;
    bool output_follow;
    bool show_output;

    enum focus focus;
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

    char search[SEARCH_MAX];
    char *clipboard;
    size_t clipboard_length;
    bool dragging;
    bool color_enabled;
    bool modal;
    bool should_quit;
};

struct layout {
    int body_y;
    int body_h;
    int explorer_w;
    int editor_x;
    int editor_w;
    int text_x;
    int text_w;
    int output_y;
    int output_h;
    int status_y;
    int footer_y;
};

/* app.c: paths, files, tabs, output and the project tree. */
double now_seconds(void);
void set_status(struct app *app, const char *fmt, ...);
const char *base_name(const char *path);
bool path_join(char *out, size_t size, const char *base, const char *name);
bool make_absolute(char *out, size_t size, const char *base, const char *path);
bool make_existing_absolute(char *out, size_t size, const char *base,
                            const char *path);
bool has_extension(const char *name, const char *extension);
bool is_cpp_file(const char *name);
char *read_whole_file(const char *path, size_t *length);
bool write_all(int fd, const char *data, size_t length);
bool write_file_atomic(const char *path, const char *data, size_t length);

int find_open_tab(struct app *app, const char *path);
bool new_tab(struct app *app);
bool open_file(struct app *app, const char *path);
bool save_tab(struct app *app, int index, const char *path);
void close_tab_index(struct app *app, int index);
bool any_dirty_tab(struct app *app);

void output_clear(struct app *app);
void output_append(struct app *app, const char *text, size_t length);
void output_printf(struct app *app, const char *fmt, ...);
void output_flush(struct app *app);
void output_free(struct app *app);
struct output_line *output_line_at(struct app *app, int index);

void tree_rebuild(struct app *app);
void tree_toggle(struct app *app, int index);
void tree_free(struct app *app);

/* runner.c: asynchronous compiler/program and compiler diagnostics. */
bool start_build(struct app *app, bool then_run);
void stop_child(struct app *app);
void pump_child(struct app *app);
void kill_child_now(struct app *app);
void remove_build_dir(struct app *app);
bool jump_to_diagnostic(struct app *app, const char *line);

/* ui.c: all ncurses drawing and modal dialogs. */
void ui_init(struct app *app);
void ui_shutdown(void);
void compute_layout(struct app *app, struct layout *layout);
void draw_app(struct app *app);
void render_app(struct app *app);
void ui_draw_text(int y, int x, const char *text, int start, int width,
                  attr_t attributes);
int read_key(char text[MB_LEN_MAX + 1], MEVENT *event);
bool prompt_dialog(struct app *app, const char *title, const char *initial,
                   char *result, size_t result_size);
int confirm_dialog(struct app *app, const char *title, const char *message,
                   const char *const *buttons, int count, int selected);
void show_message(struct app *app, const char *title, const char *message);
void show_help(struct app *app);

#endif
