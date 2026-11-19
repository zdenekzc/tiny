/* Centered ASCII dialogs using the same small curses subset as the main UI. */
#include "tiny_ide.h"

#define D_BLUE COLOR_PAIR(1)
#define D_TITLE COLOR_PAIR(2)
#define D_SELECT COLOR_PAIR(3)
#define D_ERROR COLOR_PAIR(11)
#define D_DIALOG COLOR_PAIR(12)

struct box {
    int y, x, height, width;
};

static void draw_box(struct box box, const char *title, bool danger)
{
    attr_t attributes = danger ? D_ERROR | A_BOLD : D_DIALOG;
    attrset(attributes);
    for (int y = 0; y < box.height; ++y)
        mvhline(box.y + y, box.x, ' ', box.width);
    mvaddch(box.y, box.x, '+');
    mvaddch(box.y, box.x + box.width - 1, '+');
    mvaddch(box.y + box.height - 1, box.x, '+');
    mvaddch(box.y + box.height - 1, box.x + box.width - 1, '+');
    mvwhline(stdscr, box.y, box.x + 1, '-', box.width - 2);
    mvwhline(stdscr, box.y + box.height - 1, box.x + 1, '-', box.width - 2);
    for (int y = 1; y + 1 < box.height; ++y) {
        mvaddch(box.y + y, box.x, '|');
        mvaddch(box.y + y, box.x + box.width - 1, '|');
    }
    int title_width = ed_text_column(title, strlen(title), 4);
    ui_draw_text(box.y, box.x + (box.width - title_width - 2) / 2, title,
                 0, box.width - 4, attributes | A_BOLD);
}

static bool click(const MEVENT *event)
{
    return (event->bstate & (BUTTON1_CLICKED | BUTTON1_DOUBLE_CLICKED)) != 0;
}

bool prompt_dialog(struct app *app, const char *title, const char *initial,
                   char *result, size_t result_size)
{
    if (!result_size)
        return false;
    char *value = malloc(result_size);
    if (!value)
        return false;
    snprintf(value, result_size, "%s", initial ? initial : "");
    size_t cursor = strlen(value);
    int horizontal = 0;
    bool accepted = false;
    app->modal = true;

    for (;;) {
        pump_child(app);
        render_app(app);
        int width = COLS - 4;
        if (width > 76)
            width = 76;
        if (width < 24)
            width = COLS;
        struct box box = { LINES / 2 - 4, (COLS - width) / 2, 8, width };
        if (box.y < 0)
            box.y = 0;
        draw_box(box, title, false);
        ui_draw_text(box.y + 2, box.x + 3, "Path / value:", 0, box.width - 6, D_DIALOG);
        int field_width = box.width - 6;
        int column = ed_text_column(value, cursor, 4);
        if (column < horizontal)
            horizontal = column;
        if (column >= horizontal + field_width)
            horizontal = column - field_width + 1;
        attrset(D_SELECT);
        mvhline(box.y + 3, box.x + 3, ' ', field_width);
        ui_draw_text(box.y + 3, box.x + 3, value, horizontal, field_width, D_SELECT);
        ui_draw_text(box.y + 5, box.x + box.width / 2 - 10, "[ OK ]",
                     0, 6, D_TITLE | A_BOLD);
        ui_draw_text(box.y + 5, box.x + box.width / 2 + 3, "[ Cancel ]",
                     0, 10, D_DIALOG);
        ui_draw_text(box.y + 6, box.x + 3, "Enter accepts, Esc cancels", 0,
                     box.width - 6, D_DIALOG);
        curs_set(1);
        move(box.y + 3, box.x + 3 + column - horizontal);
        refresh();

        char text[MB_LEN_MAX + 1];
        MEVENT event;
        int key = read_key(text, &event);
        if (key == ERR || key == KEY_RESIZE)
            continue;
        if (key == 27 || key == KEY_F(10))
            break;
        if (key == '\n' || key == '\r' || key == KEY_ENTER) {
            accepted = true;
            break;
        }
        if (key == KEY_LEFT && cursor)
            cursor = ed_text_previous(value, cursor);
        else if (key == KEY_RIGHT && cursor < strlen(value))
            cursor = ed_text_next(value, strlen(value), cursor);
        else if (key == KEY_HOME)
            cursor = 0;
        else if (key == KEY_END)
            cursor = strlen(value);
        else if (key == KEY_BACKSPACE && cursor) {
            size_t previous = ed_text_previous(value, cursor);
            memmove(value + previous, value + cursor, strlen(value) - cursor + 1);
            cursor = previous;
        } else if (key == KEY_DC && cursor < strlen(value)) {
            size_t next = ed_text_next(value, strlen(value), cursor);
            memmove(value + cursor, value + next, strlen(value) - next + 1);
        } else if (key == 21) { /* Ctrl+U */
            value[0] = '\0';
            cursor = 0;
        } else if (key == KEY_MOUSE && click(&event)) {
            int ok_x = box.x + box.width / 2 - 10;
            int cancel_x = box.x + box.width / 2 + 3;
            if (event.y == box.y + 5 && event.x >= ok_x && event.x < ok_x + 6) {
                accepted = true;
                break;
            }
            if (event.y == box.y + 5 && event.x >= cancel_x && event.x < cancel_x + 10)
                break;
            if (event.y == box.y + 3 && event.x >= box.x + 3 &&
                event.x < box.x + 3 + field_width)
                cursor = ed_text_offset(value, strlen(value),
                                        horizontal + event.x - box.x - 3, 4);
        } else if (text[0]) {
            size_t bytes = strlen(value), insert = strlen(text);
            if (bytes + insert < result_size) {
                memmove(value + cursor + insert, value + cursor, bytes - cursor + 1);
                memcpy(value + cursor, text, insert);
                cursor += insert;
            } else {
                beep();
            }
        }
    }
    if (accepted)
        snprintf(result, result_size, "%s", value);
    free(value);
    app->modal = false;
    draw_app(app);
    return accepted;
}

