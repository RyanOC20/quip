/* The file tree sidebar (Ctrl-E). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../keys.h"
#include "../pty.h"
#include "../test.h"

/* The sidebar part of a screen row: its first 29 columns. */
static const char *side(quip *q, int row) {
    static char buf[256];
    screen_span_text(&q->scr, row - 1, 0, 29, buf, sizeof(buf));
    int n = (int)strlen(buf);
    while (n > 0 && buf[n - 1] == ' ') buf[--n] = '\0';
    return buf;
}

static quip *start_project(void) {
    quip *q = qstart("main.c", "int main;\n");
    qwrite(q, "Zeta.txt", "zeta\n");
    qwrite(q, "alpha.txt", "alpha\n");
    qwrite(q, "src/lib.c", "lib\n");
    qwrite(q, "src/deep/x.h", "x\n");
    return q;
}

TEST(tree_opens_with_directories_first) {
    quip *q = start_project();
    qkey(q, K_CTRL_E);
    /* The root is named after the working directory (the session dir). */
    CHECK(side(q, 1)[0] == '-');
    CHECK_STR(side(q, 2), "  + src");
    CHECK_STR(side(q, 3), "    alpha.txt");
    CHECK_STR(side(q, 4), "    main.c");
    CHECK_STR(side(q, 5), "    Zeta.txt");
    CHECK_INT(qcell(q, 1, 30)->cp, '|');
    /* Text starts after the divider and the gutter. */
    CHECK(strstr(qrow(q, 1), "|  1 int main;") != NULL);
    /* The selected row is reversed; directories are bold blue. */
    CHECK(qcell(q, 1, 1)->attr & ATTR_REVERSE);
    CHECK_INT(qcell(q, 2, 5)->fg, 34);
    qquit(q);
}

TEST(tree_expand_collapse_and_open) {
    quip *q = start_project();
    qkey(q, K_CTRL_E);
    qkey(q, K_DOWN);
    qkey(q, K_ENTER);
    CHECK_STR(side(q, 2), "  - src");
    CHECK_STR(side(q, 3), "    + deep");
    CHECK_STR(side(q, 4), "      lib.c");
    qkey(q, K_LEFT);
    CHECK_STR(side(q, 2), "  + src");
    qkey(q, K_RIGHT);
    qkey(q, K_DOWN);
    qkey(q, K_DOWN);
    qkey(q, K_ENTER); /* open src/lib.c */
    CHECK(strstr(qrow(q, 1), "|  1 lib") != NULL);
    CHECK(strncmp(qstatus(q) + 30, "lib.c", 5) == 0 || strstr(qstatus(q), "src/lib.c") != NULL);
    /* Focus is back in the editor: typing edits the new file. */
    qtype(q, "x");
    qkey(q, K_CTRL_S);
    char *saved = qread(q, "src/lib.c", NULL);
    CHECK_STR(saved, "xlib\n");
    free(saved);
    qquit(q);
}

TEST(tree_escape_returns_focus_and_ctrl_e_closes) {
    quip *q = start_project();
    qkey(q, K_CTRL_E);
    qkey(q, K_ESC);
    qtype(q, "a");
    CHECK(strstr(qrow(q, 1), "|  1 aint main;") != NULL);
    qkey(q, K_CTRL_E); /* refocus the tree */
    qkey(q, K_CTRL_E); /* close it */
    CHECK_STR(qline(q, 1), "aint main;");
    qquit(q);
}

TEST(tree_open_with_unsaved_changes_asks) {
    quip *q = start_project();
    qtype(q, "x");
    qkey(q, K_CTRL_E);
    qkey(q, K_DOWN);
    qkey(q, K_DOWN); /* alpha.txt */
    qkey(q, K_ENTER);
    CHECK(strstr(qmessage(q), "Discard unsaved changes? (y/N)") != NULL);
    qkey(q, "n");
    CHECK(strstr(qrow(q, 1), "xint main;") != NULL);
    qkey(q, K_ENTER);
    qkey(q, "y");
    CHECK(strstr(qrow(q, 1), "|  1 alpha") != NULL);
    qquit(q);
}

TEST(tree_refuses_narrow_terminal) {
    qopts o = {.rows = 10, .cols = 45};
    quip *q = qstart_opts("a.txt", "x\n", &o);
    qkey(q, K_CTRL_E);
    CHECK_STR(qmessage(q), "Terminal too narrow for the file tree");
    qquit(q);
}

TEST(tree_click_selects_and_opens) {
    quip *q = start_project();
    qkey(q, K_CTRL_E);
    qclick(q, 3, 8); /* alpha.txt */
    CHECK(strstr(qrow(q, 1), "|  1 alpha") != NULL);
    qquit(q);
}

TEST(tree_focused_click_in_text_focuses_editor) {
    quip *q = start_project();
    qkey(q, K_CTRL_E);
    qclick(q, 1, 40); /* text column 6 of line 1 */
    CHECK_INT(q->scr.y, 0);
    CHECK_INT(q->scr.x, 39);
    qtype(q, "X");
    CHECK(strstr(qrow(q, 1), "|  1 int mXain;") != NULL);
    qquit(q);
}
