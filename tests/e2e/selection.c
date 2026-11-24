/* Keyboard and mouse selection, copy, cut and paste. */
#include <stdlib.h>
#include <string.h>

#include "../keys.h"
#include "../pty.h"
#include "../test.h"

/* Is screen cell (row, col) drawn in reverse video? 1-based. */
static int rev(quip *q, int row, int col) {
    return (qcell(q, row, col)->attr & ATTR_REVERSE) != 0;
}

static void check_saved(quip *q, const char *name, const char *want) {
    qkey(q, K_CTRL_S);
    char *saved = qread(q, name, NULL);
    CHECK_STR(saved, want);
    free(saved);
}

TEST(shift_right_selects_and_status_counts) {
    quip *q = qstart("a.txt", "hello world\n");
    for (int i = 0; i < 5; i++) qkey(q, K_SHIFT_RIGHT);
    for (int c = 5; c < 10; c++) CHECK(rev(q, 1, c));
    CHECK(!rev(q, 1, 10));
    const char *st = qstatus(q);
    CHECK(strstr(st, "5 selected  5:1") != NULL);
    qquit(q);
}

TEST(typing_replaces_selection) {
    quip *q = qstart("a.txt", "hello world\n");
    for (int i = 0; i < 5; i++) qkey(q, K_SHIFT_RIGHT);
    qtype(q, "J");
    CHECK_STR(qline(q, 1), "J world");
    check_saved(q, "a.txt", "J world\n");
    qquit(q);
}

TEST(backspace_deletes_multiline_selection) {
    quip *q = qstart("a.txt", "one\ntwo\nthree\n");
    qkey(q, K_RIGHT);
    qkey(q, K_SHIFT_DOWN);
    qkey(q, K_SHIFT_DOWN);
    /* The selected newline of row 1 fills to the screen edge. */
    CHECK(rev(q, 1, 80));
    qkey(q, K_BACKSPACE);
    CHECK_STR(qline(q, 1), "ohree");
    check_saved(q, "a.txt", "ohree\n");
    qquit(q);
}

TEST(escape_and_arrows_clear_selection) {
    quip *q = qstart("a.txt", "hello\n");
    qkey(q, K_SHIFT_RIGHT);
    CHECK(rev(q, 1, 5));
    qkey(q, K_ESC);
    CHECK(!rev(q, 1, 5));
    qkey(q, K_SHIFT_RIGHT);
    qkey(q, K_RIGHT);
    CHECK(!rev(q, 1, 6));
    qquit(q);
}

TEST(select_past_end_of_file_then_delete) {
    /* Regression: Shift+Down past the last line then Backspace crashed. */
    quip *q = qstart("a.txt", "ab\ncd\n");
    qkey(q, K_SHIFT_DOWN);
    qkey(q, K_SHIFT_DOWN);
    qkey(q, K_SHIFT_DOWN);
    qkey(q, K_BACKSPACE);
    check_saved(q, "a.txt", "\n");
    qquit(q);
}

TEST(word_and_line_selection_keys) {
    quip *q = qstart("a.txt", "foo bar baz\nnext\n");
    qkey(q, K_SHIFT_ALT_RIGHT);
    CHECK(strstr(qstatus(q), "3 selected") != NULL);
    qkey(q, K_SHIFT_CTRL_RIGHT);
    CHECK(strstr(qstatus(q), "11 selected") != NULL);
    qkey(q, K_SHIFT_CTRL_DOWN);
    CHECK(strstr(qstatus(q), "16 selected") != NULL);
    qquit(q);
}

TEST(copy_sends_osc52_and_paste_inserts) {
    quip *q = qstart("a.txt", "abc def\n");
    for (int i = 0; i < 3; i++) qkey(q, K_SHIFT_RIGHT);
    qkey(q, K_CTRL_C);
    CHECK_STR(q->scr.clipboard, "abc");
    CHECK_STR(qline(q, 1), "abc def");
    qkey(q, K_END);
    qkey(q, K_CTRL_V);
    CHECK_STR(qline(q, 1), "abc defabc");
    check_saved(q, "a.txt", "abc defabc\n");
    qquit(q);
}

