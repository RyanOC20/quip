/* Inserting, deleting, indenting and comment toggling. */
#include <stdlib.h>
#include <string.h>

#include "../keys.h"
#include "../pty.h"
#include "../test.h"

/* Save and compare the file with 'want'. */
static void check_saved(quip *q, const char *name, const char *want) {
    qkey(q, K_CTRL_S);
    char *saved = qread(q, name, NULL);
    CHECK_STR(saved, want);
    free(saved);
}

TEST(enter_splits_line) {
    quip *q = qstart("a.txt", "helloworld\n");
    for (int i = 0; i < 5; i++) qkey(q, K_RIGHT);
    qkey(q, K_ENTER);
    CHECK_STR(qline(q, 1), "hello");
    CHECK_STR(qline(q, 2), "world");
    CHECK_INT(q->scr.y, 1);
    CHECK_INT(q->scr.x, 4);
    check_saved(q, "a.txt", "hello\nworld\n");
    qquit(q);
}

TEST(backspace_joins_lines) {
    quip *q = qstart("a.txt", "ab\ncd\n");
    qkey(q, K_DOWN);
    qkey(q, K_BACKSPACE);
    CHECK_STR(qline(q, 1), "abcd");
    CHECK_STR(qline(q, 2), "~");
    CHECK_INT(q->scr.x, 4 + 2);
    check_saved(q, "a.txt", "abcd\n");
    qquit(q);
}

TEST(backspace_at_file_start_does_nothing) {
    quip *q = qstart("a.txt", "ab\n");
    qkey(q, K_BACKSPACE);
    CHECK_STR(qline(q, 1), "ab");
    check_saved(q, "a.txt", "ab\n");
    qquit(q);
}

TEST(delete_key_deletes_before_cursor) {
    /* quip's Delete key behaves like Backspace (inherited from kilo). */
    quip *q = qstart("a.txt", "abc\n");
    qkey(q, K_RIGHT);
    qkey(q, K_DEL);
    CHECK_STR(qline(q, 1), "bc");
    qquit(q);
}

TEST(tab_inserts_spaces_to_next_stop) {
    quip *q = qstart("a.txt", "x\n");
    qkey(q, K_TAB);
    CHECK_STR(qline(q, 1), "  x");
    qkey(q, K_RIGHT);
    qkey(q, K_TAB);
    check_saved(q, "a.txt", "  x \n");
    qquit(q);
}

TEST(hard_tabs_render_on_tab_stops) {
    /* Tabs display on QUIP_TAB_STOP (2) column stops. */
    quip *q = qstart("a.txt", "a\tb\n\tc\n");
    CHECK_STR(qline(q, 1), "a b");
    CHECK_STR(qline(q, 2), "  c");
    qkey(q, K_RIGHT);
    qkey(q, K_RIGHT);
    CHECK_INT(q->scr.x, 4 + 2);
    qquit(q);
}

TEST(shift_tab_outdents) {
    quip *q = qstart("a.txt", "    x\n y\n");
    qkey(q, K_SHIFT_TAB);
    CHECK_STR(qline(q, 1), "  x");
    qkey(q, K_SHIFT_TAB);
    CHECK_STR(qline(q, 1), "x");
    qkey(q, K_SHIFT_TAB);
    CHECK_STR(qline(q, 1), "x");
    /* A single leading space is less than a full indent: left alone. */
    qkey(q, K_DOWN);
    qkey(q, K_SHIFT_TAB);
    CHECK_STR(qline(q, 2), " y");
    check_saved(q, "a.txt", "x\n y\n");
    qquit(q);
}

TEST(toggle_line_comment_c) {
    quip *q = qstart("a.c", "int x;\n    int y;\n");
    qkey(q, K_CTRL_SLASH);
    CHECK_STR(qline(q, 1), "//int x;");
    qkey(q, K_CTRL_SLASH);
    CHECK_STR(qline(q, 1), "int x;");
    qkey(q, K_DOWN);
    qkey(q, K_CTRL_SLASH);
    CHECK_STR(qline(q, 2), "    //int y;");
    check_saved(q, "a.c", "int x;\n    //int y;\n");
    qquit(q);
}

TEST(toggle_line_comment_python) {
    quip *q = qstart("a.py", "x = 1\n");
    qkey(q, K_CTRL_SLASH);
    CHECK_STR(qline(q, 1), "#x = 1");
    qquit(q);
}

TEST(toggle_block_comment_ocaml) {
    quip *q = qstart("a.ml", "let x = 1\n");
    qkey(q, K_CTRL_SLASH);
    CHECK_STR(qline(q, 1), "(* let x = 1 *)");
    qkey(q, K_CTRL_SLASH);
    CHECK_STR(qline(q, 1), "let x = 1");
    qquit(q);
}

TEST(toggle_comment_without_language_does_nothing) {
    quip *q = qstart("a.txt", "plain\n");
    qkey(q, K_CTRL_SLASH);
    CHECK_STR(qline(q, 1), "plain");
    CHECK(!strstr(qstatus(q), "modified"));
    qquit(q);
}

TEST(typing_utf8_is_saved_intact) {
    quip *q = qstart("a.txt", "\n");
    qtype(q, "caf\xc3\xa9 \xe6\x97\xa5");
    check_saved(q, "a.txt", "caf\xc3\xa9 \xe6\x97\xa5\n");
    qquit(q);
}

TEST(file_without_final_newline_gains_one) {
    quip *q = qstart("a.txt", "a\nb");
    qtype(q, "x");
    check_saved(q, "a.txt", "xa\nb\n");
    qquit(q);
}

TEST(unbound_ctrl_keys_insert_nothing) {
    quip *q = qstart("a.txt", "x\n");
    qkey(q, "\x07"); /* Ctrl-G */
    qkey(q, "\x04"); /* Ctrl-D */
    qkey(q, "\x15"); /* Ctrl-U */
    qkey(q, "\x0b"); /* Ctrl-K */
    CHECK_STR(qline(q, 1), "x");
    CHECK(strstr(qstatus(q), "modified") == NULL);
    qquit(q);
}
