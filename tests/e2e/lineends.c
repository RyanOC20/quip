/* Line endings: a file saves with the line endings it was opened with,
 * CRLF or LF, and keeps a missing newline at the end missing. */
#include <stdlib.h>
#include <string.h>

#include "../keys.h"
#include "../pty.h"
#include "../test.h"

static void check_saved(quip *q, const char *name, const char *want) {
    qkey(q, K_CTRL_S);
    char *saved = qread(q, name, NULL);
    CHECK_STR(saved, want);
    free(saved);
}

TEST(crlf_file_shows_plain_lines_and_saves_crlf) {
    quip *q = qstart("a.txt", "one\r\ntwo\r\n");
    CHECK_STR(qline(q, 1), "one");
    CHECK_STR(qline(q, 2), "two");
    CHECK(strstr(qstatus(q), "CRLF") != NULL);
    qtype(q, "x\n");
    check_saved(q, "a.txt", "x\r\none\r\ntwo\r\n");
    qquit(q);
}

TEST(lf_file_does_not_say_crlf) {
    quip *q = qstart("a.txt", "one\ntwo\n");
    CHECK(strstr(qstatus(q), "CRLF") == NULL);
    qquit(q);
}

TEST(mostly_crlf_file_saves_all_crlf) {
    quip *q = qstart("a.txt", "a\r\nb\nc\r\n");
    CHECK_STR(qline(q, 2), "b");
    qtype(q, "x");
    check_saved(q, "a.txt", "xa\r\nb\r\nc\r\n");
    qquit(q);
}

TEST(mostly_lf_file_keeps_a_stray_cr_visible) {
    /* In an LF file a CR before a newline is part of the line: it shows as
     * a reversed M, the caret notation of ^M, and is saved as it was. */
    quip *q = qstart("a.txt", "a\r\nb\nc\n");
    CHECK_STR(qline(q, 1), "aM");
    CHECK(qcell(q, 1, 6)->attr & ATTR_REVERSE);
    qtype(q, "x");
    check_saved(q, "a.txt", "xa\r\nb\nc\n");
    qquit(q);
}

TEST(missing_final_newline_stays_missing) {
    quip *q = qstart("a.txt", "a\nb");
    qtype(q, "x");
    check_saved(q, "a.txt", "xa\nb");
    qquit(q);
}

TEST(missing_final_newline_in_a_crlf_file) {
    quip *q = qstart("a.txt", "a\r\nb");
    CHECK_STR(qline(q, 2), "b");
    qtype(q, "x");
    check_saved(q, "a.txt", "xa\r\nb");
    qquit(q);
}

TEST(new_lines_typed_at_the_end_keep_the_file_without_newline) {
    quip *q = qstart("a.txt", "a");
    qkey(q, K_END);
    qtype(q, "\nb");
    check_saved(q, "a.txt", "a\nb");
    qquit(q);
}
