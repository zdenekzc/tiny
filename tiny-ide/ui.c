/* Midnight-Commander-like blue ncurses interface. */
#include "tiny_ide.h"

enum {
    P_BLUE = 1,
    P_TITLE,
    P_SELECT,
    P_KEYWORD,
    P_TYPE,
    P_STRING,
    P_COMMENT,
    P_NUMBER,
    P_PREPROC,
    P_OUTPUT,
    P_ERROR,
    P_DIALOG,
    P_MUTED,
    P_WARNING,
    P_SUCCESS
};

static void define_extra_keys(void)
{
    define_key("\033[1;2A", IDE_KEY_SHIFT_UP);
    define_key("\033[1;2B", IDE_KEY_SHIFT_DOWN);
    define_key("\033[1;2C", IDE_KEY_SHIFT_RIGHT);
    define_key("\033[1;2D", IDE_KEY_SHIFT_LEFT);
    define_key("\033[1;2H", IDE_KEY_SHIFT_HOME);
    define_key("\033[1;2F", IDE_KEY_SHIFT_END);
    define_key("\033[1;5D", IDE_KEY_CTRL_LEFT);
    define_key("\033[1;5C", IDE_KEY_CTRL_RIGHT);
    define_key("\033[1;5H", IDE_KEY_CTRL_HOME);
    define_key("\033[1;5F", IDE_KEY_CTRL_END);
    define_key("\033[1;3D", IDE_KEY_ALT_LEFT);
    define_key("\033[1;3C", IDE_KEY_ALT_RIGHT);
    define_key("\0331", IDE_KEY_ALT_1);
    define_key("\0332", IDE_KEY_ALT_2);
    define_key("\0333", IDE_KEY_ALT_3);
    define_key("\033[5;5~", IDE_KEY_CTRL_PGUP);
    define_key("\033[6;5~", IDE_KEY_CTRL_PGDN);
    /* Fallbacks used by Linux console and a few terminal emulators. */
    define_key("\033OH", KEY_HOME);
    define_key("\033OF", KEY_END);
    define_key("\033[H", KEY_HOME);
    define_key("\033[F", KEY_END);
    define_key("\033[12~", KEY_F(2));
    define_key("\033[13~", KEY_F(3));
    define_key("\033[14~", KEY_F(4));
}

void ui_init(struct app *app)
{
    initscr();
    start_color();
    raw();
    noecho();
    nonl();
    keypad(stdscr, TRUE);
    wtimeout(stdscr, 50);
    ESCDELAY = 50;
    curs_set(1);

    static const short palette[][3] = {
        {P_BLUE, COLOR_WHITE, COLOR_BLUE},
        {P_TITLE, COLOR_BLACK, COLOR_CYAN},
        {P_SELECT, COLOR_BLACK, COLOR_CYAN},
        {P_KEYWORD, COLOR_YELLOW, COLOR_BLUE},
        {P_TYPE, COLOR_CYAN, COLOR_BLUE},
        {P_STRING, COLOR_GREEN, COLOR_BLUE},
        {P_COMMENT, COLOR_YELLOW, COLOR_BLUE},
        {P_NUMBER, COLOR_MAGENTA, COLOR_BLUE},
        {P_PREPROC, COLOR_RED, COLOR_BLUE},
        {P_OUTPUT, COLOR_WHITE, COLOR_BLACK},
        {P_ERROR, COLOR_WHITE, COLOR_RED},
        {P_DIALOG, COLOR_BLACK, COLOR_WHITE},
        {P_MUTED, COLOR_CYAN, COLOR_BLUE},
        {P_WARNING, COLOR_YELLOW, COLOR_BLACK},
        {P_SUCCESS, COLOR_GREEN, COLOR_BLACK}
    };
    for (size_t i = 0; i < sizeof(palette) / sizeof(palette[0]); ++i) {
        short pair = palette[i][0];
        short foreground = palette[i][1], background = palette[i][2];
        if (!app->color_enabled) {
            bool light = pair == P_TITLE || pair == P_SELECT || pair == P_DIALOG;
            foreground = light ? COLOR_BLACK : COLOR_WHITE;
            background = light ? COLOR_WHITE : COLOR_BLACK;
        }
        init_pair(pair, foreground, background);
    }
    mouseinterval(120);
    mousemask(BUTTON1_PRESSED | BUTTON1_RELEASED | BUTTON1_CLICKED |
              BUTTON1_DOUBLE_CLICKED | BUTTON4_PRESSED | BUTTON5_PRESSED |
              REPORT_MOUSE_POSITION, NULL);
    define_extra_keys();
}

