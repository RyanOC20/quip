/* Undo and redo. */
#include "quip.h"

/* Each snapshot stores only the delta from the previous snapshot's serialized
 * text, not a full copy: replacing bytes [pos, pos+oldlen) of the previous
 * text with 'new' (newlen bytes) yields this snapshot's text, and the reverse
 * splice undoes it. 'undo_base' always holds the full text of the snapshot at
 * 'undo_cur', so any neighbouring snapshot is one splice away. */
typedef struct editorSnapshot {
    int pos;
    char *old; /* bytes present in the previous snapshot's text */
    int oldlen;
    char *new; /* bytes present in this snapshot's text */
    int newlen;
    int cx, cy, rowoff, coloff;
} editorSnapshot;

#define UNDO_HISTORY_MAX 200

static editorSnapshot undo_hist[UNDO_HISTORY_MAX];
static int undo_cur = -1;
static int undo_count = 0;
static int undo_saved = -1;
int undo_last_insert = 0;      /* Typing continues the current undo step. */
static char *undo_base = NULL; /* Full serialized text of the snapshot at undo_cur. */
static int undo_base_len = 0;

/* A copy of the 'len' bytes at s + off, or NULL when len is 0 (s itself may
 * then be NULL, and pointer arithmetic on NULL is undefined). */
static char *memdup(const char *s, int off, int len) {
    if (len == 0) return NULL;
    char *d = malloc(len);
    memcpy(d, s + off, len);
    return d;
}

/* Produce src with bytes [pos, pos+rmlen) replaced by 'ins' (inslen bytes).
 * Returns a freshly allocated, NUL-terminated buffer; *outlen excludes NUL. */
static char *editorSplice(const char *src, int srclen, int pos, int rmlen, const char *ins,
                          int inslen, int *outlen) {
    int nl = srclen - rmlen + inslen;
    char *out = malloc(nl + 1);
    /* Either side may be NULL when empty, and memcpy from NULL is undefined
     * even for zero bytes. */
    if (pos) memcpy(out, src, pos);
    if (inslen) memcpy(out + pos, ins, inslen);
    if (srclen - pos - rmlen) memcpy(out + pos + inslen, src + pos + rmlen, srclen - pos - rmlen);
    out[nl] = '\0';
    *outlen = nl;
    return out;
}

void editorPushSnapshot(void) {
    /* Discard any redo entries above undo_cur. */
    for (int i = undo_cur + 1; i < undo_count; i++) {
        free(undo_hist[i].old);
        free(undo_hist[i].new);
    }
    undo_count = undo_cur + 1;

    /* Serialize the current document. When it equals the newest step (as
     * right after an undo, or an edit that changed nothing) there is no new
     * step to record; only note where the cursor is, so that undoing the
     * coming edit brings the cursor back here. */
    int curlen;
    char *cur = editorRowsToString(&curlen);
    if (undo_cur >= 0 && curlen == undo_base_len && memcmp(cur, undo_base, curlen) == 0) {
        editorSnapshot *t = &undo_hist[undo_cur];
        t->cx = E.cx;
        t->cy = E.cy;
        t->rowoff = E.rowoff;
        t->coloff = E.coloff;
        free(cur);
        return;
    }

    /* If the history is full, drop the oldest entry. */
    if (undo_count == UNDO_HISTORY_MAX) {
        free(undo_hist[0].old);
        free(undo_hist[0].new);
        memmove(&undo_hist[0], &undo_hist[1], (UNDO_HISTORY_MAX - 1) * sizeof(editorSnapshot));
        undo_count--;
        if (undo_cur > 0) undo_cur--;
        if (undo_saved > 0)
            undo_saved--;
        else if (undo_saved == 0)
            undo_saved = -1;
        /* The new head keeps its cursor state but its delta now has no
         * predecessor and is never applied, so release it. */
        free(undo_hist[0].old);
        free(undo_hist[0].new);
        undo_hist[0].old = NULL;
        undo_hist[0].oldlen = 0;
        undo_hist[0].new = NULL;
        undo_hist[0].newlen = 0;
        undo_hist[0].pos = 0;
    }

    /* Diff the document against the base (the text of the snapshot at
     * undo_cur), storing only the changed middle span. */
    int minlen = (undo_base_len < curlen) ? undo_base_len : curlen;
    int p = 0, s = 0;
    while (p < minlen && undo_base[p] == cur[p]) p++;
    while (s < minlen - p && undo_base[undo_base_len - 1 - s] == cur[curlen - 1 - s]) s++;

    editorSnapshot *e = &undo_hist[undo_count];
    e->pos = p;
    e->oldlen = undo_base_len - s - p;
    e->newlen = curlen - s - p;
    e->old = memdup(undo_base, p, e->oldlen);
    e->new = memdup(cur, p, e->newlen);
    e->cx = E.cx;
    e->cy = E.cy;
    e->rowoff = E.rowoff;
    e->coloff = E.coloff;
    undo_cur = undo_count;
    undo_count++;

    /* The current document is now the base. */
    free(undo_base);
    undo_base = cur;
    undo_base_len = curlen;
}

