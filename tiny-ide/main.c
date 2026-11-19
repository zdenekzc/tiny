/*
 * tiny-ide - a deliberately small ncurses IDE.
 *
 * Linux first; the UI uses the curses subset already available in the
 * ToaruOS mini-commander shim.  The editor model is independent of curses.
 */
#include "tiny_ide.h"

static void usage(const char *program)
{
    printf("Tiny IDE 1.0 - a small ncurses IDE\n"
           "Usage: %s [-b|--nocolor] [-h|--help] [-v|--version] [project-dir | source-file]\n"
           "\n"
           "F1 help, F2 save, F3 save as, F4 new, F5 build+run, F7 build,\n"
           "F8 close tab, F9 output pane, F10 quit.  C uses TinyCC; C++ uses CXX.\n",
           program);
}

static void app_cleanup(struct app *app)
{
    kill_child_now(app);
    for (int i = 0; i < app->tab_count; ++i)
        editor_free(&app->tabs[i].ed);
    output_free(app);
    tree_free(app);
    free(app->clipboard);
    app->clipboard = NULL;
    remove_build_dir(app);
}

static bool app_init(struct app *app, int argc, char **argv)
{
    memset(app, 0, sizeof(*app));
    app->active = -1;
    app->child = -1;
    app->child_fd = -1;
    app->show_output = true;
    app->output_follow = true;
    app->color_enabled = true;
    app->focus = FOCUS_EXPLORER;

    static const struct option options[] = {
        {"nocolor", no_argument, NULL, 'b'},
        {"help", no_argument, NULL, 'h'},
        {"version", no_argument, NULL, 'v'},
        {NULL, 0, NULL, 0}
    };
    int option;
    while ((option = getopt_long(argc, argv, "bhv", options, NULL)) != -1) {
        switch (option) {
        case 'b':
            app->color_enabled = false;
            break;
        case 'h':
            usage(argv[0]);
            exit(0);
        case 'v':
            puts("Tiny IDE 1.0");
            exit(0);
        default:
            usage(argv[0]);
            return false;
        }
    }
    if (optind + 1 < argc) {
        fprintf(stderr, "Expected at most one project directory or source file.\n");
        return false;
    }
    char start_file[PATH_MAX] = "";
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) {
        fprintf(stderr, "getcwd: %s\n", strerror(errno));
        return false;
    }
    if (optind < argc) {
        char absolute[PATH_MAX];
        struct stat st;
        if (stat(argv[optind], &st) != 0 || !realpath(argv[optind], absolute)) {
            fprintf(stderr, "%s: %s\n", argv[optind], strerror(errno));
            return false;
        }
        if (S_ISDIR(st.st_mode)) {
            snprintf(app->project, sizeof(app->project), "%s", absolute);
        } else if (S_ISREG(st.st_mode)) {
            snprintf(start_file, sizeof(start_file), "%s", absolute);
            snprintf(app->project, sizeof(app->project), "%s", absolute);
            char *slash = strrchr(app->project, '/');
            if (slash == app->project)
                app->project[1] = '\0';
            else if (slash)
                *slash = '\0';
        } else {
            fprintf(stderr, "%s: not a regular file or directory\n", argv[optind]);
            return false;
        }
    } else {
        snprintf(app->project, sizeof(app->project), "%s", cwd);
    }

    char temporary[] = "/tmp/tiny-ide-XXXXXX";
    if (!mkdtemp(temporary)) {
        fprintf(stderr, "mkdtemp: %s\n", strerror(errno));
        return false;
    }
    snprintf(app->build_dir, sizeof(app->build_dir), "%s", temporary);
    app->tree = calloc(MAX_TREE_ITEMS, sizeof(*app->tree));
    if (!app->tree) {
        fprintf(stderr, "Out of memory\n");
        return false;
    }
    tree_rebuild(app);
    set_status(app, "Ready - F1 shows help");
    if (start_file[0])
        open_file(app, start_file);
    return true;
}

static void focus_next(struct app *app, int direction)
{
    int count = app->show_output ? 3 : 2;
    int focus = (int)app->focus;
    if (!app->show_output && focus == FOCUS_OUTPUT)
        focus = FOCUS_EDITOR;
    focus = (focus + direction + count) % count;
    app->focus = (enum focus)focus;
}

static void switch_tab(struct app *app, int direction)
{
    if (app->tab_count < 2)
        return;
    app->active = (app->active + direction + app->tab_count) % app->tab_count;
    app->focus = FOCUS_EDITOR;
    app->dragging = false;
}