void ui_shutdown(void)
{
    mousemask(0, NULL);
    endwin();
}

int read_key(char text[MB_LEN_MAX + 1], MEVENT *event)
{
    wint_t input;
    text[0] = '\0';
    memset(event, 0, sizeof(*event));
    int result = wget_wch(stdscr, &input);
    if (result == ERR)
        return ERR;
    if (result == KEY_CODE_YES) {
        if (input == KEY_MOUSE)
            return getmouse(event) == OK ? KEY_MOUSE : ERR;
        return (int)input;
    }
    if (input == 127 || input == 8)
        return KEY_BACKSPACE;
    if (input < 32 || input == 27)
        return (int)input;
    if (!iswprint(input))
        return ERR;
    mbstate_t state;
    memset(&state, 0, sizeof(state));
    size_t bytes = wcrtomb(text, (wchar_t)input, &state);
    if (bytes == (size_t)-1) {
        text[0] = '\0';
        return ERR;
    }
    text[bytes] = '\0';
    return 0;
}

/* Draw complete UTF-8 cells, clipped by terminal columns, never by bytes. */
void ui_draw_text(int y, int x, const char *text, int start, int width,
                  attr_t attributes)
{
    if (width <= 0 || !text)
        return;
    int length = (int)strlen(text), column = 0;
    for (int offset = 0; offset < length && column < start + width;) {
        wchar_t chars[EDITOR_CELL_CHARS];
        int cell_width;
        int bytes = ed_text_cell(text + offset, length - offset, chars, &cell_width);
        if (bytes <= 0)
            break;
        bool tab = chars[0] == L'\t';
        if (tab)
            cell_width = 4 - column % 4;
        else if (!iswprint(chars[0])) {
            chars[0] = L'.';
            chars[1] = L'\0';
            cell_width = 1;
        }
        for (int part = 0; part < (tab ? cell_width : 1); ++part) {
            int at = column + part;
            int drawn_width = tab ? 1 : cell_width;
            if (at < start || at + drawn_width > start + width)
                continue;
            cchar_t cell;
            wchar_t shown[EDITOR_CELL_CHARS] = {tab ? L' ' : chars[0]};
            if (!tab) {
                for (int i = 1; i < EDITOR_CELL_CHARS; ++i)
                    shown[i] = chars[i];
            }
            setcchar(&cell, shown, attributes & ~A_COLOR,
                     PAIR_NUMBER(attributes), NULL);
            mvwadd_wchnstr(stdscr, y, x + at - start, &cell, 1);
        }
        column += cell_width;
        offset += bytes;
    }
}

void compute_layout(struct app *app, struct layout *layout)
{
    memset(layout, 0, sizeof(*layout));
    layout->body_y = 2;
    layout->footer_y = LINES - 1;
    layout->status_y = LINES - 2;
    if (app->show_output && LINES >= 10) {
        layout->output_h = LINES / 4;
        if (layout->output_h < 4)
            layout->output_h = 4;
        if (layout->output_h > 10)
            layout->output_h = 10;
        layout->output_y = layout->status_y - layout->output_h;
    } else {
        layout->output_y = layout->status_y;
    }
    layout->body_h = layout->output_y - layout->body_y;
    if (COLS >= 72) {
        layout->explorer_w = COLS / 4;
        if (layout->explorer_w < 22)
            layout->explorer_w = 22;
        if (layout->explorer_w > 32)
            layout->explorer_w = 32;
    } else if (COLS >= 54) {
        layout->explorer_w = 18;
    }
    layout->editor_x = layout->explorer_w;
    layout->editor_w = COLS - layout->editor_x;
    int digits = 4;
    if (app->active >= 0) {
        digits = 1;
        for (int n = app->tabs[app->active].ed.line_count; n >= 10; n /= 10)
            digits++;
        if (digits < 4)
            digits = 4;
    }
    layout->text_x = layout->editor_x + digits + 1;
    layout->text_w = COLS - layout->text_x;
    if (layout->text_w < 0)
        layout->text_w = 0;
}