static int message_lines(const char *message)
{
    int count = 1;
    for (const char *p = message; *p; ++p)
        if (*p == '\n')
            count++;
    return count;
}

int confirm_dialog(struct app *app, const char *title, const char *message,
                   const char *const *buttons, int count, int selected)
{
    if (count <= 0)
        return -1;
    if (selected < 0 || selected >= count)
        selected = 0;
    int lines = message_lines(message);
    int content_width = ed_text_column(title, strlen(title), 4) + 6;
    const char *line = message;
    while (line) {
        const char *end = strchr(line, '\n');
        int length = end ? (int)(end - line) : (int)strlen(line);
        int width = ed_text_column(line, (size_t)length, 4) + 8;
        if (width > content_width)
            content_width = width;
        line = end ? end + 1 : NULL;
    }
    int buttons_width = 4;
    for (int i = 0; i < count; ++i)
        buttons_width += (int)strlen(buttons[i]) + 8;
    if (buttons_width > content_width)
        content_width = buttons_width;
    int width = content_width > COLS - 2 ? COLS - 2 : content_width;
    int height = lines + 7;
    if (height > LINES - 2)
        height = LINES - 2;
    struct box box = { (LINES - height) / 2, (COLS - width) / 2, height, width };
    int answer = -1;
    int button_x[8] = {0};
    int button_width[8] = {0};
    if (count > 8)
        count = 8;
    app->modal = true;
    int total = 0;
    for (int i = 0; i < count; ++i) {
        button_width[i] = (int)strlen(buttons[i]) + 6;
        total += button_width[i] + 2;
    }

    for (;;) {
        pump_child(app);
        render_app(app);
        draw_box(box, title, false);
        int y = box.y + 2;
        line = message;
        while (line && y < box.y + box.height - 3) {
            const char *end = strchr(line, '\n');
            int length = end ? (int)(end - line) : (int)strlen(line);
            char fragment[1024];
            int copied = length < (int)sizeof(fragment) - 1 ? length : (int)sizeof(fragment) - 1;
            memcpy(fragment, line, (size_t)copied);
            fragment[copied] = '\0';
            ui_draw_text(y++, box.x + 3, fragment, 0, box.width - 6, D_DIALOG);
            line = end ? end + 1 : NULL;
        }
        int x = box.x + (box.width - total) / 2 + 1;
        for (int i = 0; i < count; ++i) {
            button_x[i] = x;
            attrset(i == selected ? D_TITLE | A_BOLD : D_DIALOG);
            mvprintw(box.y + box.height - 3, x, "[ %s ]", buttons[i]);
            x += button_width[i] + 2;
        }
        curs_set(0);
        refresh();

        char text[MB_LEN_MAX + 1];
        MEVENT event;
        int key = read_key(text, &event);
        if (key == ERR || key == KEY_RESIZE)
            continue;
        if (key == 27 || key == KEY_F(10))
            break;
        if (key == KEY_LEFT || key == KEY_BTAB)
            selected = (selected + count - 1) % count;
        else if (key == KEY_RIGHT || key == '\t')
            selected = (selected + 1) % count;
        else if (key == KEY_HOME)
            selected = 0;
        else if (key == KEY_END)
            selected = count - 1;
        else if (key == '\n' || key == '\r' || key == KEY_ENTER) {
            answer = selected;
            break;
        } else if (key == KEY_MOUSE && click(&event) &&
                   event.y == box.y + box.height - 3) {
            for (int i = 0; i < count; ++i)
                if (event.x >= button_x[i] && event.x < button_x[i] + button_width[i]) {
                    answer = i;
                    break;
                }
            if (answer >= 0)
                break;
        }
    }
    app->modal = false;
    draw_app(app);
    return answer;
}