static bool save_as_action(struct app *app)
{
    if (app->active < 0 || app->active >= app->tab_count) {
        set_status(app, "No file to save");
        return false;
    }
    struct tab *tab = &app->tabs[app->active];
    char initial[PATH_MAX], result[PATH_MAX];
    if (!tab->untitled && tab->path[0])
        snprintf(initial, sizeof(initial), "%s", tab->path);
    else
        path_join(initial, sizeof(initial), app->project, tab->title);
    if (!prompt_dialog(app, " Save file as ", initial, result, sizeof(result)))
        return false;
    return save_tab(app, app->active, result);
}

static bool save_action(struct app *app)
{
    if (app->active < 0 || app->active >= app->tab_count) {
        set_status(app, "No file to save");
        return false;
    }
    struct tab *tab = &app->tabs[app->active];
    if (tab->untitled || !tab->path[0])
        return save_as_action(app);
    return save_tab(app, app->active, tab->path);
}

static void open_action(struct app *app)
{
    char initial[PATH_MAX], result[PATH_MAX];
    path_join(initial, sizeof(initial), app->project, "");
    if (prompt_dialog(app, " Open file ", initial, result, sizeof(result)))
        open_file(app, result);
}

static void close_action(struct app *app)
{
    if (app->active < 0)
        return;
    struct tab *tab = &app->tabs[app->active];
    if (tab->ed.revision != tab->ed.saved_revision) {
        char message[600];
        snprintf(message, sizeof(message), "%s has unsaved changes.\nSave before closing?", tab->title);
        const char *const buttons[] = { "Save", "Discard", "Cancel" };
        int answer = confirm_dialog(app, " Unsaved changes ", message, buttons, 3, 0);
        if (answer < 0 || answer == 2)
            return;
        if (answer == 0 && !save_action(app))
            return;
    }
    close_tab_index(app, app->active);
}

static void quit_action(struct app *app)
{
    if (!any_dirty_tab(app)) {
        app->should_quit = true;
        return;
    }
    const char *const buttons[] = { "Save all", "Discard all", "Cancel" };
    int answer = confirm_dialog(app, " Quit Tiny IDE ",
                                "There are unsaved changes.\nSave all files before quitting?",
                                buttons, 3, 0);
    if (answer < 0 || answer == 2) {
        set_status(app, "Quit cancelled");
        return;
    }
    if (answer == 0) {
        int previous = app->active;
        for (int i = 0; i < app->tab_count; ++i) {
            app->active = i;
            if (app->tabs[i].ed.revision != app->tabs[i].ed.saved_revision ||
                app->tabs[i].untitled)
                if (!save_action(app)) {
                    app->active = previous >= 0 && previous < app->tab_count ? previous : 0;
                    set_status(app, "Quit cancelled - file was not saved");
                    return;
                }
        }
        app->active = previous;
    }
    app->should_quit = true;
}

static void build_action(struct app *app, bool run)
{
    if (app->active < 0 || app->active >= app->tab_count) {
        set_status(app, "Nothing to build - open a source file first");
        return;
    }
    struct tab *tab = &app->tabs[app->active];
    if ((tab->untitled || tab->ed.revision != tab->ed.saved_revision) &&
        !save_action(app))
        return;
    start_build(app, run);
}

static void edit_error(struct app *app, int result)
{
    if (result < 0)
        set_status(app, "Edit failed: %s", strerror(errno));
}

static void copy_selection(struct app *app)
{
    if (app->active < 0)
        return;
    size_t length = 0;
    char *copy = editor_copy_selection(&app->tabs[app->active].ed, &length);
    if (!copy) {
        set_status(app, "Nothing selected");
        return;
    }
    free(app->clipboard);
    app->clipboard = copy;
    app->clipboard_length = length;
    set_status(app, "Copied %zu bytes", length);
}

static void find_action(struct app *app)
{
    if (app->active < 0)
        return;
    char value[SEARCH_MAX];
    if (!prompt_dialog(app, " Find text (case-sensitive) ", app->search,
                       value, sizeof(value)))
        return;
    snprintf(app->search, sizeof(app->search), "%s", value);
    if (!app->search[0])
        return;
    if (editor_find(&app->tabs[app->active].ed, app->search) >= 0)
        set_status(app, "Found %s", app->search);
    else
        set_status(app, "Not found: %s", app->search);
}

static void goto_action(struct app *app)
{
    if (app->active < 0)
        return;
    char value[64];
    if (!prompt_dialog(app, " Go to line ", "", value, sizeof(value)))
        return;
    char *end;
    errno = 0;
    long line = strtol(value, &end, 10);
    if (errno || end == value || *end || line <= 0 || line > INT32_MAX) {
        set_status(app, "Invalid line number: %s", value);
        return;
    }
    editor_goto_line(&app->tabs[app->active].ed, (int)line, false);
    set_status(app, "Line %ld", line);
}