static bool list_word(const char *word, size_t length, const char *const *list)
{
    for (int i = 0; list[i]; ++i)
        if (strlen(list[i]) == length && strncmp(word, list[i], length) == 0)
            return true;
    return false;
}

static bool c_keyword(const char *word, size_t length, bool *type)
{
    static const char *const keywords[] = {
        "alignas", "alignof", "asm", "auto", "break", "case", "catch",
        "class", "const", "constexpr", "const_cast", "continue", "decltype",
        "default", "delete", "do", "dynamic_cast", "else", "enum", "explicit",
        "export", "extern", "false", "for", "friend", "goto", "if", "inline",
        "mutable", "namespace", "new", "noexcept", "nullptr", "operator",
        "private", "protected", "public", "register", "reinterpret_cast",
        "requires", "restrict", "return", "sizeof", "static", "static_assert",
        "static_cast", "struct", "switch", "template", "this", "thread_local",
        "throw", "true", "try", "typedef", "typeid", "typename", "union",
        "using", "virtual", "volatile", "while", "_Alignas", "_Alignof",
        "_Atomic", "_Generic", "_Noreturn", "_Static_assert", "_Thread_local", NULL
    };
    static const char *const types[] = {
        "bool", "char", "char8_t", "char16_t", "char32_t", "double", "float",
        "int", "long", "short", "signed", "unsigned", "void", "wchar_t",
        "size_t", "ssize_t", "ptrdiff_t", "uint8_t", "uint16_t", "uint32_t",
        "uint64_t", "int8_t", "int16_t", "int32_t", "int64_t", NULL
    };
    *type = list_word(word, length, types);
    return *type || list_word(word, length, keywords);
}

/* Return one token's byte length and update only multiline comment state. */
static size_t syntax_token(const char *text, size_t length, bool *block,
                           bool preproc, attr_t *attributes)
{
    *attributes = COLOR_PAIR(P_BLUE);
    if (!length)
        return 0;
    if (preproc) {
        *attributes = COLOR_PAIR(P_PREPROC) | A_BOLD;
        return length;
    }
    if (*block) {
        *attributes = COLOR_PAIR(P_COMMENT);
        for (size_t i = 0; i + 1 < length; ++i)
            if (text[i] == '*' && text[i + 1] == '/') {
                *block = false;
                return i + 2;
            }
        return length;
    }
    if (length >= 2 && text[0] == '/' && text[1] == '/') {
        *attributes = COLOR_PAIR(P_COMMENT);
        return length;
    }
    if (length >= 2 && text[0] == '/' && text[1] == '*') {
        *attributes = COLOR_PAIR(P_COMMENT);
        for (size_t i = 2; i + 1 < length; ++i)
            if (text[i] == '*' && text[i + 1] == '/') {
                return i + 2;
            }
        *block = true;
        return length;
    }
    if (text[0] == '"' || text[0] == '\'') {
        char quote = text[0];
        bool escaped = false;
        *attributes = COLOR_PAIR(P_STRING);
        size_t i = 1;
        for (; i < length; ++i) {
            if (escaped) {
                escaped = false;
            } else if (text[i] == '\\') {
                escaped = true;
            } else if (text[i] == quote) {
                i++;
                break;
            }
        }
        return i;
    }
    if (isdigit((unsigned char)text[0])) {
        *attributes = COLOR_PAIR(P_NUMBER);
        size_t i = 1;
        while (i < length && (isalnum((unsigned char)text[i]) || text[i] == '.' ||
                              text[i] == '_' || ((text[i] == '+' || text[i] == '-') &&
                              i > 0 && (text[i - 1] == 'e' || text[i - 1] == 'E'))))
            i++;
        return i;
    }
    if (isalpha((unsigned char)text[0]) || text[0] == '_') {
        size_t i = 1;
        while (i < length && (isalnum((unsigned char)text[i]) || text[i] == '_'))
            i++;
        bool type;
        if (c_keyword(text, i, &type))
            *attributes = COLOR_PAIR(type ? P_TYPE : P_KEYWORD) | A_BOLD;
        return i;
    }
    return 1;
}

