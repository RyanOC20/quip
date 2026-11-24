/* Bugs found by the fuzzer, as it minimized them. */
#include <stdio.h>

#include "../keys.h"
#include "../pty.h"
#include "../test.h"

TEST(selection_anchored_past_line_end_then_typed_over) {
    /* A press beyond the right edge anchored the selection past the end of
     * the line; replacing the selection wrote past the row buffer. */
    qopts o = {.rows = 11, .cols = 14};
    quip *q = qstart_opts("a.md", "# Title\n\nSome *text* <!-- c -->\n", &o);
    qkey(q, K_END);
    qpress(q, 2, 15);
    qkey(q, K_SHIFT_CTRL_UP);
    qkey(q, K_ESC);
    qkey(q, K_ESC);
    qquit(q);
}

TEST(selection_past_line_end_on_tiny_terminal) {
    qopts o = {.rows = 4, .cols = 5};
    quip *q = qstart_opts("a.txt",
        "plain text with trailing spaces   \ncaf\303\251 \346\227\245\346\234\254\n"
        "\tindented\na long line abcdefghijklmnopqrstuvwxyz abcdefghijklmnopqrstuvwxyz "
        "abcdefghijklmnopqrstuvwxyz abcdefghijklmnopqrstuvwxyz abcdefghij\n", &o);
    qkey(q, K_ENTER);
    qkey(q, "^");
    qkey(q, "-");
    qpress(q, 1, 6);
    qkey(q, "a");
    qquit(q);
}

TEST(left_at_top_of_scrolled_view) {
    /* Left at column 0 on the top screen row of a scrolled view moved the
     * cursor to screen row -1; Up then read before the row array. */
    char text[40 * 8 + 1];
    int o = 0;
    for (int i = 1; i <= 40; i++) o += sprintf(text + o, "line %d\n", i);
    quip *q = qstart("a.txt", text);
    qwheel(q, 0, 5, 10); /* view starts at line 4, cursor pulled onto it */
    CHECK_STR(qline(q, 1), "line 4");
    CHECK_INT(q->scr.y, 0);
    qkey(q, K_LEFT);
    /* The cursor goes to the end of line 3, scrolling it into view. */
    CHECK_STR(qline(q, 1), "line 3");
    CHECK_INT(q->scr.y, 0);
    CHECK_INT(q->scr.x, 4 + 6);
    qkey(q, K_UP);
    CHECK_STR(qline(q, 1), "line 2");
    CHECK_INT(q->scr.y, 0);
    qquit(q);
}

TEST(fuzz_seed_678) {
    qopts o = {.rows = 3, .cols = 19};
    quip *q = qstart_opts("a.py", "def f(a, b):\n    \"\"\"Doc\n    string\"\"\"\n"
                          "    return a + b  # sum\nx = '''t'''\n", &o);
    qkey(q, K_DOWN);
    qkey(q, ")");
    qkey(q, K_SHIFT_CTRL_LEFT);
    qkey(q, K_SHIFT_LEFT);
    qkey(q, K_SHIFT_UP);
    qquit(q);
}
