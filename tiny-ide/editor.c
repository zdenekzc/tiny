#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#elif _XOPEN_SOURCE < 700
#undef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include "editor.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

struct editor_state {
    size_t cursor, anchor;
    unsigned long long revision;
    size_t top_line;
    int left_column, preferred_column;
    int preferred_valid;
};

struct editor_history {
    size_t start;
    size_t old_length, new_length;
    char *old_text, *new_text;
    struct editor_state before, after;
    struct editor_history *next;
};

struct decoded_char {
    wchar_t wc;
    size_t bytes;
    int valid;
    int width;
};

static struct decoded_char decode_char(const char *text, size_t length)
{
    struct decoded_char result;
    mbstate_t state;
    size_t used;

    result.wc = L'�';
    result.bytes = 1;
    result.valid = 0;
    result.width = 1;
    memset(&state, 0, sizeof(state));
    used = mbrtowc(&result.wc, text, length, &state);
    if (used == (size_t)-1 || used == (size_t)-2) {
        result.wc = L'�';
        return result;
    }

    result.valid = 1;
    result.bytes = used == 0 ? 1 : used;
    result.width = wcwidth(result.wc);
    if (result.width < 0)
        result.width = 1;
    return result;
}

/* Decode a base character and all following zero-width characters. */
static size_t text_cell_end(const char *text, size_t length, size_t position,
                            wchar_t *chars, int *width)
{
    struct decoded_char decoded;
    size_t position_after;
    int count = 0;

    if (position >= length) {
        if (width)
            *width = 0;
        return position;
    }
    if (text[position] == '\t') {
        if (chars)
            chars[0] = L'\t';
        if (width)
            *width = 1;
        return position + 1;
    }

    decoded = decode_char(text + position, length - position);
    position_after = position + decoded.bytes;
    if (decoded.width <= 0)
        decoded.width = 1;
    if (chars)
        chars[count++] = decoded.wc;

    while (position_after < length) {
        struct decoded_char mark = decode_char(text + position_after,
                                               length - position_after);
        if (!mark.valid || mark.width != 0)
            break;
        if (chars && count < EDITOR_CELL_CHARS - 1)
            chars[count++] = mark.wc;
        position_after += mark.bytes;
    }
    if (width)
        *width = decoded.width;
    return position_after;
}

int ed_text_cell(const char *text, int length,
                 wchar_t chars[EDITOR_CELL_CHARS], int *width)
{
    size_t consumed;

    if (chars)
        memset(chars, 0, sizeof(wchar_t) * EDITOR_CELL_CHARS);
    if (width)
        *width = 0;
    if (!text || length <= 0 || !chars || !width)
        return 0;
    consumed = text_cell_end(text, (size_t)length, 0, chars, width);
    chars[EDITOR_CELL_CHARS - 1] = L'\0';
    return (int)consumed;
}

size_t ed_text_next(const char *text, size_t length, size_t position)
{
    if (!text || position >= length)
        return position > length ? length : position;
    return text_cell_end(text, length, position, NULL, NULL);
}

static size_t previous_codepoint(const char *text, size_t position)
{
    size_t start = position - 1;

    while (start > 0 &&
           (((unsigned char)text[start] & 0xc0U) == 0x80U))
        --start;
    return start;
}

size_t ed_text_previous(const char *text, size_t position)
{
    size_t end = position;

    if (!text || position == 0)
        return 0;
    for (;;) {
        size_t start = previous_codepoint(text, end);
        struct decoded_char decoded = decode_char(text + start, end - start);

        if (!decoded.valid || decoded.width != 0 || start == 0)
            return start;
        end = start;
    }
}

static int add_column(int column, int width)
{
    if (width > 0 && column > INT_MAX - width)
        return INT_MAX;
    return column + width;
}

static int cell_advance(const char *text, size_t length, size_t position,
                        int column, int tab_width, size_t *next)
{
    int width;

    *next = text_cell_end(text, length, position, NULL, &width);
    if (text[position] == '\t')
        width = tab_width - column % tab_width;
    return add_column(column, width);
}

int ed_text_column(const char *text, size_t length, int tab_width)
{
    size_t position = 0;
    int column = 0;

    if (!text)
        return 0;
    if (tab_width <= 0)
        tab_width = 4;
    while (position < length) {
        size_t next;
        column = cell_advance(text, length, position, column, tab_width,
                              &next);
        position = next;
    }
    return column;
}