static bool global_key(struct app *app, int key)
{
    switch (key) {
    case KEY_F(1): show_help(app); return true;
    case KEY_F(2): save_action(app); return true;
    case KEY_F(3): save_as_action(app); return true;
    case KEY_F(4): new_tab(app); return true;
    case KEY_F(5): build_action(app, true); return true;
    case KEY_F(6): focus_next(app, 1); return true;
    case KEY_F(7): build_action(app, false); return true;
    case KEY_F(8): close_action(app); return true;
    case KEY_F(9): app->show_output = !app->show_output; return true;
    case KEY_F(10): quit_action(app); return true;
    case KEY_BTAB: focus_next(app, -1); return true;
    case IDE_KEY_ALT_1: app->focus = FOCUS_EXPLORER; return true;
    case IDE_KEY_ALT_2: app->focus = FOCUS_EDITOR; return true;
    case IDE_KEY_ALT_3: app->show_output = true; app->focus = FOCUS_OUTPUT; return true;
    case IDE_KEY_ALT_LEFT:
    case IDE_KEY_CTRL_PGUP: switch_tab(app, -1); return true;
    case IDE_KEY_ALT_RIGHT:
    case IDE_KEY_CTRL_PGDN: switch_tab(app, 1); return true;
    case 14: new_tab(app); return true;                 /* Ctrl+N */
    case 15: open_action(app); return true;             /* Ctrl+O */
    case 19: save_action(app); return true;             /* Ctrl+S */
    case 23: close_action(app); return true;            /* Ctrl+W */
    case 17: quit_action(app); return true;             /* Ctrl+Q */
    case 18: tree_rebuild(app); set_status(app, "Project tree refreshed"); return true;
    case 12: redrawwin(stdscr); return true;            /* Ctrl+L */
    case 27:
        if (app->child_active) {
            stop_child(app);
        } else if (app->active >= 0) {
            struct editor *ed = &app->tabs[app->active].ed;
            editor_move_to(ed, ed->cursor, false);
        }
        return true;
    default:
        return false;
    }
}

static void explorer_key(struct app *app, int key)
{
    if (!app->tree_count)
        return;
    int visible = LINES - 6;
    if (visible < 1)
        visible = 1;
    switch (key) {
    case KEY_UP: app->tree_selected--; break;
    case KEY_DOWN: app->tree_selected++; break;
    case KEY_PPAGE: app->tree_selected -= visible; break;
    case KEY_NPAGE: app->tree_selected += visible; break;
    case KEY_HOME: app->tree_selected = 0; break;
    case KEY_END: app->tree_selected = app->tree_count - 1; break;
    case KEY_RIGHT:
        if (app->tree[app->tree_selected].is_dir &&
            !app->tree[app->tree_selected].expanded)
            tree_toggle(app, app->tree_selected);
        else if (app->tree_selected + 1 < app->tree_count &&
                 app->tree[app->tree_selected + 1].depth > app->tree[app->tree_selected].depth)
            app->tree_selected++;
        break;
    case KEY_LEFT:
        if (app->tree[app->tree_selected].is_dir && app->tree[app->tree_selected].expanded) {
            tree_toggle(app, app->tree_selected);
        } else {
            int depth = app->tree[app->tree_selected].depth;
            for (int i = app->tree_selected - 1; i >= 0; --i)
                if (app->tree[i].depth < depth) {
                    app->tree_selected = i;
                    break;
                }
        }
        break;
    case '\n':
    case '\r':
    case KEY_ENTER:
        if (app->tree[app->tree_selected].is_dir)
            tree_toggle(app, app->tree_selected);
        else
            open_file(app, app->tree[app->tree_selected].path);
        break;
    default:
        break;
    }
    if (app->tree_selected < 0)
        app->tree_selected = 0;
    if (app->tree_selected >= app->tree_count)
        app->tree_selected = app->tree_count - 1;
}

