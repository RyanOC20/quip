/* What quip draws: gutter, syntax colors, trailing spaces, brackets,
 * control characters and line widths. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../keys.h"
#include "../pty.h"
#include "../test.h"

static int fg(quip *q, int row, int col) { return qcell(q, row, col)->fg; }
static int bg(quip *q, int row, int col) { return qcell(q, row, col)->bg; }

TEST(gutter_marks_current_line) {
    quip *q = qstart("a.txt", "a\nb\n");
    CHECK(qcell(q, 1, 3)->attr & ATTR_BOLD);
    CHECK_INT(fg(q, 2, 3), 90);
    qkey(q, K_DOWN);
    CHECK_INT(fg(q, 1, 3), 90);
    CHECK(qcell(q, 2, 3)->attr & ATTR_BOLD);
    qquit(q);
}

TEST(gutter_widens_with_line_count) {
    char *text = malloc(1000 * 2 + 1);
    for (int i = 0; i < 1000; i++) {
        text[2 * i] = 'x';
        text[2 * i + 1] = '\n';
    }
    text[2000] = '\0';
    quip *q = qstart("a.txt", text);
    free(text);
    CHECK_STR(qrow(q, 1), "   1 x");
    qkey(q, K_CTRL_DOWN);
    CHECK_STR(qrow(q, 22), "1000 x");
    qquit(q);
}

TEST(gutter_hides_on_narrow_terminal) {
    qopts o = {.rows = 10, .cols = 22};
    quip *q = qstart_opts("a.txt", "hello\n", &o);
    CHECK_STR(qrow(q, 1), "hello");
    CHECK_STR(qrow(q, 2), "~");
    qquit(q);
}

TEST(syntax_colors_c) {
    quip *q = qstart("a.c", "int main() { return 42; } // hi \"s\"\nchar *s = \"str\";\n");
    /* int: type (green), return: keyword (yellow), 42: number (red),
     * comment: cyan, string: magenta. Columns are 1-based, gutter is 4. */
    CHECK_INT(fg(q, 1, 5), 32);
    CHECK_INT(fg(q, 1, 9), 0);
    CHECK_INT(fg(q, 1, 18), 33);
    CHECK_INT(fg(q, 1, 25), 31);
    CHECK_INT(fg(q, 1, 31), 36);
    CHECK_INT(fg(q, 1, 39), 36);
    CHECK_INT(fg(q, 2, 15), 35);
    qquit(q);
}

TEST(multiline_comment_highlight_follows_edits) {
    quip *q = qstart("a.c", "int a;\nint b;\nint c;\n");
    CHECK_INT(fg(q, 3, 5), 32);
    qtype(q, "/*");
    CHECK_INT(fg(q, 3, 5), 36);
    qkey(q, K_BACKSPACE);
    CHECK_INT(fg(q, 3, 5), 32);
    qquit(q);
}

TEST(trailing_spaces_are_red) {
    quip *q = qstart("a.txt", "ab  \nc\t\n");
    CHECK_INT(bg(q, 1, 6), 0);
    CHECK_INT(bg(q, 1, 7), 41);
    CHECK_INT(bg(q, 1, 8), 41);
    /* Nothing bleeds past the end of the line. */
    CHECK_INT(bg(q, 1, 9), 0);
    CHECK_INT(bg(q, 1, 80), 0);
    /* Tabs are not flagged. */
    CHECK_INT(bg(q, 2, 6), 0);
    qquit(q);
}

TEST(bracket_pair_is_highlighted) {
    quip *q = qstart("a.c", "f(a[1], b)\n");
    qkey(q, K_RIGHT); /* on '(' */
    CHECK_INT(bg(q, 1, 6), 100);
    CHECK_INT(bg(q, 1, 14), 100);
    CHECK_INT(bg(q, 1, 8), 0);
    qkey(q, K_RIGHT);
    qkey(q, K_RIGHT); /* on '[' */
    CHECK_INT(bg(q, 1, 8), 100);
    CHECK_INT(bg(q, 1, 10), 100);
    CHECK_INT(bg(q, 1, 6), 0);
    qquit(q);
}

TEST(unmatched_bracket_is_red) {
    quip *q = qstart("a.c", "x = (1;\n");
    for (int i = 0; i < 4; i++) qkey(q, K_RIGHT);
    CHECK_INT(bg(q, 1, 9), 41);
    qquit(q);
}

TEST(brackets_in_strings_are_ignored) {
    quip *q = qstart("a.c", "f(\")\")\n");
    qkey(q, K_RIGHT);
    CHECK_INT(bg(q, 1, 6), 100);
    CHECK_INT(bg(q, 1, 10), 100);
    CHECK_INT(bg(q, 1, 8), 0);
    qquit(q);
}

TEST(status_bar_truncates_long_names) {
    quip *q = qstart("a_very_long_file_name_indeed.txt", "x\n");
    CHECK(strncmp(qstatus(q), "a_very_long_file_nam - 1 line ", 30) == 0);
    qquit(q);
}

TEST(resize_redraws_to_new_size) {
    quip *q = qstart("a.txt", "hello\n");
    qresize(q, 10, 40);
    CHECK_STR(qrow(q, 1), "  1 hello");
    CHECK(strncmp(qrow(q, 9), "a.txt - 1 line ", 15) == 0);
    CHECK_INT((int)strlen(qrow(q, 9)), 40);
    qresize(q, 30, 100);
    CHECK_INT((int)strlen(qrow(q, 29)), 100);
    qquit(q);
}
TEST(full_width_line_keeps_last_column) {
    /* A line exactly as wide as the text area: its last character must
     * survive the erase-to-end-of-line that follows it. */
    char line[80];
    memset(line, 'x', 75);
    line[75] = 'E';
    line[76] = '\n';
    line[77] = '\0';
    quip *q = qstart("a.txt", line);
    CHECK_INT(qcell(q, 1, 80)->cp, 'E');
    qquit(q);
}
