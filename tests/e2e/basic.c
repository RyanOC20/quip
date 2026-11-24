/* Opening, typing, saving and quitting. */
#include <stdlib.h>

#include "../keys.h"
#include "../pty.h"
#include "../test.h"

TEST(open_shows_file_and_status) {
    quip *q = qstart("a.txt", "hello\nworld\n");
    CHECK_STR(qrow(q, 1), "  1 hello");
    CHECK_STR(qrow(q, 2), "  2 world");
    CHECK_STR(qline(q, 3), "~");
    CHECK_STR(qstatus(q), "a.txt - 2 lines                                                              1:1");
    /* The cursor sits on the first text column, past the gutter. */
    CHECK_INT(q->scr.y, 0);
    CHECK_INT(q->scr.x, 4);
    CHECK_INT(q->scr.cursor_shape, 5);
    CHECK(q->scr.alt);
    qquit(q);
}

TEST(type_and_save) {
    quip *q = qstart("a.txt", "world\n");
    qtype(q, "hello ");
    CHECK_STR(qline(q, 1), "hello world");
    CHECK_STR(qstatus(q), "a.txt - 1 line (modified)                                                    1:7");
    qkey(q, K_CTRL_S);
    CHECK_STR(qmessage(q), "12 bytes written on disk");
    int len;
    char *saved = qread(q, "a.txt", &len);
    CHECK_STR(saved, "hello world\n");
    free(saved);
    qquit(q);
}

TEST(new_file_is_created_on_save) {
    quip *q = qstart("new.txt", NULL);
    CHECK(qread(q, "new.txt", NULL) == NULL);
    qtype(q, "one\ntwo");
    qkey(q, K_CTRL_S);
    char *saved = qread(q, "new.txt", NULL);
    CHECK_STR(saved, "one\ntwo\n");
    free(saved);
    qquit(q);
}

TEST(quit_needs_four_presses_when_modified) {
    quip *q = qstart("a.txt", "x\n");
    qtype(q, "y");
    qkey(q, K_CTRL_Q);
    CHECK(!q->exited);
    CHECK_STR(qmessage(q),
              "WARNING!!! File has unsaved changes. Press Ctrl-Q 3 more times to quit.");
    qquit(q);
    /* Nothing was saved. */
}
