#ifndef TINY_IDE_EDITOR_H
#define TINY_IDE_EDITOR_H

#include <stddef.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EDITOR_NO_SELECTION ((size_t)-1)
#define EDITOR_MAX_FILE ((size_t)4 * 1024 * 1024)
#define EDITOR_CELL_CHARS 8

struct editor_history;

/*
 * The text is always NUL-terminated when text is non-NULL.  Cursor and
 * selection offsets are byte offsets.  lines contains the start of every
 * line, including the one empty line of an empty buffer.
 */
struct editor {
    char *text;
    size_t length, capacity;
    size_t *lines;
    int line_count;
    size_t cursor, anchor;
    unsigned long long revision, saved_revision;
    size_t top_line;
    int left_column, preferred_column;
    int preferred_valid;

    /* Private implementation details. */
    int line_capacity;
    struct editor_history *undo, *redo;
};

/* editor_init and editor_set_text establish an initially saved, empty undo
 * history.  set_text resets revision and saved_revision to zero on success,
 * preserves the old buffer on failure, and returns zero on success. */
void editor_init(struct editor *e);
int editor_set_text(struct editor *e, const char *data, size_t length);
void editor_free(struct editor *e);

/* Line numbers are clamped to the available range.  line_end excludes LF. */
size_t editor_line_start(const struct editor *e, int line);
size_t editor_line_end(const struct editor *e, int line);
/* Return zero on success; an out-of-range offset is clamped. */
int editor_position(const struct editor *e, size_t offset,
                    int *line, int *column);
size_t editor_offset_at(const struct editor *e, int line, int column);
/* Return one if the viewport changed, zero otherwise. */
int editor_scroll_visible(struct editor *e, int rows, int columns);

void editor_move_to(struct editor *e, size_t offset, int keep_selection);
void editor_move_left(struct editor *e, int keep_selection);
void editor_move_right(struct editor *e, int keep_selection);
void editor_move_up(struct editor *e, int keep_selection);
void editor_move_down(struct editor *e, int keep_selection);
void editor_move_home(struct editor *e, int keep_selection);
void editor_move_end(struct editor *e, int keep_selection);
void editor_move_page(struct editor *e, int lines, int keep_selection);
void editor_move_document(struct editor *e, int end, int keep_selection);
void editor_move_word(struct editor *e, int direction, int keep_selection);
void editor_goto_line(struct editor *e, int one_based_line, int keep_selection);
void editor_select_all(struct editor *e);

int editor_selection(const struct editor *e, size_t *start, size_t *end);
char *editor_copy_selection(const struct editor *e, size_t *length);

/* Editing functions return one for a change, zero for a no-op, and -1 on
 * error.  Inserting into a nonempty selection replaces it as one undo step. */
int editor_insert(struct editor *e, const char *text, size_t length);
int editor_backspace(struct editor *e);
int editor_delete(struct editor *e);
int editor_newline(struct editor *e);
int editor_undo(struct editor *e);
int editor_redo(struct editor *e);
void editor_mark_saved(struct editor *e);

/* Search is bytewise and case-sensitive.  A successful match is selected and
 * the cursor is placed at its end. */
long editor_find(struct editor *e, const char *needle);

/*
 * Display-cell helpers use the current C locale.  ed_text_cell returns the
 * bytes consumed, writes at most EDITOR_CELL_CHARS-1 wide characters, and
 * NUL-terminates chars.  Tabs occupy one returned byte and have width one;
 * line-oriented column helpers expand them to tab stops.  Invalid input is
 * represented by U+FFFD and consumes one byte.
 */
int ed_text_cell(const char *text, int length,
                 wchar_t chars[EDITOR_CELL_CHARS], int *width);
size_t ed_text_next(const char *text, size_t length, size_t position);
size_t ed_text_previous(const char *text, size_t position);
int ed_text_column(const char *text, size_t length, int tab_width);
size_t ed_text_offset(const char *text, size_t length, int column,
                      int tab_width);

#ifdef __cplusplus
}
#endif

#endif