static bool syntax_supported(const struct tab *tab)
{
    const char *name = tab->untitled ? tab->title : tab->path;
    return has_extension(name, ".c") || has_extension(name, ".h") ||
           has_extension(name, ".cc") || has_extension(name, ".cpp") ||
           has_extension(name, ".cxx") || has_extension(name, ".hpp") ||
           has_extension(name, ".hh");
}

static void skip_syntax_line(const char *text, size_t length, bool *block)
{
    bool preproc = false;
    if (!*block) {
        size_t first = 0;
        while (first < length && (text[first] == ' ' || text[first] == '\t'))
            first++;
        preproc = first < length && text[first] == '#';
    }
    for (size_t at = 0; at < length;) {
        attr_t ignored;
        size_t token = syntax_token(text + at, length - at, block, preproc, &ignored);
        if (!token)
            break;
        at += token;
    }
}

static void draw_code_line(struct tab *tab, int row, int y, int x, int width,
                           bool *block)
{
    struct editor *ed = &tab->ed;
    const char *text = ed->text;
    size_t start = editor_line_start(ed, row), end = editor_line_end(ed, row);
    size_t length = end - start;
    bool supported = syntax_supported(tab), preproc = false;
    if (supported && !*block) {
        size_t first = 0;
        while (first < length && (text[start + first] == ' ' || text[start + first] == '\t'))
            first++;
        preproc = first < length && text[start + first] == '#';
    }
    size_t selection_start = 0, selection_end = 0;
    bool selected = editor_selection(ed, &selection_start, &selection_end) == 1;
    int column = 0;
    size_t token_end = 0;
    attr_t token_attributes = COLOR_PAIR(P_BLUE);
    for (size_t at = 0; at < length && column < ed->left_column + width;) {
        if (at >= token_end) {
            size_t token = supported ? syntax_token(text + start + at, length - at,
                                                    block, preproc, &token_attributes) : 1;
            if (!token)
                break;
            token_end = at + token;
        }
        wchar_t chars[EDITOR_CELL_CHARS];
        int cell_width;
        int bytes = ed_text_cell(text + start + at, (int)(length - at), chars, &cell_width);
        if (bytes <= 0)
            break;
        bool tab_character = chars[0] == L'\t';
        if (tab_character)
            cell_width = 4 - column % 4;
        else if (!iswprint(chars[0])) {
            chars[0] = L'.';
            chars[1] = L'\0';
            cell_width = 1;
        }
        attr_t attributes = selected && start + at < selection_end &&
                            start + at + (size_t)bytes > selection_start ?
                            COLOR_PAIR(P_SELECT) : token_attributes;
        for (int part = 0; part < (tab_character ? cell_width : 1); ++part) {
            int at_column = column + part;
            int drawn_width = tab_character ? 1 : cell_width;
            if (at_column < ed->left_column ||
                at_column + drawn_width > ed->left_column + width)
                continue;
            cchar_t cell;
            wchar_t shown[EDITOR_CELL_CHARS] = {tab_character ? L' ' : chars[0]};
            if (!tab_character)
                for (int i = 1; i < EDITOR_CELL_CHARS; ++i)
                    shown[i] = chars[i];
            setcchar(&cell, shown, attributes & ~A_COLOR, PAIR_NUMBER(attributes), NULL);
            mvwadd_wchnstr(stdscr, y, x + at_column - ed->left_column, &cell, 1);
        }
        column += cell_width;
        at += (size_t)bytes;
    }
}

