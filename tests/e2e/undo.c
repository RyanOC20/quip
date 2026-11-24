/* Undo and redo. Each typing run, and each other edit, is one step; Undo
 * reverts the newest step and Redo brings it back. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../keygen.h"
#include "../keys.h"
#include "../pty.h"
#include "../test.h"

static void check_saved(quip *q, const char *name, const char *want) {
    qkey(q, K_CTRL_S);
    char *saved = qread(q, name, NULL);
    CHECK_STR(saved, want);
    free(saved);
}

static int modified(quip *q) { return strstr(qstatus(q), "modified") != NULL; }

TEST(undo_typing_group) {
    quip *q = qstart("a.txt", "x\n");
    qtype(q, "abc");
    CHECK_STR(qline(q, 1), "abcx");
    qkey(q, K_CTRL_Z);
    CHECK_STR(qline(q, 1), "x");
    CHECK(!modified(q));
    qquit(q);
}

TEST(nothing_to_undo_at_start) {
    quip *q = qstart("a.txt", "x\n");
    qkey(q, K_CTRL_Z);
    CHECK_STR(qmessage(q), "Nothing to undo.");
    CHECK_STR(qline(q, 1), "x");
    qkey(q, K_CTRL_Y);
    CHECK_STR(qmessage(q), "Nothing to redo.");
    qquit(q);
}

TEST(undo_redo_round_trip) {
    quip *q = qstart("a.txt", "\n");
    qtype(q, "one");
    qkey(q, K_ENTER);
    qtype(q, "two");
    qkey(q, K_CTRL_Z);
    CHECK_STR(qline(q, 1), "one");
    CHECK_STR(qline(q, 2), "");
    qkey(q, K_CTRL_Z);
    CHECK_STR(qline(q, 1), "one");
    CHECK_STR(qline(q, 2), "~");
    qkey(q, K_CTRL_Z);
    CHECK_STR(qline(q, 1), "");
    qkey(q, K_CTRL_Z);
    CHECK_STR(qmessage(q), "Nothing to undo.");
    qkey(q, K_CTRL_Y);
    qkey(q, K_CTRL_Y);
    qkey(q, K_CTRL_Y);
    CHECK_STR(qline(q, 1), "one");
    CHECK_STR(qline(q, 2), "two");
    qkey(q, K_CTRL_Y);
    CHECK_STR(qmessage(q), "Nothing to redo.");
    check_saved(q, "a.txt", "one\ntwo\n");
    qquit(q);
}

TEST(undo_backspace_then_typing) {
    quip *q = qstart("a.txt", "\n");
    qtype(q, "ab");
    qkey(q, K_BACKSPACE);
    qkey(q, K_CTRL_Z);
    CHECK_STR(qline(q, 1), "ab");
    qkey(q, K_CTRL_Z);
    CHECK_STR(qline(q, 1), "");
    qquit(q);
}

TEST(redo_after_single_edit) {
    quip *q = qstart("a.txt", "\n");
    qtype(q, "a");
    qkey(q, K_CTRL_Z);
    CHECK_STR(qline(q, 1), "");
    qkey(q, K_CTRL_Y);
    CHECK_STR(qline(q, 1), "a");
    CHECK_INT(q->scr.x, 4 + 1);
    qquit(q);
}

TEST(new_edit_after_undo_discards_redo) {
    quip *q = qstart("a.txt", "\n");
    qtype(q, "a");
    qkey(q, K_CTRL_Z);
    qtype(q, "b");
    qkey(q, K_CTRL_Y);
    CHECK_STR(qmessage(q), "Nothing to redo.");
    CHECK_STR(qline(q, 1), "b");
    qkey(q, K_CTRL_Z);
    CHECK_STR(qline(q, 1), "");
    qquit(q);
}

TEST(undo_returns_cursor_to_the_edit) {
    quip *q = qstart("a.txt", "line one\nline two\n");
    qkey(q, K_DOWN);
    qkey(q, K_END);
    qtype(q, "!");
    qkey(q, K_CTRL_UP);
    qkey(q, K_CTRL_Z);
    CHECK_STR(qline(q, 2), "line two");
    CHECK_INT(q->scr.y, 1);
    CHECK_INT(q->scr.x, 4 + 8);
    qkey(q, K_CTRL_UP);
    qkey(q, K_CTRL_Y);
    /* Redo puts the cursor at the end of the restored text. */
    CHECK_STR(qline(q, 2), "line two!");
    CHECK_INT(q->scr.y, 1);
    CHECK_INT(q->scr.x, 4 + 9);
    qquit(q);
}

