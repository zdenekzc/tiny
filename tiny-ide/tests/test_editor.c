#include "editor.h"

#include <errno.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static int tests_run;

#define ASSERT(condition)                                                     \
    do {                                                                      \
        if (!(condition)) {                                                   \
            fprintf(stderr, "ASSERT failed at %s:%d: %s\n", __FILE__,       \
                    __LINE__, #condition);                                    \
            exit(1);                                                          \
        }                                                                     \
    } while (0)

#define SET_TEXT(editor, literal)                                             \
    ASSERT(editor_set_text((editor), (literal), sizeof(literal) - 1) == 0)

#define RUN_TEST(function)                                                    \
    do {                                                                      \
        ++tests_run;                                                          \
        function();                                                           \
    } while (0)

static void assert_text(const struct editor *e, const char *expected)
{
    size_t length = strlen(expected);
    ASSERT(e->length == length);
    ASSERT(memcmp(e->text, expected, length) == 0);
    ASSERT(e->text[length] == '\0');
}

static void test_empty_and_line_index(void)
{
    struct editor e;

    editor_init(&e);
    ASSERT(e.text != NULL);
    ASSERT(e.length == 0);
    ASSERT(e.capacity >= 1);
    ASSERT(e.text[0] == '\0');
    ASSERT(e.lines != NULL);
    ASSERT(e.line_count == 1);
    ASSERT(e.lines[0] == 0);
    ASSERT(e.anchor == EDITOR_NO_SELECTION);
    ASSERT(editor_line_start(&e, 99) == 0);
    ASSERT(editor_line_end(&e, -1) == 0);
    ASSERT(editor_position(&e, 123, NULL, NULL) == 0);
    ASSERT(editor_offset_at(&e, 50, 50) == 0);
    ASSERT(editor_undo(&e) == 0);
    ASSERT(editor_redo(&e) == 0);
    editor_free(&e);
    ASSERT(e.text == NULL);
    ASSERT(e.lines == NULL);
}

static void test_utf8_cells_and_columns(void)
{
    static const char text[] = "aé́\t中";
    static const char many_marks[] = "á́́́́́́́́";
    wchar_t chars[EDITOR_CELL_CHARS];
    int width;
    int used;

    ASSERT(setlocale(LC_ALL, "C.UTF-8") != NULL || MB_CUR_MAX > 1);
    ASSERT(ed_text_next(text, sizeof(text) - 1, 0) == 1);
    ASSERT(ed_text_next(text, sizeof(text) - 1, 1) == 5);
    used = ed_text_cell(text + 1, 4, chars, &width);
    ASSERT(used == 4);
    ASSERT(width == 1);
    ASSERT(chars[0] == L'é');
    ASSERT(chars[1] == L'́');
    ASSERT(chars[2] == L'\0');
    ASSERT(ed_text_next(text, sizeof(text) - 1, 5) == 6);
    ASSERT(ed_text_next(text, sizeof(text) - 1, 6) == 9);
    ASSERT(ed_text_previous(text, 9) == 6);
    ASSERT(ed_text_previous(text, 6) == 5);
    ASSERT(ed_text_previous(text, 5) == 1);
    ASSERT(ed_text_previous(text, 1) == 0);
    ASSERT(ed_text_column(text, sizeof(text) - 1, 4) == 6);
    ASSERT(ed_text_offset(text, sizeof(text) - 1, 3, 4) == 5);
    ASSERT(ed_text_offset(text, sizeof(text) - 1, 4, 4) == 6);
    ASSERT(ed_text_offset(text, sizeof(text) - 1, 5, 4) == 6);
    ASSERT(ed_text_offset(text, sizeof(text) - 1, 6, 4) == 9);

    used = ed_text_cell("\xff", 1, chars, &width);
    ASSERT(used == 1);
    ASSERT(width == 1);
    ASSERT(chars[0] == 0xfffd);
    used = ed_text_cell(many_marks, (int)sizeof(many_marks) - 1,
                        chars, &width);
    ASSERT(used == (int)sizeof(many_marks) - 1);
    ASSERT(chars[0] == L'a');
    ASSERT(chars[6] == L'́');
    ASSERT(chars[7] == L'\0');
    ASSERT(ed_text_previous(many_marks, sizeof(many_marks) - 1) == 0);
}

static void test_insert_and_multibyte_editing(void)
{
    struct editor e;

    editor_init(&e);
    ASSERT(editor_insert(&e, "a\n\né́b", 8) == 1);
    assert_text(&e, "a\n\né́b");
    ASSERT(e.revision == 1);
    ASSERT(e.line_count == 3);
    ASSERT(editor_line_start(&e, 0) == 0);
    ASSERT(editor_line_start(&e, 1) == 2);
    ASSERT(editor_line_start(&e, 2) == 3);
    ASSERT(editor_line_start(&e, 3) == 3);
    ASSERT(e.cursor == e.length);
    editor_move_left(&e, 0);
    ASSERT(e.cursor == 7);
    ASSERT(editor_backspace(&e) == 1);
    assert_text(&e, "a\n\nb");
    ASSERT(e.line_count == 3);
    ASSERT(editor_backspace(&e) == 1);
    assert_text(&e, "a\nb");
    ASSERT(e.line_count == 2);
    editor_free(&e);
}

static void test_movement_lines_and_pages(void)
{
    struct editor e;
    int line, column;

    editor_init(&e);
    SET_TEXT(&e, "one\ntwo\nthree\nfour\nfive");
    editor_move_to(&e, 2, 0);
    editor_move_down(&e, 0);
    ASSERT(e.cursor == 6);
    editor_move_down(&e, 0);
    ASSERT(e.cursor == 10);
    editor_move_up(&e, 0);
    ASSERT(e.cursor == 6);
    editor_move_end(&e, 0);
    ASSERT(e.cursor == 7);
    editor_move_right(&e, 0);
    ASSERT(e.cursor == 8);
    editor_move_left(&e, 0);
    ASSERT(e.cursor == 7);
    editor_goto_line(&e, 5, 0);
    editor_move_page(&e, -2, 0);
    ASSERT(e.cursor == 8);
    editor_move_document(&e, 1, 0);
    ASSERT(e.cursor == e.length);
    ASSERT(editor_position(&e, e.cursor, &line, &column) == 0);
    ASSERT(line == 4 && column == 4);
    editor_move_home(&e, 0);
    ASSERT(e.cursor == 19);
    editor_goto_line(&e, 999, 0);
    ASSERT(e.cursor == 19);
    editor_goto_line(&e, 0, 0);
    ASSERT(e.cursor == 0);

    SET_TEXT(&e, "abcdef\nx\nabcdef");
    editor_move_end(&e, 0);
    editor_move_down(&e, 0);
    ASSERT(e.cursor == 8);
    editor_move_down(&e, 0);
    ASSERT(e.cursor == 15);
    editor_move_up(&e, 0);
    ASSERT(e.cursor == 8);
    editor_move_left(&e, 0);
    ASSERT(e.preferred_valid == 0);
    editor_free(&e);
}

static void test_word_movement_and_selection_motion(void)
{
    struct editor e;
    size_t start, end;

    editor_init(&e);
    SET_TEXT(&e, "one, two\nthree");
    editor_move_word(&e, 1, 0);
    ASSERT(e.cursor == 3);
    editor_move_word(&e, 1, 0);
    ASSERT(e.cursor == 4);
    editor_move_word(&e, 1, 0);
    ASSERT(e.cursor == 8);
    editor_move_word(&e, -1, 1);
    ASSERT(e.cursor == 5);
    ASSERT(editor_selection(&e, &start, &end) == 1);
    ASSERT(start == 5 && end == 8);
    editor_move_word(&e, -1, 0);
    ASSERT(e.cursor == 3);
    ASSERT(editor_selection(&e, NULL, NULL) == 0);
    editor_move_word(&e, -1, 0);
    ASSERT(e.cursor == 0);
    editor_select_all(&e);
    ASSERT(editor_selection(&e, &start, &end) == 1);
    ASSERT(start == 0 && end == e.length);
    editor_free(&e);
}

static void test_newline_autoindent(void)
{
    struct editor e;

    editor_init(&e);
    SET_TEXT(&e, "  if (x) {  ");
    editor_move_document(&e, 1, 0);
    ASSERT(editor_newline(&e) == 1);
    assert_text(&e, "  if (x) {  \n      ");
    ASSERT(e.line_count == 2);
    ASSERT(e.cursor == e.length);
    ASSERT(editor_undo(&e) == 1);
    assert_text(&e, "  if (x) {  ");

    SET_TEXT(&e, "\t  value");
    editor_move_document(&e, 1, 0);
    ASSERT(editor_newline(&e) == 1);
    assert_text(&e, "\t  value\n\t  ");
    SET_TEXT(&e, "  value");
    editor_move_to(&e, 1, 0);
    ASSERT(editor_newline(&e) == 1);
    assert_text(&e, " \n  value");
    editor_free(&e);
}

static void test_selection_copy_and_replacement(void)
{
    struct editor e;
    char *copy;
    size_t length, start, end;

    editor_init(&e);
    SET_TEXT(&e, "hello world");
    ASSERT(editor_copy_selection(&e, &length) == NULL);
    ASSERT(length == 0);
    editor_move_to(&e, 6, 0);
    editor_move_to(&e, 11, 1);
    copy = editor_copy_selection(&e, &length);
    ASSERT(copy != NULL);
    ASSERT(length == 5);
    ASSERT(strcmp(copy, "world") == 0);
    free(copy);
    ASSERT(editor_insert(&e, "there", 5) == 1);
    assert_text(&e, "hello there");
    ASSERT(e.revision == 1);
    ASSERT(e.cursor == 11);
    ASSERT(editor_selection(&e, NULL, NULL) == 0);
    ASSERT(editor_undo(&e) == 1);
    assert_text(&e, "hello world");
    ASSERT(editor_selection(&e, &start, &end) == 1);
    ASSERT(start == 6 && end == 11);
    ASSERT(editor_redo(&e) == 1);
    assert_text(&e, "hello there");
    editor_free(&e);
}

static void test_backspace_delete_and_newline(void)
{
    struct editor e;

    editor_init(&e);
    SET_TEXT(&e, "a\nb");
    editor_move_to(&e, 2, 0);
    ASSERT(editor_backspace(&e) == 1);
    assert_text(&e, "ab");
    ASSERT(e.line_count == 1);

    SET_TEXT(&e, "aé́b");
    editor_move_to(&e, 5, 0);
    ASSERT(editor_backspace(&e) == 1);
    assert_text(&e, "ab");
    editor_move_document(&e, 0, 0);
    ASSERT(editor_delete(&e) == 1);
    assert_text(&e, "b");
    editor_move_document(&e, 1, 0);
    ASSERT(editor_delete(&e) == 0);
    ASSERT(editor_backspace(&e) == 1);
    ASSERT(editor_backspace(&e) == 0);

    SET_TEXT(&e, "ab\ncd");
    editor_move_to(&e, 1, 0);
    editor_move_to(&e, 4, 1);
    ASSERT(editor_delete(&e) == 1);
    assert_text(&e, "ad");
    ASSERT(e.line_count == 1);
    editor_free(&e);
}

static void test_undo_redo_and_saved_revision(void)
{
    struct editor e;

    editor_init(&e);
    ASSERT(editor_insert(&e, "abc", 3) == 1);
    editor_mark_saved(&e);
    ASSERT(e.revision == 1 && e.saved_revision == 1);
    ASSERT(e.revision == e.saved_revision);
    ASSERT(editor_insert(&e, "d", 1) == 1);
    ASSERT(e.revision != e.saved_revision);
    ASSERT(e.cursor == 4);
    ASSERT(editor_undo(&e) == 1);
    assert_text(&e, "abc");
    ASSERT(e.revision == 1 && e.revision == e.saved_revision);
    ASSERT(e.cursor == 3);
    ASSERT(editor_undo(&e) == 1);
    assert_text(&e, "");
    ASSERT(e.revision == 0 && e.revision != e.saved_revision);
    ASSERT(editor_undo(&e) == 0);
    ASSERT(editor_redo(&e) == 1);
    assert_text(&e, "abc");
    ASSERT(e.revision == e.saved_revision);
    ASSERT(editor_redo(&e) == 1);
    assert_text(&e, "abcd");
    ASSERT(e.cursor == 4);
    ASSERT(editor_redo(&e) == 0);

    ASSERT(editor_undo(&e) == 1);
    ASSERT(editor_insert(&e, "X", 1) == 1);
    assert_text(&e, "abcX");
    ASSERT(editor_redo(&e) == 0);
    ASSERT(e.revision == 2);
    ASSERT(editor_undo(&e) == 1);
    assert_text(&e, "abc");
    editor_free(&e);
}

static void test_set_text_validation_and_crlf(void)
{
    struct editor e;
    char *large;

    editor_init(&e);
    SET_TEXT(&e, "a\r\nb\r\nc\r");
    assert_text(&e, "a\nb\nc\r");
    ASSERT(e.line_count == 3);
    ASSERT(editor_line_start(&e, 1) == 2);
    ASSERT(editor_line_start(&e, 2) == 4);
    errno = 0;
    ASSERT(editor_set_text(&e, "bad\0text", 8) == -1);
    ASSERT(errno == EINVAL);
    assert_text(&e, "a\nb\nc\r");
    large = malloc(EDITOR_MAX_FILE + 1);
    ASSERT(large != NULL);
    memset(large, 'x', EDITOR_MAX_FILE + 1);
    errno = 0;
    ASSERT(editor_set_text(&e, large, EDITOR_MAX_FILE + 1) == -1);
    ASSERT(errno == EFBIG);
    free(large);
    assert_text(&e, "a\nb\nc\r");
    ASSERT(editor_set_text(&e, NULL, 0) == 0);
    assert_text(&e, "");
    ASSERT(e.revision == 0 && e.saved_revision == 0);
    ASSERT(e.undo == NULL && e.redo == NULL);
    editor_free(&e);
}

static void test_find_wraparound(void)
{
    struct editor e;
    size_t start, end;

    editor_init(&e);
    SET_TEXT(&e, "one two one two");
    ASSERT(editor_find(&e, "") == -1);
    ASSERT(editor_find(&e, "ONE") == -1);
    ASSERT(editor_find(&e, "one") == 0);
    ASSERT(editor_selection(&e, &start, &end) == 1);
    ASSERT(start == 0 && end == 3 && e.cursor == 3);
    ASSERT(editor_find(&e, "one") == 8);
    ASSERT(editor_find(&e, "one") == 0);
    ASSERT(editor_find(&e, "two") == 4);
    ASSERT(e.revision == 0);
    editor_find(&e, "one");
    /* A backwards selection over the current match must still advance. */
    {
        size_t match_start = e.anchor;
        e.anchor = e.cursor;
        editor_move_to(&e, match_start, 1);
    }
    ASSERT(editor_find(&e, "one") == 0);
    ASSERT(editor_find(&e, "absent") == -1);
    editor_free(&e);
}

static void test_offset_mapping_and_scroll(void)
{
    struct editor e;
    int line, column;

    editor_init(&e);
    SET_TEXT(&e, "\tAé́\nxy");
    ASSERT(editor_offset_at(&e, 0, 0) == 0);
    ASSERT(editor_offset_at(&e, 0, 3) == 0);
    ASSERT(editor_offset_at(&e, 0, 4) == 1);
    ASSERT(editor_offset_at(&e, 0, 5) == 2);
    ASSERT(editor_offset_at(&e, 0, 6) == 6);
    ASSERT(editor_offset_at(&e, 0, 100) == 6);
    ASSERT(editor_offset_at(&e, 1, 1) == 8);
    ASSERT(editor_offset_at(&e, 999, 999) == e.length);
    ASSERT(editor_position(&e, 1, &line, &column) == 0);
    ASSERT(line == 0 && column == 4);
    ASSERT(editor_position(&e, 2, &line, &column) == 0);
    ASSERT(line == 0 && column == 5);
    ASSERT(editor_position(&e, 7, &line, &column) == 0);
    ASSERT(line == 1 && column == 0);

    SET_TEXT(&e, "abcdef");
    editor_move_document(&e, 1, 0);
    ASSERT(editor_scroll_visible(&e, 1, 3) == 1);
    ASSERT(e.left_column == 4);
    ASSERT(editor_scroll_visible(&e, 1, 3) == 0);
    SET_TEXT(&e, "0\n1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n");
    editor_goto_line(&e, 8, 0);
    ASSERT(editor_scroll_visible(&e, 4, 10) == 1);
    ASSERT(e.top_line == 4);
    editor_move_document(&e, 0, 0);
    ASSERT(editor_scroll_visible(&e, 4, 10) == 1);
    ASSERT(e.top_line == 0);
    editor_free(&e);
}

int main(void)
{
    (void)setlocale(LC_ALL, "");

    RUN_TEST(test_empty_and_line_index);
    RUN_TEST(test_utf8_cells_and_columns);
    RUN_TEST(test_insert_and_multibyte_editing);
    RUN_TEST(test_movement_lines_and_pages);
    RUN_TEST(test_word_movement_and_selection_motion);
    RUN_TEST(test_newline_autoindent);
    RUN_TEST(test_selection_copy_and_replacement);
    RUN_TEST(test_backspace_delete_and_newline);
    RUN_TEST(test_undo_redo_and_saved_revision);
    RUN_TEST(test_set_text_validation_and_crlf);
    RUN_TEST(test_find_wraparound);
    RUN_TEST(test_offset_mapping_and_scroll);

    printf("%d tests passed\n", tests_run);
    return 0;
}