static void draw_tabs(struct app *app)
{
    attrset(COLOR_PAIR(P_BLUE));
    move(1, 0);
    clrtoeol();
    if (!app->tab_count) {
        ui_draw_text(1, 1, "No file open - Ctrl+N new, Ctrl+O open", 0, COLS - 2,
                     COLOR_PAIR(P_MUTED));
        return;
    }
    for (int i = 0; i < app->tab_count; ++i) {
        int x = i * COLS / app->tab_count;
        int width = (i + 1) * COLS / app->tab_count - x;
        struct tab *tab = &app->tabs[i];
        char title[300];
        snprintf(title, sizeof(title), "%s%s", tab->title,
                 tab->ed.revision != tab->ed.saved_revision ? " *" : "");
        attr_t attributes = i == app->active ? COLOR_PAIR(P_SELECT) | A_BOLD :
                                               COLOR_PAIR(P_BLUE);
        attrset(attributes);
        mvhline(1, x, ' ', width);
        ui_draw_text(1, x + 1, title, 0, width - 2, attributes);
    }
}

static void draw_explorer(struct app *app, const struct layout *layout)
{
    if (!layout->explorer_w || layout->body_h <= 0)
        return;
    attrset(COLOR_PAIR(P_BLUE));
    for (int row = 0; row < layout->body_h; ++row) {
        move(layout->body_y + row, 0);
        mvhline(layout->body_y + row, 0, ' ', layout->explorer_w - 1);
        mvaddch(layout->body_y + row, layout->explorer_w - 1, '|');
    }
    ui_draw_text(layout->body_y, 1, "EXPLORER", 0, layout->explorer_w - 3,
                 COLOR_PAIR(P_MUTED) | A_BOLD);
    ui_draw_text(layout->body_y + 1, 1, app->project, 0, layout->explorer_w - 3,
                 COLOR_PAIR(P_BLUE));

    int rows = layout->body_h - 2;
    if (rows <= 0)
        return;
    if (app->tree_selected < app->tree_scroll)
        app->tree_scroll = app->tree_selected;
    if (app->tree_selected >= app->tree_scroll + rows)
        app->tree_scroll = app->tree_selected - rows + 1;
    if (app->tree_scroll > app->tree_count - rows)
        app->tree_scroll = app->tree_count > rows ? app->tree_count - rows : 0;
    if (app->tree_scroll < 0)
        app->tree_scroll = 0;
    for (int row = 0; row < rows && app->tree_scroll + row < app->tree_count; ++row) {
        int index = app->tree_scroll + row;
        struct tree_item *item = &app->tree[index];
        attr_t attributes = index == app->tree_selected ?
            COLOR_PAIR(P_SELECT) | (app->focus == FOCUS_EXPLORER ? A_BOLD : 0) :
            COLOR_PAIR(P_BLUE);
        int y = layout->body_y + 2 + row;
        attrset(attributes);
        mvhline(y, 0, ' ', layout->explorer_w - 1);
        char prefix[128];
        int indent = item->depth * 2;
        if (indent > 40)
            indent = 40;
        memset(prefix, ' ', (size_t)indent);
        prefix[indent] = '\0';
        char marker[160];
        snprintf(marker, sizeof(marker), "%s%s ", prefix,
                 item->is_dir ? (item->expanded ? "-" : "+") : " ");
        bool dirty = false;
        if (!item->is_dir) {
            int tab = find_open_tab(app, item->path);
            dirty = tab >= 0 && app->tabs[tab].ed.revision != app->tabs[tab].ed.saved_revision;
        }
        char label[512];
        snprintf(label, sizeof(label), "%s%s%s", marker, item->name, dirty ? " *" : "");
        ui_draw_text(y, 1, label, 0, layout->explorer_w - 3,
                     item->is_dir && index != app->tree_selected ?
                     attributes | A_BOLD : attributes);
    }
}