TEST(cut_removes_and_copies) {
    quip *q = qstart("a.txt", "one\ntwo\n");
    qkey(q, K_SHIFT_DOWN);
    qkey(q, K_CTRL_X);
    CHECK_STR(q->scr.clipboard, "one\n");
    CHECK_STR(qline(q, 1), "two");
    qkey(q, K_DOWN);
    qkey(q, K_END);
    qkey(q, K_CTRL_V);
    check_saved(q, "a.txt", "two\none\n\n");
    qquit(q);
}

TEST(paste_from_terminal_clipboard) {
    quip *q = qstart("a.txt", "\n");
    free(q->scr.clipboard);
    q->scr.clipboard = strdup("from\nterminal");
    q->scr.clipboard_len = 13;
    qkey(q, K_CTRL_V);
    CHECK_STR(qline(q, 1), "from");
    CHECK_STR(qline(q, 2), "terminal");
    check_saved(q, "a.txt", "from\nterminal\n");
    qquit(q);
}

TEST(paste_falls_back_to_internal_clipboard) {
    /* A terminal that ignores OSC 52 queries: quip waits, then pastes what
     * it copied itself. */
    quip *q = qstart("a.txt", "xy\n");
    q->scr.answer_clipboard = 0;
    qkey(q, K_SHIFT_RIGHT);
    qkey(q, K_CTRL_C);
    qkey(q, K_END);
    qkey(q, K_CTRL_V);
    CHECK_STR(qline(q, 1), "xyx");
    qquit(q);
}

TEST(click_places_cursor) {
    quip *q = qstart("a.txt", "hello\nworld\n");
    qclick(q, 2, 8); /* gutter is 4 wide: column 8 is 'l' of world */
    CHECK_INT(q->scr.y, 1);
    CHECK_INT(q->scr.x, 7);
    CHECK(!rev(q, 2, 8));
    qquit(q);
}

TEST(click_below_last_line_goes_to_end) {
    quip *q = qstart("a.txt", "hello\nworld\n");
    qclick(q, 10, 20);
    CHECK_INT(q->scr.y, 1);
    CHECK_INT(q->scr.x, 4 + 5);
    qquit(q);
}

TEST(drag_selects) {
    quip *q = qstart("a.txt", "hello world\n");
    qpress(q, 1, 5);
    qdrag(q, 1, 8);
    qdrag(q, 1, 10);
    qrelease(q, 1, 10);
    CHECK(rev(q, 1, 5));
    CHECK(rev(q, 1, 9));
    CHECK(!rev(q, 1, 10));
    CHECK(strstr(qstatus(q), "5 selected") != NULL);
    qquit(q);
}

TEST(double_click_selects_word) {
    quip *q = qstart("a.txt", "foo bar_baz qux\n");
    qclick(q, 1, 10);
    qclick(q, 1, 10);
    CHECK(strstr(qstatus(q), "7 selected") != NULL);
    CHECK(rev(q, 1, 9) && rev(q, 1, 15) && !rev(q, 1, 16) && !rev(q, 1, 8));
    qquit(q);
}

TEST(triple_click_selects_line) {
    quip *q = qstart("a.txt", "first\nsecond\nthird\n");
    qclick(q, 2, 6);
    qclick(q, 2, 6);
    qclick(q, 2, 6);
    /* The line and its newline. */
    CHECK(strstr(qstatus(q), "7 selected") != NULL);
    qkey(q, K_CTRL_C);
    CHECK_STR(q->scr.clipboard, "second\n");
    qquit(q);
}

TEST(double_click_drag_extends_by_words) {
    quip *q = qstart("a.txt", "one two three four\n");
    qpress(q, 1, 5);
    qrelease(q, 1, 5);
    qpress(q, 1, 5);
    qdrag(q, 1, 14);
    qrelease(q, 1, 14);
    qkey(q, K_CTRL_C);
    CHECK_STR(q->scr.clipboard, "one two three");
    qquit(q);
}