size_t ed_text_offset(const char *text, size_t length, int column,
                      int tab_width)
{
    size_t position = 0;
    int current = 0;

    if (!text || column <= 0)
        return 0;
    if (tab_width <= 0)
        tab_width = 4;
    while (position < length && current < column) {
        size_t next;
        int following = cell_advance(text, length, position, current,
                                     tab_width, &next);
        if (column < following)
            return position;
        current = following;
        position = next;
    }
    return position;
}

static void free_histories(struct editor_history *history)
{
    while (history) {
        struct editor_history *next = history->next;
        free(history->old_text);
        free(history->new_text);
        free(history);
        history = next;
    }
}

static size_t clamp_offset(const struct editor *e, size_t offset)
{
    return offset > e->length ? e->length : offset;
}

static int line_for_offset(const struct editor *e, size_t offset)
{
    int low = 0;
    int high = e->line_count - 1;

    while (low < high) {
        int middle = low + (high - low + 1) / 2;
        if (e->lines[middle] <= offset)
            low = middle;
        else
            high = middle - 1;
    }
    return low;
}

static void rebuild_lines(struct editor *e)
{
    int count = 1;
    size_t i;

    e->lines[0] = 0;
    for (i = 0; i < e->length; ++i) {
        if (e->text[i] == '\n')
            e->lines[count++] = i + 1;
    }
    e->line_count = count;
}

static int reserve_text(struct editor *e, size_t needed)
{
    size_t capacity;
    char *text;

    if (needed <= e->capacity)
        return 0;
    capacity = e->capacity ? e->capacity : 1;
    while (capacity < needed) {
        if (capacity > (EDITOR_MAX_FILE + 1) / 2) {
            capacity = EDITOR_MAX_FILE + 1;
            break;
        }
        capacity *= 2;
    }
    if (capacity < needed) {
        errno = EFBIG;
        return -1;
    }
    text = realloc(e->text, capacity);
    if (!text)
        return -1;
    e->text = text;
    e->capacity = capacity;
    return 0;
}

static int reserve_lines(struct editor *e, int needed)
{
    int capacity;
    size_t *lines;

    if (needed <= e->line_capacity)
        return 0;
    capacity = e->line_capacity ? e->line_capacity : 1;
    while (capacity < needed) {
        if (capacity > INT_MAX / 2) {
            capacity = INT_MAX;
            break;
        }
        capacity *= 2;
    }
    if (capacity < needed || (size_t)capacity > SIZE_MAX / sizeof(*lines)) {
        errno = ENOMEM;
        return -1;
    }
    lines = realloc(e->lines, (size_t)capacity * sizeof(*lines));
    if (!lines)
        return -1;
    e->lines = lines;
    e->line_capacity = capacity;
    return 0;
}

static struct editor_state save_state(const struct editor *e)
{
    struct editor_state state;

    state.cursor = e->cursor;
    state.anchor = e->anchor;
    state.revision = e->revision;
    state.top_line = e->top_line;
    state.left_column = e->left_column;
    state.preferred_column = e->preferred_column;
    state.preferred_valid = e->preferred_valid;
    return state;
}

static void restore_state(struct editor *e, struct editor_state state)
{
    e->cursor = clamp_offset(e, state.cursor);
    e->anchor = state.anchor == EDITOR_NO_SELECTION
                    ? EDITOR_NO_SELECTION
                    : clamp_offset(e, state.anchor);
    e->revision = state.revision;
    e->top_line = state.top_line;
    if (e->line_count > 0 && e->top_line >= (size_t)e->line_count)
        e->top_line = (size_t)e->line_count - 1;
    e->left_column = state.left_column < 0 ? 0 : state.left_column;
    e->preferred_column = state.preferred_column;
    e->preferred_valid = state.preferred_valid;
}

