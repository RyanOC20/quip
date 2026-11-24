/* Undo and redo, including the snapshot semantics quip has always had:
 * a snapshot is taken before each edit group, so the newest group's result
 * is not itself a snapshot, and Undo goes back to the state before the
 * group before it. These tests pin that behavior down; see the roadmap. */
#include <stdlib.h>

#include "../keys.h"
#include "../pty.h"
#include "../test.h"

static void check_saved(quip *q, const char *name, const char *want) {
    qkey(q, K_CTRL_S);
    char *saved = qread(q, name, NULL);
    CHECK_STR(saved, want);
    free(saved);
}

TEST(undo_typing_group) {
    quip *q = qstart("a.txt", "x\n");
    qtype(q, "abc");
    CHECK_STR(qline(q, 1), "abcx");
    qkey(q, K_CTRL_Z);
    CHECK_STR(qline(q, 1), "x");
    CHECK(strstr(qstatus(q), "modified") == NULL);
    qquit(q);
}

TEST(undo_redo_round_trip) {
    quip *q = qstart("a.txt", "\n");
    qtype(q, "one");
    qkey(q, K_ENTER);
    qtype(q, "two");
    /* Snapshots: "" (start), "" (before typing), "one" (before Enter),
     * "one\n" (before "two"). Undo lands on the one before the last. */
    qkey(q, K_CTRL_Z);
    CHECK_STR(qline(q, 1), "one");
    CHECK_STR(qline(q, 2), "~");
    qkey(q, K_CTRL_Z);
    CHECK_STR(qline(q, 1), "");
    qkey(q, K_CTRL_Y);
    CHECK_STR(qline(q, 1), "one");
    qkey(q, K_CTRL_Y);
    CHECK_STR(qline(q, 2), "");
    /* "two" was never snapshotted: Redo cannot bring it back. */
    qkey(q, K_CTRL_Y);
    CHECK_STR(qline(q, 2), "");
    CHECK_STR(qmessage(q), "Nothing to redo.");
    check_saved(q, "a.txt", "one\n\n");
    qquit(q);
}

TEST(undo_quirk_type_backspace_undo) {
    /* Type "ab", Backspace, Undo: back to "" (the state before the typing
     * group), not "ab". */
    quip *q = qstart("a.txt", "\n");
    qtype(q, "ab");
    qkey(q, K_BACKSPACE);
    qkey(q, K_CTRL_Z);
    CHECK_STR(qline(q, 1), "");
    qquit(q);
}

TEST(redo_after_single_edit_does_nothing) {
    quip *q = qstart("a.txt", "\n");
    qtype(q, "a");
    qkey(q, K_CTRL_Z);
    CHECK_STR(qline(q, 1), "");
    qkey(q, K_CTRL_Y);
    CHECK_STR(qline(q, 1), "");
    qquit(q);
}

TEST(undo_restores_cursor) {
    quip *q = qstart("a.txt", "line one\nline two\n");
    qkey(q, K_DOWN);
    qkey(q, K_END);
    qtype(q, "!");
    qkey(q, K_ENTER);
    qkey(q, K_CTRL_UP);
    /* Back to the state before the "!" group, with the cursor where that
     * group started. */
    qkey(q, K_CTRL_Z);
    CHECK_STR(qline(q, 2), "line two");
    CHECK_INT(q->scr.y, 1);
    CHECK_INT(q->scr.x, 4 + 8);
    qquit(q);
}

TEST(undo_cut_and_paste) {
    quip *q = qstart("a.txt", "abc\n");
    qkey(q, K_SHIFT_RIGHT);
    qkey(q, K_CTRL_X);
    CHECK_STR(qline(q, 1), "bc");
    qkey(q, K_CTRL_Z);
    CHECK_STR(qline(q, 1), "abc");
    qquit(q);
}

TEST(save_marks_clean_and_undo_back_to_saved_state) {
    quip *q = qstart("a.txt", "\n");
    qtype(q, "a");
    qkey(q, K_CTRL_S);
    CHECK(strstr(qstatus(q), "modified") == NULL);
    qkey(q, K_ENTER);
    CHECK(strstr(qstatus(q), "modified") != NULL);
    qkey(q, K_CTRL_Z);
    CHECK_STR(qline(q, 2), "~");
    qquit(q);
}
