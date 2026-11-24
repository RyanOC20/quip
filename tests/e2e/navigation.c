/* Cursor motion, paging and scrolling. */
#include <stdio.h>
#include <stdlib.h>

#include "../keys.h"
#include "../pty.h"
#include "../test.h"

/* A file of 'n' lines "line 1", "line 2", ... */
static char *numbered(int n) {
    char *buf = malloc(n * 16 + 1);
    int o = 0;
    for (int i = 1; i <= n; i++) o += sprintf(buf + o, "line %d\n", i);
    return buf;
}

TEST(arrows_move_and_wrap_lines) {
    quip *q = qstart("a.txt", "ab\ncd\n");
    qkey(q, K_RIGHT);
    qkey(q, K_RIGHT);
    qkey(q, K_RIGHT); /* past the end: to the next line */
    CHECK_INT(q->scr.y, 1);
    CHECK_INT(q->scr.x, 4);
    qkey(q, K_LEFT); /* back to the end of the previous line */
    CHECK_INT(q->scr.y, 0);
    CHECK_INT(q->scr.x, 6);
    qquit(q);
}

TEST(up_down_clamp_to_line_length) {
    quip *q = qstart("a.txt", "long line\nab\nlonger line\n");
    qkey(q, K_END);
    CHECK_INT(q->scr.x, 4 + 9);
    qkey(q, K_DOWN);
    CHECK_INT(q->scr.x, 4 + 2);
    qquit(q);
}

TEST(home_end_and_ctrl_arrows) {
    quip *q = qstart("a.txt", "hello world\nsecond\n");
    qkey(q, K_END);
    CHECK_INT(q->scr.x, 4 + 11);
    qkey(q, K_HOME);
    CHECK_INT(q->scr.x, 4);
    qkey(q, K_CTRL_RIGHT);
    CHECK_INT(q->scr.x, 4 + 11);
    qkey(q, K_CTRL_LEFT);
    CHECK_INT(q->scr.x, 4);
    qkey(q, K_CTRL_DOWN);
    CHECK_INT(q->scr.y, 1);
    qkey(q, K_CTRL_UP);
    CHECK_INT(q->scr.y, 0);
    CHECK_INT(q->scr.x, 4);
    qquit(q);
}

TEST(alt_arrows_move_by_word) {
    quip *q = qstart("a.txt", "foo bar.baz qux\n");
    qkey(q, K_ALT_RIGHT);
    CHECK_INT(q->scr.x, 4 + 3);
    qkey(q, K_ALT_RIGHT);
    CHECK_INT(q->scr.x, 4 + 7);
    qkey(q, K_META_F); /* ESC f, as macOS terminals send */
    CHECK_INT(q->scr.x, 4 + 11);
    qkey(q, K_ALT_LEFT);
    CHECK_INT(q->scr.x, 4 + 8);
    qkey(q, K_META_B);
    CHECK_INT(q->scr.x, 4 + 4);
    qquit(q);
}

TEST(page_down_and_up) {
    char *text = numbered(100);
    quip *q = qstart("a.txt", text);
    free(text);
    qkey(q, K_PGDN);
    /* The cursor goes to the bottom row, then a screen further. */
    CHECK_STR(qline(q, 22), "line 44");
    CHECK_INT(q->scr.y, 21);
    qkey(q, K_PGUP);
    CHECK_INT(q->scr.y, 0);
    CHECK_STR(qline(q, 1), "line 1");
    qquit(q);
}

TEST(page_down_in_short_file_then_left) {
    /* Regression: PgDn parked the cursor far past the end of a short file
     * and Left then read rows out of bounds. */
    quip *q = qstart("a.txt", "one\ntwo\n");
    qkey(q, K_PGDN);
    CHECK_INT(q->scr.y, 2);
    qkey(q, K_LEFT);
    CHECK_INT(q->scr.y, 1);
    CHECK_INT(q->scr.x, 4 + 3);
    qquit(q);
}

TEST(cursor_down_scrolls_view) {
    char *text = numbered(40);
    quip *q = qstart("a.txt", text);
    free(text);
    for (int i = 0; i < 25; i++) qkey(q, K_DOWN);
    CHECK_INT(q->scr.y, 21);
    CHECK_STR(qline(q, 22), "line 26");
    CHECK_STR(qline(q, 1), "line 5");
    qquit(q);
}

TEST(wheel_scrolls_three_lines) {
    char *text = numbered(40);
    quip *q = qstart("a.txt", text);
    free(text);
    qwheel(q, 0, 5, 10);
    CHECK_STR(qline(q, 1), "line 4");
    qwheel(q, 1, 5, 10);
    CHECK_STR(qline(q, 1), "line 1");
    qquit(q);
}

TEST(long_line_scrolls_horizontally) {
    char line[202];
    for (int i = 0; i < 200; i++) line[i] = 'a' + i % 26;
    line[200] = '\n';
    line[201] = '\0';
    quip *q = qstart("a.txt", line);
    qkey(q, K_END);
    /* Cursor at the end, in the last text column or before it. */
    CHECK(q->scr.x <= 79);
    char want[80];
    snprintf(want, sizeof(want), "%.20s", line + 200 - 20);
    const char *shown = qline(q, 1);
    CHECK(strlen(shown) >= 20);
    CHECK(strstr(shown, want) != NULL);
    qkey(q, K_HOME);
    CHECK_INT(q->scr.x, 4);
    CHECK(strncmp(qline(q, 1), "abcdef", 6) == 0);
    qquit(q);
}

TEST(status_shows_line_and_column) {
    /* line:column, both counted from 1, as compilers and grep print them. */
    quip *q = qstart("a.txt", "abc\ndef\n");
    qkey(q, K_DOWN);
    qkey(q, K_RIGHT);
    qkey(q, K_RIGHT);
    const char *st = qstatus(q);
    CHECK(strlen(st) > 4);
    CHECK_STR(st + strlen(st) - 3, "2:3");
    qquit(q);
}