static int perform_edit(struct editor *e, size_t start, size_t old_length,
                        const char *new_text, size_t new_length)
{
    struct editor_history *history;
    size_t retained, resulting_length;
    size_t i;
    int added_lines = 0;

    if (!e || start > e->length || old_length > e->length - start ||
        (new_length && !new_text)) {
        errno = EINVAL;
        return -1;
    }
    retained = e->length - old_length;
    if (new_length > EDITOR_MAX_FILE - retained) {
        errno = EFBIG;
        return -1;
    }
    if (old_length == 0 && new_length == 0)
        return 0;
    resulting_length = retained + new_length;

    history = calloc(1, sizeof(*history));
    if (!history)
        return -1;
    history->start = start;
    history->old_length = old_length;
    history->new_length = new_length;
    history->before = save_state(e);
    if (old_length) {
        history->old_text = malloc(old_length);
        if (!history->old_text)
            goto allocation_error;
        memcpy(history->old_text, e->text + start, old_length);
    }
    if (new_length) {
        history->new_text = malloc(new_length);
        if (!history->new_text)
            goto allocation_error;
        memcpy(history->new_text, new_text, new_length);
    }
    for (i = 0; i < new_length; ++i) {
        if (new_text[i] == '\n')
            ++added_lines;
    }
    if (reserve_text(e, resulting_length + 1) < 0 ||
        reserve_lines(e, e->line_count + added_lines) < 0)
        goto allocation_error;

    free_histories(e->redo);
    e->redo = NULL;
    memmove(e->text + start + new_length, e->text + start + old_length,
            e->length - start - old_length + 1);
    if (new_length)
        memcpy(e->text + start, history->new_text, new_length);
    e->length = resulting_length;
    rebuild_lines(e);

    e->cursor = start + new_length;
    e->anchor = EDITOR_NO_SELECTION;
    e->preferred_valid = 0;
    if (e->top_line >= (size_t)e->line_count)
        e->top_line = (size_t)e->line_count - 1;
    ++e->revision;
    history->after = save_state(e);
    history->next = e->undo;
    e->undo = history;
    return 1;

allocation_error:
    free(history->old_text);
    free(history->new_text);
    free(history);
    return -1;
}

static int apply_history(struct editor *e, struct editor_history *history,
                         int redo)
{
    size_t old_length = redo ? history->old_length : history->new_length;
    size_t new_length = redo ? history->new_length : history->old_length;
    const char *new_text = redo ? history->new_text : history->old_text;
    size_t resulting_length = e->length - old_length + new_length;
    int added_lines = 0;
    size_t i;

    for (i = 0; i < new_length; ++i) {
        if (new_text[i] == '\n')
            ++added_lines;
    }
    if (reserve_text(e, resulting_length + 1) < 0 ||
        reserve_lines(e, e->line_count + added_lines) < 0)
        return -1;
    memmove(e->text + history->start + new_length,
            e->text + history->start + old_length,
            e->length - history->start - old_length + 1);
    if (new_length)
        memcpy(e->text + history->start, new_text, new_length);
    e->length = resulting_length;
    rebuild_lines(e);
    restore_state(e, redo ? history->after : history->before);
    return 1;
}

void editor_init(struct editor *e)
{
    if (!e)
        return;
    memset(e, 0, sizeof(*e));
    e->anchor = EDITOR_NO_SELECTION;
    (void)editor_set_text(e, NULL, 0);
}

int editor_set_text(struct editor *e, const char *data, size_t length)
{
    char *text;
    size_t *lines;
    size_t i, output = 0;
    int line_count = 1, line_capacity;

    if (!e || (!data && length != 0)) {
        errno = EINVAL;
        return -1;
    }
    if (length > EDITOR_MAX_FILE) {
        errno = EFBIG;
        return -1;
    }
    for (i = 0; i < length; ++i) {
        if (data[i] == '\0') {
            errno = EINVAL;
            return -1;
        }
        if (data[i] == '\n')
            ++line_count;
    }
    /* CRLF normalization can only reduce the allocated size and line count. */
    text = malloc(length + 1);
    line_capacity = line_count;
    lines = malloc((size_t)line_capacity * sizeof(*lines));
    if (!text || !lines) {
        free(text);
        free(lines);
        return -1;
    }
    for (i = 0; i < length; ++i) {
        if (data[i] == '\r' && i + 1 < length && data[i + 1] == '\n')
            continue;
        text[output++] = data[i];
    }
    text[output] = '\0';

    free(e->text);
    free(e->lines);
    free_histories(e->undo);
    free_histories(e->redo);
    e->text = text;
    e->length = output;
    e->capacity = length + 1;
    e->lines = lines;
    e->line_capacity = line_capacity;
    rebuild_lines(e);
    e->cursor = 0;
    e->anchor = EDITOR_NO_SELECTION;
    e->revision = 0;
    e->saved_revision = 0;
    e->top_line = 0;
    e->left_column = 0;
    e->preferred_column = 0;
    e->preferred_valid = 0;
    e->undo = NULL;
    e->redo = NULL;
    return 0;
}

