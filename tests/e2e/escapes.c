/* Untrusted bytes must never reach the terminal as control sequences: file
 * contents, file names and messages are shown, never obeyed. The screen
 * model fails any test where quip sends a raw control byte. */
#include <stdlib.h>
#include <string.h>

#include "../keys.h"
#include "../pty.h"
#include "../test.h"

static int rev(quip *q, int row, int col) {
    return (qcell(q, row, col)->attr & ATTR_REVERSE) != 0;
}

TEST(control_characters_show_in_caret_notation) {
    /* Ctrl-A shows as a reverse 'A', ESC as '[', DEL as '?'. */
    quip *q = qstart("a.txt", "a\x01" "b\x1b" "c\x7f" "d\n");
    CHECK_STR(qline(q, 1), "aAb[c?d");
    CHECK(!rev(q, 1, 5));
    CHECK(rev(q, 1, 6));
    CHECK(!rev(q, 1, 7));
    CHECK(rev(q, 1, 8));
    CHECK(rev(q, 1, 10));
    CHECK(!rev(q, 1, 11));
    qquit(q);
}

TEST(control_characters_in_strings_and_comments) {
    /* Inside a string or comment the lexer colors the bytes, but they are
     * still drawn as caret letters. */
    quip *q = qstart("a.c", "char *s = \"\x1b[2J\"; // \x07\n/* \x1b" "c */\n");
    CHECK_STR(qline(q, 1), "char *s = \"[[2J\"; // G");
    CHECK_STR(qline(q, 2), "/* [c */");
    CHECK(rev(q, 1, 16));
    CHECK(rev(q, 2, 8));
    qquit(q);
}

TEST(osc52_in_file_does_not_set_clipboard) {
    quip *q = qstart("a.txt", "\x1b]52;c;aGFja2Vk\x07\n");
    CHECK(q->scr.clipboard == NULL);
    qquit(q);
}

TEST(c1_controls_are_not_sent) {
    /* U+009B is CSI in terminals that honor C1 controls. */
    quip *q = qstart("a.txt", "x\xc2\x9b" "2Jy\n");
    CHECK_STR(qline(q, 1), "x??2Jy");
    CHECK(rev(q, 1, 6) && rev(q, 1, 7));
    qquit(q);
}

TEST(utf8_in_highlighted_code_is_drawn_as_text) {
    quip *q = qstart("a.c", "int caf\xc3\xa9 = 1; /* \xe6\x97\xa5 */\n");
    CHECK_STR(qline(q, 1), "int caf\xc3\xa9 = 1; /* \xe6\x97\xa5 */");
    CHECK_INT(qcell(q, 1, 5)->fg, 32);
    qquit(q);
}

TEST(file_name_with_escape_in_status_bar) {
    quip *q = qstart("a\x1b]0;x\x07.txt", "x\n");
    CHECK(strncmp(qstatus(q), "a?]0;x?.txt - 1 lines", 21) == 0);
    qquit(q);
}

TEST(file_name_with_escape_in_tree) {
    const char *const files[] = {"evil\x1b[2Jname", "x\n", NULL};
    qopts o = {.files = files};
    quip *q = qstart_opts("a.txt", "x\n", &o);
    qkey(q, K_CTRL_E);
    char want[64];
    screen_span_text(&q->scr, 2, 0, 29, want, sizeof(want));
    CHECK(strstr(want, "evil?[2Jname") != NULL);
    qquit(q);
}

TEST(control_character_keeps_trailing_space_background) {
    /* After drawing a control character the background state is reset, so
     * a trailing space right after it is still red. */
    quip *q = qstart("a.txt", "a\x01 \n");
    CHECK_INT(qcell(q, 1, 7)->bg, 41);
    qquit(q);
}
