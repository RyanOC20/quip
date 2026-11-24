/* Bugs found by the fuzzer, as it minimized them. */
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