void editor_free(struct editor *e)
{
    if (!e)
        return;
    free(e->text);
    free(e->lines);
    free_histories(e->undo);
    free_histories(e->redo);
    memset(e, 0, sizeof(*e));
    e->anchor = EDITOR_NO_SELECTION;
}

size_t editor_line_start(const struct editor *e, int line)
{
    if (!e || e->line_count <= 0)
        return 0;
    if (line < 0)
        line = 0;
    if (line >= e->line_count)
        line = e->line_count - 1;
    return e->lines[line];
}

size_t editor_line_end(const struct editor *e, int line)
{
    if (!e || e->line_count <= 0)
        return 0;
    if (line < 0)
        line = 0;
    if (line >= e->line_count)
        line = e->line_count - 1;
    if (line + 1 == e->line_count)
        return e->length;
    return e->lines[line + 1] - 1;
}

int editor_position(const struct editor *e, size_t offset,
                    int *line, int *column)
{
    int found_line;
    size_t start, position;

    if (!e || !e->text || e->line_count <= 0)
        return -1;
    offset = clamp_offset(e, offset);
    found_line = line_for_offset(e, offset);
    start = e->lines[found_line];
    position = start;
    if (line)
        *line = found_line;
    if (column) {
        int display_column = 0;
        while (position < offset) {
            size_t next = text_cell_end(e->text, e->length, position,
                                        NULL, NULL);
            int width;
            if (next > offset)
                break;
            if (e->text[position] == '\t')
                width = 4 - display_column % 4;
            else
                (void)text_cell_end(e->text, e->length, position, NULL,
                                    &width);
            display_column = add_column(display_column, width);
            position = next;
        }
        *column = display_column;
    }
    return 0;
}

size_t editor_offset_at(const struct editor *e, int line, int column)
{
    size_t start, end, relative;

    if (!e || !e->text || e->line_count <= 0)
        return 0;
    start = editor_line_start(e, line);
    end = editor_line_end(e, line);
    relative = ed_text_offset(e->text + start, end - start, column, 4);
    return start + relative;
}

int editor_scroll_visible(struct editor *e, int rows, int columns)
{
    size_t old_top;
    int old_left, line, column;

    if (!e || rows <= 0 || columns <= 0 ||
        editor_position(e, e->cursor, &line, &column) < 0)
        return 0;
    old_top = e->top_line;
    old_left = e->left_column;
    if (e->left_column < 0)
        e->left_column = 0;
    if (e->top_line > (size_t)line)
        e->top_line = (size_t)line;
    else if (line - (int)e->top_line >= rows)
        e->top_line = (size_t)(line - rows + 1);
    if (column < e->left_column)
        e->left_column = column;
    else if (column - e->left_column >= columns)
        e->left_column = column - columns + 1;
    return old_top != e->top_line || old_left != e->left_column;
}

static void finish_motion(struct editor *e, size_t offset, int keep_selection,
                          int vertical)
{
    if (keep_selection) {
        if (e->anchor == EDITOR_NO_SELECTION)
            e->anchor = e->cursor;
    } else {
        e->anchor = EDITOR_NO_SELECTION;
    }
    e->cursor = clamp_offset(e, offset);
    if (!vertical)
        e->preferred_valid = 0;
}

void editor_move_to(struct editor *e, size_t offset, int keep_selection)
{
    if (e)
        finish_motion(e, offset, keep_selection, 0);
}

void editor_move_left(struct editor *e, int keep_selection)
{
    if (e && e->text)
        finish_motion(e, ed_text_previous(e->text, e->cursor),
                      keep_selection, 0);
}

void editor_move_right(struct editor *e, int keep_selection)
{
    if (e && e->text)
        finish_motion(e, ed_text_next(e->text, e->length, e->cursor),
                      keep_selection, 0);
}

