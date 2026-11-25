/* UTF-8 text: the cursor, clicks and edits work in whole characters, and
 * wide characters take the two cells the terminal gives them. Text starts
 * at screen column 5 (1-based), after the line number gutter; q->scr.x is
 * 0-based, so text column n is x == 4 + n. */
#include <stdlib.h>
#include <string.h>

#include "../keys.h"
#include "../pty.h"
#include "../test.h"

#define WIDE            "\xe6\x97\xa5" /* U+65E5, two cells */
#define WIDE2           "\xe6\x9c\xac" /* U+672C, two cells */
#define E_ACUTE         "\xc3\xa9"     /* U+00E9, one cell, two bytes */
#define COMBINING_ACUTE "\xcc\x81"

static void check_saved(quip *q, const char *name, const char *want) {
    qkey(q, K_CTRL_S);
    char *saved = qread(q, name, NULL);
    CHECK_STR(saved, want);
    free(saved);
}

static int rev(quip *q, int row, int col) { return (qcell(q, row, col)->attr & ATTR_REVERSE) != 0; }

TEST(cursor_is_drawn_after_wide_characters) {
    quip *q = qstart("a.txt", WIDE WIDE2 "x\n");
    qkey(q, K_END);
    CHECK_INT(q->scr.x, 4 + 5);
    qkey(q, K_LEFT);
    CHECK_INT(q->scr.x, 4 + 4);
    qkey(q, K_LEFT);
    CHECK_INT(q->scr.x, 4 + 2);
    qquit(q);
}

TEST(arrows_step_over_whole_characters) {
    quip *q = qstart("a.txt", E_ACUTE WIDE "x\n");
    qkey(q, K_RIGHT);
    CHECK_INT(q->scr.x, 4 + 1);
    qkey(q, K_RIGHT);
    CHECK_INT(q->scr.x, 4 + 3);
    qtype(q, "!");
    check_saved(q, "a.txt", E_ACUTE WIDE "!x\n");
    qquit(q);
}

TEST(combining_mark_moves_with_its_letter) {
    quip *q = qstart("a.txt", "e" COMBINING_ACUTE "x\n");
    qkey(q, K_RIGHT);
    CHECK_INT(q->scr.x, 4 + 1);
    qtype(q, "!");
    check_saved(q, "a.txt", "e" COMBINING_ACUTE "!x\n");
    qquit(q);
}

TEST(backspace_deletes_a_whole_character) {
    quip *q = qstart("a.txt", "a" WIDE "b\n");
    qkey(q, K_RIGHT);
    qkey(q, K_RIGHT);
    qkey(q, K_BACKSPACE);
    CHECK_INT(q->scr.x, 4 + 1);
    check_saved(q, "a.txt", "ab\n");
    qquit(q);
}

TEST(up_and_down_keep_the_screen_column) {
    quip *q = qstart("a.txt", WIDE WIDE2 "\nabcdef\n");
    qkey(q, K_END);
    CHECK_INT(q->scr.x, 4 + 4);
    qkey(q, K_DOWN);
    CHECK_INT(q->scr.x, 4 + 4);
    qtype(q, "!");
    check_saved(q, "a.txt", WIDE WIDE2 "\nabcd!ef\n");
    qquit(q);
}

TEST(click_on_a_wide_character) {
    quip *q = qstart("a.txt", WIDE WIDE2 "\n");
    /* The first cell of the second character puts the cursor before it,
     * the second cell after it. */
    qclick(q, 1, 5 + 2);
    CHECK_INT(q->scr.x, 4 + 2);
    qclick(q, 1, 5 + 3);
    CHECK_INT(q->scr.x, 4 + 4);
    qquit(q);
}

TEST(double_click_selects_a_word_with_utf8) {
    quip *q = qstart("a.txt", "caf" E_ACUTE " " WIDE WIDE2 " x\n");
    qclick(q, 1, 5 + 6);
    qclick(q, 1, 5 + 6);
    CHECK(rev(q, 1, 5 + 5) && rev(q, 1, 5 + 8));
    CHECK(!rev(q, 1, 5 + 4) && !rev(q, 1, 5 + 9));
    CHECK(strstr(qstatus(q), "2 selected") != NULL);
    qquit(q);
}

