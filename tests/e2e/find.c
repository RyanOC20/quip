/* Incremental search (Ctrl-F). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../keys.h"
#include "../pty.h"
#include "../test.h"

TEST(find_prompt_and_first_match) {
    quip *q = qstart("a.c", "int alpha;\nint beta;\nint gamma;\n");
    qkey(q, K_CTRL_F);
    CHECK_STR(qmessage(q), "Search:  (Use ESC/Arrows/Enter)");
    qtype(q, "bet");
    CHECK_STR(qmessage(q), "Search: bet (Use ESC/Arrows/Enter)");
    /* The matching line scrolls to the top, the match is drawn blue and the
     * cursor sits on it. */
    CHECK_STR(qline(q, 1), "int beta;");
    CHECK_INT(qcell(q, 1, 9)->fg, 34);
    CHECK_INT(qcell(q, 1, 11)->fg, 34);
    CHECK_INT(q->scr.x, 4 + 4);
    qkey(q, K_ENTER);
    CHECK_STR(qmessage(q), "");
    /* The match color goes away, the cursor stays. */
    CHECK(qcell(q, 1, 9)->fg != 34);
    CHECK_INT(q->scr.x, 4 + 4);
    qquit(q);
}

TEST(find_escape_restores_cursor) {
    quip *q = qstart("a.txt", "one\ntwo\nthree\n");
    qkey(q, K_DOWN);
    qkey(q, K_RIGHT);
    qkey(q, K_CTRL_F);
    qtype(q, "thr");
    CHECK_STR(qline(q, 1), "three");
    qkey(q, K_ESC);
    CHECK_STR(qline(q, 1), "one");
    CHECK_INT(q->scr.y, 1);
    CHECK_INT(q->scr.x, 4 + 1);
    qquit(q);
}

TEST(find_arrows_cycle_matches) {
    quip *q = qstart("a.txt", "x1\nfoo a\nx2\nfoo b\nx3\n");
    qkey(q, K_CTRL_F);
    qtype(q, "foo");
    CHECK_STR(qline(q, 1), "foo a");
    qkey(q, K_DOWN);
    CHECK_STR(qline(q, 1), "foo b");
    qkey(q, K_DOWN); /* wraps around */
    CHECK_STR(qline(q, 1), "foo a");
    qkey(q, K_UP);
    CHECK_STR(qline(q, 1), "foo b");
    qkey(q, K_ENTER);
    qquit(q);
}

TEST(find_backspace_edits_query) {
    quip *q = qstart("a.txt", "abc\nabd\n");
    qkey(q, K_CTRL_F);
    qtype(q, "abd");
    CHECK_STR(qline(q, 1), "abd");
    qkey(q, K_BACKSPACE);
    CHECK_STR(qmessage(q), "Search: ab (Use ESC/Arrows/Enter)");
    qkey(q, K_ESC);
    qquit(q);
}

TEST(find_no_match_keeps_view) {
    quip *q = qstart("a.txt", "abc\n");
    qkey(q, K_CTRL_F);
    qtype(q, "zzz");
    CHECK_STR(qline(q, 1), "abc");
    qkey(q, K_ESC);
    qquit(q);
}