static void move_vertically(struct editor *e, int distance, int keep_selection)
{
    int line, column;
    int64_t target;

    if (!e || !e->text || editor_position(e, e->cursor, &line, &column) < 0)
        return;
    if (!e->preferred_valid) {
        e->preferred_column = column;
        e->preferred_valid = 1;
    }
    target = (int64_t)line + distance;
    if (target < 0)
        target = 0;
    if (target >= e->line_count)
        target = e->line_count - 1;
    finish_motion(e, editor_offset_at(e, (int)target, e->preferred_column),
                  keep_selection, 1);
}

void editor_move_up(struct editor *e, int keep_selection)
{
    move_vertically(e, -1, keep_selection);
}

void editor_move_down(struct editor *e, int keep_selection)
{
    move_vertically(e, 1, keep_selection);
}

void editor_move_home(struct editor *e, int keep_selection)
{
    int line;

    if (e && editor_position(e, e->cursor, &line, NULL) == 0)
        finish_motion(e, editor_line_start(e, line), keep_selection, 0);
}

void editor_move_end(struct editor *e, int keep_selection)
{
    int line;

    if (e && editor_position(e, e->cursor, &line, NULL) == 0)
        finish_motion(e, editor_line_end(e, line), keep_selection, 0);
}

void editor_move_page(struct editor *e, int lines, int keep_selection)
{
    move_vertically(e, lines, keep_selection);
}

void editor_move_document(struct editor *e, int end, int keep_selection)
{
    if (e)
        finish_motion(e, end ? e->length : 0, keep_selection, 0);
}

static int word_kind(const struct editor *e, size_t position)
{
    struct decoded_char decoded = decode_char(e->text + position,
                                              e->length - position);

    if (decoded.valid && iswspace(decoded.wc))
        return 1;
    if (decoded.wc == L'_' || (decoded.valid && iswalnum(decoded.wc)))
        return 2;
    return 3;
}

void editor_move_word(struct editor *e, int direction, int keep_selection)
{
    size_t position;

    if (!e || !e->text)
        return;
    position = e->cursor;
    if (direction > 0) {
        while (position < e->length && word_kind(e, position) == 1)
            position = ed_text_next(e->text, e->length, position);
        if (position < e->length) {
            int kind = word_kind(e, position);
            do {
                position = ed_text_next(e->text, e->length, position);
            } while (position < e->length && word_kind(e, position) == kind);
        }
    } else if (direction < 0) {
        while (position > 0) {
            size_t previous = ed_text_previous(e->text, position);
            if (word_kind(e, previous) != 1)
                break;
            position = previous;
        }
        if (position > 0) {
            size_t previous = ed_text_previous(e->text, position);
            int kind = word_kind(e, previous);
            do {
                position = previous;
                if (position == 0)
                    break;
                previous = ed_text_previous(e->text, position);
            } while (word_kind(e, previous) == kind);
        }
    }
    finish_motion(e, position, keep_selection, 0);
}

void editor_goto_line(struct editor *e, int one_based_line, int keep_selection)
{
    if (!e || !e->text)
        return;
    if (one_based_line <= 1)
        one_based_line = 1;
    else if (one_based_line > e->line_count)
        one_based_line = e->line_count;
    finish_motion(e, editor_line_start(e, one_based_line - 1),
                  keep_selection, 0);
}

void editor_select_all(struct editor *e)
{
    if (!e)
        return;
    e->anchor = 0;
    e->cursor = e->length;
    e->preferred_valid = 0;
}

int editor_selection(const struct editor *e, size_t *start, size_t *end)
{
    size_t first, last;

    if (!e || e->anchor == EDITOR_NO_SELECTION || e->anchor == e->cursor)
        return 0;
    first = e->anchor < e->cursor ? e->anchor : e->cursor;
    last = e->anchor < e->cursor ? e->cursor : e->anchor;
    first = clamp_offset(e, first);
    last = clamp_offset(e, last);
    if (first == last)
        return 0;
    if (start)
        *start = first;
    if (end)
        *end = last;
    return 1;
}

char *editor_copy_selection(const struct editor *e, size_t *length)
{
    size_t start, end;
    char *copy;

    if (length)
        *length = 0;
    if (!editor_selection(e, &start, &end))
        return NULL;
    copy = malloc(end - start + 1);
    if (!copy)
        return NULL;
    memcpy(copy, e->text + start, end - start);
    copy[end - start] = '\0';
    if (length)
        *length = end - start;
    return copy;
}