void show_message(struct app *app, const char *title, const char *message)
{
    const char *const buttons[] = { "OK" };
    confirm_dialog(app, title, message, buttons, 1, 0);
}

void show_help(struct app *app)
{
    static const char *const help[] = {
        "Tiny IDE - a small ncurses IDE for C and C++",
        "",
        "F1 Help              F2 / Ctrl+S Save",
        "F3 Save as           F4 / Ctrl+N New file",
        "F5 Build + run       F6 Next pane",
        "F7 Build             F8 / Ctrl+W Close tab",
        "F9 Output pane       F10 / Ctrl+Q Quit",
        "Ctrl+O Open path     Ctrl+R Refresh project tree",
        "Ctrl+F Find          Ctrl+G Go to line",
        "Ctrl+Z / Ctrl+Y      Undo / redo",
        "Ctrl+A/C/X/V         Select all / copy / cut / paste",
        "Shift+arrows         Extend selection",
        "Ctrl+Left/Right      Previous / next word",
        "Ctrl+Home/End        Beginning / end of file",
        "Ctrl+PgUp/PgDn       Previous / next tab",
        "Alt+1 / 2 / 3        Explorer / editor / output",
        "",
        "Explorer: arrows and Enter; Right expands, Left folds.",
        "Output: arrows select a line; Enter jumps to file:line.",
        "Editor: Tab inserts four spaces; Enter auto-indents.",
        "Esc stops a running compiler/program (then SIGKILL).",
        "The clipboard is internal to Tiny IDE. Program stdin is /dev/null.",
        "Files are limited to 4 MiB. Long runs stop after 15 seconds.",
        "",
        "Esc, F10 or Enter closes this help."
    };
    int count = (int)(sizeof(help) / sizeof(help[0]));
    int scroll = 0;
    app->modal = true;
    for (;;) {
        pump_child(app);
        render_app(app);
        int height = LINES - 2;
        if (height > count + 4)
            height = count + 4;
        if (height < 5)
            height = LINES;
        int width = COLS - 6;
        if (width > 76)
            width = 76;
        struct box box = { (LINES - height) / 2, (COLS - width) / 2, height, width };
        draw_box(box, " Tiny IDE - Help ", false);
        int rows = height - 4;
        if (scroll > count - rows)
            scroll = count > rows ? count - rows : 0;
        if (scroll < 0)
            scroll = 0;
        for (int i = 0; i < rows && scroll + i < count; ++i)
            ui_draw_text(box.y + 2 + i, box.x + 2, help[scroll + i], 0,
                         box.width - 4, D_DIALOG);
        char footer[64];
        snprintf(footer, sizeof(footer), "%d-%d / %d", scroll + 1,
                 scroll + rows < count ? scroll + rows : count, count);
        ui_draw_text(box.y + height - 2, box.x + 2, footer, 0, box.width - 4,
                     D_DIALOG);
        curs_set(0);
        refresh();
        char text[MB_LEN_MAX + 1];
        MEVENT event;
        int key = read_key(text, &event);
        if (key == 27 || key == KEY_F(10) || key == '\n' || key == '\r')
            break;
        if (key == KEY_UP)
            scroll--;
        else if (key == KEY_DOWN)
            scroll++;
        else if (key == KEY_PPAGE)
            scroll -= rows;
        else if (key == KEY_NPAGE)
            scroll += rows;
        else if (key == KEY_HOME)
            scroll = 0;
        else if (key == KEY_END)
            scroll = count;
        else if (key == KEY_MOUSE && (event.bstate & BUTTON4_PRESSED))
            scroll--;
        else if (key == KEY_MOUSE && (event.bstate & BUTTON5_PRESSED))
            scroll++;
    }
    app->modal = false;
    draw_app(app);
}