TEST(tab_stops_count_screen_columns) {
    /* QUIP_TAB_STOP is 2: after a wide character the TAB is a full stop. */
    quip *q = qstart("a.txt", WIDE "\tx\n");
    CHECK_STR(qline(q, 1), WIDE "  x");
    qkey(q, K_END);
    CHECK_INT(q->scr.x, 4 + 5);
    qquit(q);
}

TEST(status_column_counts_characters) {
    quip *q = qstart("a.txt", WIDE E_ACUTE "x\n");
    qkey(q, K_END);
    CHECK(strstr(qstatus(q), " 1:4") != NULL);
    qquit(q);
}

TEST(selection_covers_both_cells_of_wide_characters) {
    quip *q = qstart("a.txt", "a" WIDE "b\n");
    qkey(q, K_RIGHT);
    qkey(q, K_SHIFT_RIGHT);
    CHECK(!rev(q, 1, 5));
    CHECK(rev(q, 1, 6) && rev(q, 1, 7));
    CHECK(!rev(q, 1, 8));
    CHECK(strstr(qstatus(q), "1 selected") != NULL);
    qquit(q);
}

TEST(long_line_of_wide_characters_scrolls_whole_characters) {
    char line[60 * 3 + 2];
    for (int i = 0; i < 60; i++) memcpy(line + 3 * i, WIDE, 3);
    strcpy(line + 180, "\n");
    quip *q = qstart("a.txt", line);
    qkey(q, K_END);
    /* 120 cells in 76 text columns: the view starts at column 45, half way
     * into a character, which shows as a blank, and the cursor is in the
     * last column. */
    CHECK_INT(q->scr.x, 79);
    CHECK_INT(qcell(q, 1, 5)->cp, ' ');
    CHECK(qcell(q, 1, 6)->cp == 0x65e5);
    CHECK(strstr(qstatus(q), " 1:61") != NULL);
    qkey(q, K_HOME);
    CHECK_INT(q->scr.x, 4);
    CHECK(qcell(q, 1, 5)->cp == 0x65e5);
    qquit(q);
}

TEST(stray_bytes_are_one_cell_each) {
    quip *q = qstart("a.txt", "a\xff\xfe"
                              "b\n");
    CHECK_STR(qline(q, 1), "a??b");
    CHECK(rev(q, 1, 6) && rev(q, 1, 7));
    qkey(q, K_END);
    CHECK_INT(q->scr.x, 4 + 4);
    qkey(q, K_LEFT);
    qkey(q, K_LEFT);
    qkey(q, K_BACKSPACE);
    check_saved(q, "a.txt",
                "a\xfe"
                "b\n");
    qquit(q);
}

TEST(find_a_utf8_query) {
    quip *q = qstart("a.txt", "x " WIDE WIDE2 " y\n");
    qkey(q, K_CTRL_F);
    qtype(q, WIDE2);
    CHECK(strstr(qmessage(q), "Search: " WIDE2) != NULL);
    qkey(q, K_ENTER);
    CHECK_INT(q->scr.x, 4 + 4);
    qquit(q);
}

TEST(typed_wide_character_is_one_frame) {
    quip *q = qstart("a.txt", "\n");
    qtype(q, WIDE "x");
    CHECK_STR(qline(q, 1), WIDE "x");
    CHECK_INT(q->scr.x, 4 + 3);
    check_saved(q, "a.txt", WIDE "x\n");
    qquit(q);
}

TEST(status_bar_fits_a_utf8_file_name) {
    /* 12 wide characters: 24 columns, cut to the 20 the name may take. */
    char name[12 * 3 + 5];
    for (int i = 0; i < 12; i++) memcpy(name + 3 * i, WIDE, 3);
    strcpy(name + 36, ".txt");
    quip *q = qstart(name, "x\n");
    char want[64];
    for (int i = 0; i < 10; i++) memcpy(want + 3 * i, WIDE, 3);
    strcpy(want + 30, " - 1 line ");
    const char *st = qstatus(q);
    CHECK(strncmp(st, want, strlen(want)) == 0);
    /* The position still ends in the last column. */
    CHECK_INT(qcell(q, 23, 78)->cp, '1');
    CHECK_INT(qcell(q, 23, 80)->cp, '1');
    qquit(q);
}