TEST(undo_after_moving_between_edits) {
    /* Two typing runs in different places are two steps, each undone
     * where it happened, even after an undo and a move. */
    quip *q = qstart("a.txt", "aaa\nbbb\n");
    qtype(q, "1");
    qkey(q, K_DOWN);
    qtype(q, "2");
    qkey(q, K_CTRL_Z);
    CHECK_STR(qline(q, 2), "bbb");
    qkey(q, K_CTRL_UP);
    qkey(q, K_END);
    qtype(q, "3");
    qkey(q, K_CTRL_Z);
    CHECK_STR(qline(q, 1), "1aaa");
    CHECK_INT(q->scr.x, 4 + 4);
    qkey(q, K_CTRL_Z);
    CHECK_STR(qline(q, 1), "aaa");
    qquit(q);
}

TEST(undo_cut_and_paste) {
    quip *q = qstart("a.txt", "abc\n");
    qkey(q, K_SHIFT_RIGHT);
    qkey(q, K_CTRL_X);
    CHECK_STR(qline(q, 1), "bc");
    qkey(q, K_END);
    qkey(q, K_CTRL_V);
    CHECK_STR(qline(q, 1), "bca");
    qkey(q, K_CTRL_Z);
    CHECK_STR(qline(q, 1), "bc");
    qkey(q, K_CTRL_Z);
    CHECK_STR(qline(q, 1), "abc");
    qquit(q);
}

TEST(saved_state_tracks_undo_and_redo) {
    quip *q = qstart("a.txt", "\n");
    qtype(q, "a");
    qkey(q, K_CTRL_S);
    CHECK(!modified(q));
    qtype(q, "b");
    CHECK(modified(q));
    qkey(q, K_CTRL_Z);
    CHECK_STR(qline(q, 1), "a");
    CHECK(!modified(q));
    qkey(q, K_CTRL_Z);
    CHECK_STR(qline(q, 1), "");
    CHECK(modified(q));
    qkey(q, K_CTRL_Y);
    CHECK(!modified(q));
    qkey(q, K_CTRL_Y);
    CHECK_STR(qline(q, 1), "ab");
    CHECK(modified(q));
    qquit(q);
}

TEST(failed_save_keeps_file_modified) {
    quip *q = qstart("sub/a.txt", "x\n");
    qtype(q, "y");
    /* Make the file read-only so the save fails. */
    char path[1100];
    snprintf(path, sizeof(path), "%s/sub/a.txt", q->dir);
    CHECK(chmod(path, 0444) == 0);
    qkey(q, K_CTRL_S);
    CHECK(strstr(qmessage(q), "Can't save") != NULL);
    CHECK(modified(q));
    /* The disk still holds "x\n": no state of the history matches it but
     * the first. */
    qkey(q, K_CTRL_Z);
    qkey(q, K_CTRL_Y);
    CHECK_STR(qline(q, 1), "yx");
    CHECK(modified(q));
    chmod(path, 0644);
    qquit(q);
}

/* Save and return the file's text. */
static char *save_text(quip *q, const char *name) {
    qkey(q, K_CTRL_S);
    char *t = qread(q, name, NULL);
    return t ? t : strdup("");
}

/* After any editing session, undoing everything gives back the starting
 * text and redoing everything gives back the final text. */
static void undo_redo_property(uint32_t first, uint32_t last) {
    for (uint32_t seed = first; seed <= last; seed++) {
        kcase c;
        kcase_make_no_tree(&c, seed, 120);
        quip *q = kcase_run(&c, NULL);
        qkey(q, K_ESC); /* leave the find prompt */
        qkey(q, K_ESC);
        char *final = save_text(q, c.file);
        int steps = 0;
        do {
            qkey(q, K_CTRL_Z);
        } while (strcmp(qmessage(q), "Nothing to undo.") && ++steps < 300);
        char *start = save_text(q, c.file);
        CHECK_STR(start, c.content ? c.content : "");
        do {
            qkey(q, K_CTRL_Y);
        } while (strcmp(qmessage(q), "Nothing to redo.") && --steps > -300);
        char *again = save_text(q, c.file);
        if (strcmp(again, final)) printf("seed %u\n", seed);
        CHECK_STR(again, final);
        free(final);
        free(start);
        free(again);
        qquit(q);
        kcase_free(&c);
    }
}

TEST(undo_redo_property_1) { undo_redo_property(1, 10); }
TEST(undo_redo_property_2) { undo_redo_property(11, 20); }
TEST(undo_redo_property_3) { undo_redo_property(21, 30); }
TEST(undo_redo_property_4) { undo_redo_property(31, 40); }