static void draw_editor(struct app *app, const struct layout *layout)
{
    if (app->active < 0 || app->active >= app->tab_count || layout->body_h <= 0)
        return;
    struct tab *tab = &app->tabs[app->active];
    struct editor *ed = &tab->ed;
    editor_scroll_visible(ed, layout->body_h, layout->text_w);
    bool block = false;
    for (int row = 0; row < (int)ed->top_line && row < ed->line_count; ++row)
        skip_syntax_line(ed->text + editor_line_start(ed, row),
                         editor_line_end(ed, row) - editor_line_start(ed, row), &block);
    int number_width = layout->text_x - layout->editor_x - 1;
    int cursor_line = 0;
    editor_position(ed, ed->cursor, &cursor_line, NULL);
    for (int line = 0; line < layout->body_h; ++line) {
        int y = layout->body_y + line;
        attrset(COLOR_PAIR(P_BLUE));
        move(y, layout->editor_x);
        clrtoeol();
        int row = (int)ed->top_line + line;
        if (row >= ed->line_count)
            continue;
        char number[32];
        snprintf(number, sizeof(number), "%*d ", number_width, row + 1);
        ui_draw_text(y, layout->editor_x, number, 0, number_width + 1,
                     COLOR_PAIR(P_MUTED) | (row == cursor_line ? A_BOLD : 0));
        draw_code_line(tab, row, y, layout->text_x, layout->text_w, &block);
    }
}

static void draw_output(struct app *app, const struct layout *layout)
{
    if (!layout->output_h)
        return;
    attrset(COLOR_PAIR(P_OUTPUT));
    for (int y = layout->output_y; y < layout->status_y; ++y) {
        move(y, 0);
        clrtoeol();
    }
    attrset(COLOR_PAIR(P_OUTPUT) | A_BOLD);
    mvhline(layout->output_y, 0, '-', COLS);
    const char *state = app->build_state == BUILD_COMPILING ? "COMPILING" :
                        app->build_state == BUILD_RUNNING ? "RUNNING" :
                        app->build_failed ? "FAILED" : "OUTPUT";
    ui_draw_text(layout->output_y, 2, state, 0, COLS - 4,
                 app->build_failed ? COLOR_PAIR(P_ERROR) | A_BOLD :
                                     COLOR_PAIR(P_OUTPUT) | A_BOLD);
    int rows = layout->output_h - 1;
    bool pinned = false;
    if (app->output_follow && app->output_count) {
        app->output_selected = app->output_count - 1;
        if (app->output_count > rows) {
            /* Pin the compiler command line (output line 0) at the top of the
               pane so the build itself stays visible while the tail follows
               the program; otherwise the pane shows only the last lines and
               F5 looks as if it never compiled anything. */
            app->output_scroll = app->output_count - rows + 1;
            if (app->output_scroll < 1)
                app->output_scroll = 1;
            pinned = true;
        } else {
            app->output_scroll = 0;
        }
    }
    if (app->output_selected < app->output_scroll)
        app->output_scroll = app->output_selected;
    if (app->output_selected >= app->output_scroll + rows)
        app->output_scroll = app->output_selected - rows + 1;
    if (app->output_scroll < 0)
        app->output_scroll = 0;
    for (int row = 0; row < rows; ++row) {
        int index = pinned && row == 0 ? 0 : app->output_scroll + row - (pinned ? 1 : 0);
        struct output_line *line = output_line_at(app, index);
        if (!line)
            break;
        attr_t attributes = COLOR_PAIR(P_OUTPUT);
        if (strstr(line->text, "error:") || strstr(line->text, "Error"))
            attributes = COLOR_PAIR(P_ERROR);
        else if (strstr(line->text, "warning:"))
            attributes = COLOR_PAIR(P_WARNING);
        else if (line->text[0] == '$')
            attributes = COLOR_PAIR(P_TYPE);
        if (index == app->output_selected && app->focus == FOCUS_OUTPUT)
            attributes = COLOR_PAIR(P_SELECT);
        attrset(attributes);
        move(layout->output_y + 1 + row, 0);
        clrtoeol();
        ui_draw_text(layout->output_y + 1 + row, 0, line->text,
                     app->output_left, COLS, attributes);
    }
}