int editor_insert(struct editor *e, const char *text, size_t length)
{
    size_t start, end;

    if (!e || (!text && length != 0)) {
        errno = EINVAL;
        return -1;
    }
    if (editor_selection(e, &start, &end))
        return perform_edit(e, start, end - start, text, length);
    return perform_edit(e, e->cursor, 0, text, length);
}

int editor_backspace(struct editor *e)
{
    size_t start, end;

    if (!e || !e->text)
        return 0;
    if (editor_selection(e, &start, &end))
        return perform_edit(e, start, end - start, NULL, 0);
    if (e->cursor == 0)
        return 0;
    start = ed_text_previous(e->text, e->cursor);
    return perform_edit(e, start, e->cursor - start, NULL, 0);
}

int editor_delete(struct editor *e)
{
    size_t start, end;

    if (!e || !e->text)
        return 0;
    if (editor_selection(e, &start, &end))
        return perform_edit(e, start, end - start, NULL, 0);
    if (e->cursor >= e->length)
        return 0;
    end = ed_text_next(e->text, e->length, e->cursor);
    return perform_edit(e, e->cursor, end - e->cursor, NULL, 0);
}

int editor_newline(struct editor *e)
{
    size_t start, end, line_start, line_end, indent = 0, position;
    int extra = 0;
    char *inserted;
    int result;

    if (!e || !e->text)
        return 0;
    if (!editor_selection(e, &start, &end)) {
        start = e->cursor;
        end = start;
    }
    line_start = editor_line_start(e, line_for_offset(e, start));
    line_end = editor_line_end(e, line_for_offset(e, start));
    while (line_start + indent < line_end &&
           (e->text[line_start + indent] == ' ' ||
            e->text[line_start + indent] == '\t') &&
           indent < start - line_start)
        ++indent;
    position = start;
    while (position > line_start &&
           (e->text[position - 1] == ' ' || e->text[position - 1] == '\t'))
        --position;
    if (position > line_start && e->text[position - 1] == '{')
        extra = 4;
    inserted = malloc(indent + (size_t)extra + 1);
    if (!inserted)
        return -1;
    inserted[0] = '\n';
    memcpy(inserted + 1, e->text + line_start, indent);
    memset(inserted + 1 + indent, ' ', (size_t)extra);
    result = perform_edit(e, start, end - start, inserted,
                          indent + (size_t)extra + 1);
    free(inserted);
    return result;
}

int editor_undo(struct editor *e)
{
    struct editor_history *history;
    int result;

    if (!e || !e->undo)
        return 0;
    history = e->undo;
    result = apply_history(e, history, 0);
    if (result < 0)
        return result;
    e->undo = history->next;
    history->next = e->redo;
    e->redo = history;
    return 1;
}

int editor_redo(struct editor *e)
{
    struct editor_history *history;
    int result;

    if (!e || !e->redo)
        return 0;
    history = e->redo;
    result = apply_history(e, history, 1);
    if (result < 0)
        return result;
    e->redo = history->next;
    history->next = e->undo;
    e->undo = history;
    return 1;
}

void editor_mark_saved(struct editor *e)
{
    if (e)
        e->saved_revision = e->revision;
}

long editor_find(struct editor *e, const char *needle)
{
    size_t needle_length, count, start, i, selection_start, selection_end;

    if (!e || !e->text || !needle)
        return -1;
    needle_length = strlen(needle);
    if (needle_length == 0 || needle_length > e->length)
        return -1;
    count = e->length - needle_length + 1;
    start = e->cursor <= count - 1 ? e->cursor : 0;
    if (editor_selection(e, &selection_start, &selection_end) &&
        e->cursor == selection_start && selection_start < count &&
        memcmp(e->text + selection_start, needle, needle_length) == 0)
        start = (selection_start + 1) % count;
    for (i = 0; i < count; ++i) {
        size_t position = (start + i) % count;
        if (memcmp(e->text + position, needle, needle_length) == 0) {
            e->anchor = position;
            e->cursor = position + needle_length;
            e->preferred_valid = 0;
            return (long)position;
        }
    }
    return -1;
}