static void output_key(struct app *app, int key)
{
    if (!app->output_count)
        return;
    app->output_follow = false;
    int rows = LINES / 4 - 1;
    if (rows < 1)
        rows = 1;
    switch (key) {
    case KEY_UP: app->output_selected--; break;
    case KEY_DOWN: app->output_selected++; break;
    case KEY_PPAGE: app->output_selected -= rows; break;
    case KEY_NPAGE: app->output_selected += rows; break;
    case KEY_HOME: app->output_selected = 0; break;
    case KEY_END: app->output_selected = app->output_count - 1; break;
    case KEY_LEFT: app->output_left = app->output_left > 8 ? app->output_left - 8 : 0; break;
    case KEY_RIGHT: app->output_left += 8; break;
    case '\n':
    case '\r':
    case KEY_ENTER: {
        struct output_line *line = output_line_at(app, app->output_selected);
        if (line)
            jump_to_diagnostic(app, line->text);
        break;
    }
    default:
        break;
    }
    if (app->output_selected < 0)
        app->output_selected = 0;
    if (app->output_selected >= app->output_count)
        app->output_selected = app->output_count - 1;
}

static void editor_key(struct app *app, int key, const char *text)
{
    if (app->active < 0 || app->active >= app->tab_count)
        return;
    struct editor *ed = &app->tabs[app->active].ed;
    switch (key) {
    case 1: editor_select_all(ed); return;               /* Ctrl+A */
    case 3: copy_selection(app); return;                 /* Ctrl+C */
    case 24:                                             /* Ctrl+X */
        if (editor_selection(ed, NULL, NULL)) {
            copy_selection(app);
            edit_error(app, editor_delete(ed));
        }
        return;
    case 22:                                             /* Ctrl+V */
        if (app->clipboard)
            edit_error(app, editor_insert(ed, app->clipboard, app->clipboard_length));
        return;
    case 26: edit_error(app, editor_undo(ed)); return;   /* Ctrl+Z */
    case 25: edit_error(app, editor_redo(ed)); return;   /* Ctrl+Y */
    case 6: find_action(app); return;                    /* Ctrl+F */
    case 7: goto_action(app); return;                    /* Ctrl+G */
    case KEY_UP: editor_move_up(ed, false); return;
    case KEY_DOWN: editor_move_down(ed, false); return;
    case KEY_LEFT: editor_move_left(ed, false); return;
    case KEY_RIGHT: editor_move_right(ed, false); return;
    case IDE_KEY_SHIFT_UP:
    case KEY_SR: editor_move_up(ed, true); return;
    case IDE_KEY_SHIFT_DOWN:
    case KEY_SF: editor_move_down(ed, true); return;
    case IDE_KEY_SHIFT_LEFT:
    case KEY_SLEFT: editor_move_left(ed, true); return;
    case IDE_KEY_SHIFT_RIGHT:
    case KEY_SRIGHT: editor_move_right(ed, true); return;
    case IDE_KEY_CTRL_LEFT: editor_move_word(ed, -1, false); return;
    case IDE_KEY_CTRL_RIGHT: editor_move_word(ed, 1, false); return;
    case KEY_HOME: editor_move_home(ed, false); return;
    case KEY_END: editor_move_end(ed, false); return;
    case IDE_KEY_SHIFT_HOME: editor_move_home(ed, true); return;
    case IDE_KEY_SHIFT_END: editor_move_end(ed, true); return;
    case IDE_KEY_CTRL_HOME: editor_move_document(ed, false, false); return;
    case IDE_KEY_CTRL_END: editor_move_document(ed, true, false); return;
    case KEY_PPAGE: {
        struct layout layout;
        compute_layout(app, &layout);
        editor_move_page(ed, layout.body_h > 0 ? -layout.body_h : -1, false);
        return;
    }
    case KEY_NPAGE: {
        struct layout layout;
        compute_layout(app, &layout);
        editor_move_page(ed, layout.body_h > 0 ? layout.body_h : 1, false);
        return;
    }
    case KEY_BACKSPACE: edit_error(app, editor_backspace(ed)); return;
    case KEY_DC: edit_error(app, editor_delete(ed)); return;
    case '\n':
    case '\r':
    case KEY_ENTER: edit_error(app, editor_newline(ed)); return;
    case '\t': edit_error(app, editor_insert(ed, "    ", 4)); return;
    default:
        if (text[0])
            edit_error(app, editor_insert(ed, text, strlen(text)));
        return;
    }
}

static size_t mouse_editor_offset(struct app *app, const struct layout *layout,
                                  const MEVENT *event)
{
    struct editor *ed = &app->tabs[app->active].ed;
    int line = (int)ed->top_line + event->y - layout->body_y;
    int column = ed->left_column + event->x - layout->text_x;
    if (column < 0)
        column = 0;
    return editor_offset_at(ed, line, column);
}