static void draw_footer(struct app *app)
{
    (void)app;
    static const char *const labels[] = {
        "Help", "Save", "SaveAs", "New", "Run",
        "Pane", "Build", "Close", "Output", "Quit"
    };
    for (int i = 0; i < 10; ++i) {
        int x = i * COLS / 10;
        int width = (i + 1) * COLS / 10 - x;
        int digits = i == 9 ? 2 : 1;
        char key[4];
        snprintf(key, sizeof(key), "%d", i + 1);
        attrset(COLOR_PAIR(P_BLUE) | A_BOLD);
        mvaddstr(LINES - 1, x, key);
        attrset(COLOR_PAIR(P_TITLE));
        mvhline(LINES - 1, x + digits, ' ', width - digits);
        ui_draw_text(LINES - 1, x + digits, labels[i], 0, width - digits,
                     COLOR_PAIR(P_TITLE));
    }
}

/* Render the whole application to the virtual screen without flushing. */
void render_app(struct app *app)
{
    erase();
    wbkgd(stdscr, COLOR_PAIR(P_BLUE));
    if (LINES < 8 || COLS < 40) {
        attrset(COLOR_PAIR(P_ERROR) | A_BOLD);
        mvaddstr(0, 0, "Tiny IDE needs at least 40 x 8 terminal cells.");
        curs_set(0);
        refresh();
        return;
    }
    struct layout layout;
    compute_layout(app, &layout);

    attrset(COLOR_PAIR(P_TITLE) | A_BOLD);
    mvhline(0, 0, ' ', COLS);
    ui_draw_text(0, 1, "Tiny IDE", 0, COLS - 2, COLOR_PAIR(P_TITLE) | A_BOLD);
    char project[PATH_MAX + 32];
    snprintf(project, sizeof(project), "  %s", app->project);
    ui_draw_text(0, 11, project, 0, COLS > 30 ? COLS - 25 : COLS - 12,
                 COLOR_PAIR(P_TITLE));
    draw_tabs(app);
    draw_explorer(app, &layout);
    if (app->active >= 0) {
        draw_editor(app, &layout);
    } else if (layout.body_h > 1) {
        ui_draw_text(layout.body_y + 1, layout.editor_x + 2,
                     "Ctrl+N new file   Ctrl+O open path   F1 help", 0,
                     layout.editor_w - 4, COLOR_PAIR(P_MUTED));
    }
    draw_output(app, &layout);

    attrset(COLOR_PAIR(P_TITLE));
    mvhline(layout.status_y, 0, ' ', COLS);
    char position[128] = "";
    if (app->active >= 0) {
        int line, column;
        editor_position(&app->tabs[app->active].ed, app->tabs[app->active].ed.cursor,
                        &line, &column);
        snprintf(position, sizeof(position), "  Ln %d, Col %d  %s", line + 1, column + 1,
                 app->focus == FOCUS_EDITOR ? "Editor" :
                 app->focus == FOCUS_OUTPUT ? "Output" : "Explorer");
    }
    int reserved = (int)strlen(position);
    ui_draw_text(layout.status_y, 1, app->status, 0,
                 COLS - reserved - 2 > 0 ? COLS - reserved - 2 : 0,
                 COLOR_PAIR(P_TITLE));
    ui_draw_text(layout.status_y, COLS - reserved - 1, position, 0, reserved,
                 COLOR_PAIR(P_TITLE));
    draw_footer(app);

    /* While a modal dialog is open it owns the cursor; toggling visibility
       here and there would emit show/hide sequences every idle tick. */
    if (app->modal)
        return;
    if (app->focus == FOCUS_EDITOR && app->active >= 0 && layout.body_h > 0) {
        struct editor *ed = &app->tabs[app->active].ed;
        int line, column;
        editor_position(ed, ed->cursor, &line, &column);
        int y = layout.body_y + line - (int)ed->top_line;
        int x = layout.text_x + column - ed->left_column;
        if (y >= layout.body_y && y < layout.output_y && x >= layout.text_x && x < COLS) {
            curs_set(1);
            move(y, x);
        } else {
            curs_set(0);
        }
    } else {
        curs_set(0);
    }
}

void draw_app(struct app *app)
{
    render_app(app);
    refresh();
}