/* Record the document as a step if it changed since the newest one, so
 * that the latest edits can be undone and then redone. */
void editorCommitPending(void) {
    int len;
    char *cur = editorRowsToString(&len);
    int same = len == undo_base_len && memcmp(cur, undo_base, len) == 0;
    free(cur);
    if (!same) editorPushSnapshot();
    undo_last_insert = 0;
}

void editorUndo(void) {
    editorCommitPending();
    if (undo_cur <= 0) {
        editorSetStatusMessage("Nothing to undo.");
        return;
    }
    /* Reverse the delta at undo_cur to reconstruct the previous text. */
    editorSnapshot *e = &undo_hist[undo_cur];
    int prevlen;
    char *prev =
        editorSplice(undo_base, undo_base_len, e->pos, e->newlen, e->old, e->oldlen, &prevlen);
    free(undo_base);
    undo_base = prev;
    undo_base_len = prevlen;
    undo_cur--;

    editorRebuildRows(undo_base, undo_base_len);
    editorSnapshot *t = &undo_hist[undo_cur];
    E.cx = t->cx;
    E.cy = t->cy;
    E.rowoff = t->rowoff;
    E.coloff = t->coloff;
    E.dirty = (undo_cur != undo_saved);
    E.sel_active = 0;
    undo_last_insert = 0;
}

void editorRedo(void) {
    if (undo_cur >= undo_count - 1) {
        editorSetStatusMessage("Nothing to redo.");
        return;
    }
    undo_cur++;
    /* Apply the delta at undo_cur to reconstruct the next text. */
    editorSnapshot *e = &undo_hist[undo_cur];
    int nextlen;
    char *next =
        editorSplice(undo_base, undo_base_len, e->pos, e->oldlen, e->new, e->newlen, &nextlen);
    free(undo_base);
    undo_base = next;
    undo_base_len = nextlen;

    editorRebuildRows(undo_base, undo_base_len);
    /* The cursor goes to the end of the restored text. */
    int end = e->pos + e->newlen, row = 0, col = 0;
    for (int i = 0; i < end && i < undo_base_len; i++) {
        if (undo_base[i] == '\n') {
            row++;
            col = 0;
        } else
            col++;
    }
    if (row >= E.numrows && E.numrows > 0) {
        row = E.numrows - 1;
        col = E.row[row].size;
    }
    editorSetCursorPos(row, col);
    E.dirty = (undo_cur != undo_saved);
    E.sel_active = 0;
    undo_last_insert = 0;
}

/* Forget the whole history, as when another file is opened. */
void editorUndoReset(void) {
    for (int i = 0; i < undo_count; i++) {
        free(undo_hist[i].old);
        free(undo_hist[i].new);
    }
    undo_cur = -1;
    undo_count = 0;
    undo_saved = -1;
    undo_last_insert = 0;
    free(undo_base);
    undo_base = NULL;
    undo_base_len = 0;
}

/* The newest step is what is on disk now. */
void editorUndoMarkSaved(void) { undo_saved = undo_cur; }