static void mouse_editor(struct app *app, const struct layout *layout,
                         const MEVENT *event)
{
    if (app->active < 0)
        return;
    struct editor *ed = &app->tabs[app->active].ed;
    if (event->bstate & BUTTON4_PRESSED) {
        editor_move_up(ed, false);
        return;
    }
    if (event->bstate & BUTTON5_PRESSED) {
        editor_move_down(ed, false);
        return;
    }
    if (event->y < layout->body_y || event->y >= layout->output_y)
        return;
    app->focus = FOCUS_EDITOR;
    if (event->bstate & BUTTON1_PRESSED) {
        editor_move_to(ed, mouse_editor_offset(app, layout, event), false);
        ed->anchor = ed->cursor;
        app->dragging = true;
    } else if ((event->bstate & REPORT_MOUSE_POSITION) && app->dragging) {
        editor_move_to(ed, mouse_editor_offset(app, layout, event), true);
    } else if (event->bstate & BUTTON1_RELEASED) {
        if (app->dragging)
            editor_move_to(ed, mouse_editor_offset(app, layout, event), true);
        app->dragging = false;
    } else if (event->bstate & (BUTTON1_CLICKED | BUTTON1_DOUBLE_CLICKED)) {
        app->dragging = false;
        editor_move_to(ed, mouse_editor_offset(app, layout, event), false);
    }
}

static void mouse_key(struct app *app, const MEVENT *event)
{
    struct layout layout;
    compute_layout(app, &layout);
    if (event->y == layout.footer_y && (event->bstate &
        (BUTTON1_CLICKED | BUTTON1_DOUBLE_CLICKED))) {
        int slot = event->x * 10 / (COLS > 0 ? COLS : 1);
        if (slot >= 0 && slot < 10)
            global_key(app, KEY_F(slot + 1));
        return;
    }
    if (event->y == 1 && app->tab_count && (event->bstate &
        (BUTTON1_CLICKED | BUTTON1_DOUBLE_CLICKED))) {
        int tab = event->x * app->tab_count / (COLS > 0 ? COLS : 1);
        if (tab >= 0 && tab < app->tab_count) {
            app->active = tab;
            app->focus = FOCUS_EDITOR;
        }
        return;
    }
    if (layout.explorer_w && event->x < layout.explorer_w &&
        event->y >= layout.body_y + 2 && event->y < layout.output_y) {
        app->focus = FOCUS_EXPLORER;
        if (event->bstate & BUTTON4_PRESSED)
            explorer_key(app, KEY_UP);
        else if (event->bstate & BUTTON5_PRESSED)
            explorer_key(app, KEY_DOWN);
        else if (event->bstate & (BUTTON1_CLICKED | BUTTON1_DOUBLE_CLICKED)) {
            int index = app->tree_scroll + event->y - layout.body_y - 2;
            if (index >= 0 && index < app->tree_count) {
                app->tree_selected = index;
                if (app->tree[index].is_dir)
                    tree_toggle(app, index);
                else
                    open_file(app, app->tree[index].path);
            }
        }
        return;
    }
    if (layout.output_h && event->y > layout.output_y && event->y < layout.status_y) {
        app->focus = FOCUS_OUTPUT;
        if (event->bstate & BUTTON4_PRESSED) {
            output_key(app, KEY_UP);
        } else if (event->bstate & BUTTON5_PRESSED) {
            output_key(app, KEY_DOWN);
        } else if (event->bstate & (BUTTON1_CLICKED | BUTTON1_DOUBLE_CLICKED)) {
            int index = app->output_scroll + event->y - layout.output_y - 1;
            if (index >= 0 && index < app->output_count) {
                app->output_selected = index;
                app->output_follow = false;
                struct output_line *line = output_line_at(app, index);
                if (line)
                    jump_to_diagnostic(app, line->text);
            }
        }
        return;
    }
    if (event->x >= layout.editor_x)
        mouse_editor(app, &layout, event);
}

int main(int argc, char **argv)
{
    setlocale(LC_CTYPE, "");
    struct app app;
    if (!app_init(&app, argc, argv)) {
        app_cleanup(&app);
        return 1;
    }
    ui_init(&app);
    while (!app.should_quit) {
        pump_child(&app);
        draw_app(&app);
        char text[MB_LEN_MAX + 1];
        MEVENT event;
        int key = read_key(text, &event);
        if (key == ERR)
            continue;
        if (key == KEY_RESIZE) {
            redrawwin(stdscr);
            continue;
        }
        if (key == KEY_MOUSE) {
            mouse_key(&app, &event);
            continue;
        }
        if (global_key(&app, key))
            continue;
        if (app.focus == FOCUS_EXPLORER)
            explorer_key(&app, key);
        else if (app.focus == FOCUS_OUTPUT)
            output_key(&app, key);
        else
            editor_key(&app, key, text);
    }
    ui_shutdown();
    app_cleanup(&app);
    return 0;
}
