/* Keys and mouse: cursor motion, selection, editing commands and the key dispatcher. */
#include "quip.h"

/* The row under the cursor, or NULL on the line after the last. */
static erow *editorCursorRow(void) { return E.cy < E.numrows ? &E.row[E.cy] : NULL; }

/* Screen column of the cursor, counted from the start of its line. */
static int editorCursorRx(void) {
    erow *row = editorCursorRow();
    return row ? rowCxToRx(row, E.cx) : E.cx;
}

/* Insert spaces up to the next tab stop instead of a TAB character. */
static void editorInsertTab(void) {
    int n = QUIP_TAB_STOP - (editorCursorRx() % QUIP_TAB_STOP);
    while (n--) editorInsertChar(' ');
}

static void editorSelClear(void) { E.sel_active = 0; }

/* Ordered bounds of the selection: from (*sr,*sc) up to (*er,*ec). An end on
 * the empty line past the last row (the keyboard can reach it) is clamped to
 * the end of the file, so callers only ever see real rows. Returns 0 when
 * nothing is selected. */
int editorSelBounds(int *sr, int *sc, int *er, int *ec) {
    if (!E.sel_active || E.numrows == 0) return 0;
    int ar = E.sel_anchor_row, ac = E.sel_anchor_col;
    int cr = E.cy, cc = E.cx;
    if (ar < cr || (ar == cr && ac <= cc)) {
        *sr = ar;
        *sc = ac;
        *er = cr;
        *ec = cc;
    } else {
        *sr = cr;
        *sc = cc;
        *er = ar;
        *ec = ac;
    }
    if (*er >= E.numrows) {
        *er = E.numrows - 1;
        *ec = E.row[*er].size;
    }
    if (*sr > *er) {
        *sr = *er;
        *sc = *ec;
    }
    /* An end can lie past the end of its line (a click beyond the last
     * column, a cursor parked by sideways scrolling): clamp it, so callers
     * only ever see real positions. */
    if (*sc > E.row[*sr].size) *sc = E.row[*sr].size;
    if (*ec > E.row[*er].size) *ec = E.row[*er].size;
    if (*sr == *er && *sc > *ec) *sc = *ec;
    return !(*sr == *er && *sc == *ec);
}

/* Number of characters in the selection, each line break counting as one.
 * UTF-8 continuation bytes are skipped so a multibyte character counts once. */
long editorSelCharCount(void) {
    int sr, sc, er, ec;
    if (!editorSelBounds(&sr, &sc, &er, &ec)) return 0;
    long count = er - sr;
    for (int i = sr; i <= er; i++) {
        erow *row = &E.row[i];
        int from = (i == sr) ? sc : 0;
        int to = (i == er) ? ec : row->size;
        for (int j = from; j < to; j++)
            if (((unsigned char)row->chars[j] & 0xC0) != 0x80) count++;
    }
    return count;
}

static void editorSelSetAnchor(void) {
    if (!E.sel_active) {
        E.sel_active = 1;
        E.sel_anchor_row = E.cy;
        E.sel_anchor_col = E.cx;
    }
}

/* Move the cursor to file position (row,col). The view follows on the next
 * redraw (editorScroll). */
void editorSetCursorPos(int row, int col) {
    E.cy = row;
    E.cx = col;
}

/* Move the cursor to file row 'row', as near screen column 'rx' as that line
 * allows: up and down keep the column the cursor is drawn in, whatever
 * TABs and wide characters lie before it on either line. */
static void editorMoveToRow(int row, int rx) {
    E.cy = row;
    E.cx = row < E.numrows ? rowRxToCx(&E.row[row], rx) : 0;
}

/* Handle cursor position change because arrow keys were pressed. */
static void editorMoveCursor(int key) {
    erow *row = editorCursorRow();

    switch (key) {
    case ARROW_LEFT:
        if (E.cx > 0) {
            E.cx = row ? rowPrevCx(row, E.cx) : 0;
        } else if (E.cy > 0) {
            /* To the end of the previous line. */
            E.cy--;
            E.cx = E.row[E.cy].size;
        }
        break;
    case ARROW_RIGHT:
        if (row && E.cx < row->size) {
            E.cx = rowNextCx(row, E.cx);
        } else if (row) {
            E.cy++;
            E.cx = 0;
        }
        break;
    case ARROW_UP:
        if (E.cy > 0) editorMoveToRow(E.cy - 1, editorCursorRx());
        break;
    case ARROW_DOWN:
        if (E.cy < E.numrows) editorMoveToRow(E.cy + 1, editorCursorRx());
        break;
    }
}

static void editorMoveWordRight(void) {
    erow *row = editorCursorRow();
    if (!row) return;
    int pos = E.cx;

    if (pos >= row->size) {
        if (E.cy + 1 < E.numrows) {
            E.cy++;
            E.cx = 0;
        }
        return;
    }
    while (pos < row->size && editorIsSeparator(row->chars[pos])) pos++;
    while (pos < row->size && !editorIsSeparator(row->chars[pos])) pos++;
    E.cx = pos;
}

static void editorMoveWordLeft(void) {
    erow *row = editorCursorRow();
    if (!row) return;
    int pos = E.cx;

    if (pos == 0) {
        if (E.cy > 0) {
            E.cy--;
            E.cx = E.row[E.cy].size;
        }
        return;
    }
    pos--;
    while (pos > 0 && editorIsSeparator(row->chars[pos])) pos--;
    while (pos > 0 && !editorIsSeparator(row->chars[pos - 1])) pos--;
    E.cx = pos;
}

/* Home: to the first non-blank character, or to column 0 if already there. */
static void editorMoveLineStart(void) {
    erow *row = editorCursorRow();
    if (!row) {
        E.cx = 0;
        return;
    }
    int indent = 0;
    while (indent < row->size && isspace((unsigned char)row->chars[indent])) indent++;
    E.cx = E.cx != indent ? indent : 0;
}

static void editorMoveLineEnd(void) {
    erow *row = editorCursorRow();
    if (row) E.cx = row->size;
}

static void editorMoveFileStart(void) {
    E.cx = 0;
    E.cy = 0;
}

static void editorMoveFileEnd(void) {
    if (E.numrows == 0) return;
    E.cy = E.numrows - 1;
    E.cx = E.row[E.cy].size;
}

/* Where a cursor at 'col' lands after 'len' chars at 'at' are removed. */
static int editorColAfterDel(int col, int at, int len) {
    if (col >= at + len) return col - len;
    return col > at ? at : col;
}

/* If the text of 'row' between 'start' and 'end' is exactly one block
 * comment, return 1. "(* a *) x (* b *)" starts and ends like one but is
 * two, so this follows the comment's nesting to its real end. */
static int editorRowIsOneBlockComment(erow *row, int start, int end) {
    const hldelim *mcs = &E.syntax->block_start;
    const hldelim *mce = &E.syntax->block_end;
    char *p = row->chars;
    if (end - start < mcs->len + mce->len || !hlAt(p, start, end, mcs)) return 0;
    int depth = 1;
    for (int i = start + mcs->len; i < end;) {
        if (hlAt(p, i, end, mce)) {
            i += mce->len;
            if (--depth == 0) return i == end;
        } else if ((E.syntax->flags & HL_NESTED_COMMENTS) && hlAt(p, i, end, mcs)) {
            i += mcs->len;
            depth++;
        } else {
            i++;
        }
    }
    return 0;
}

/* True if Ctrl-/ has something to do on 'filerow'. A language without line
 * comments wraps lines in a block comment, which a blank line does not get. */
static int editorCanToggleComment(int filerow) {
    if (filerow >= E.numrows || E.syntax == NULL) return 0;
    if (E.syntax->line_comment.len) return 1;
    if (!E.syntax->block_start.len) return 0;
    erow *row = &E.row[filerow];
    for (int i = 0; i < row->size; i++)
        if (!isspace((unsigned char)row->chars[i])) return 1;
    return 0;
}

/* Ctrl-/: toggle a line comment on 'filerow'. Languages without line
 * comments (OCaml) wrap the line in "(* " ... " *)" instead, or unwrap it.
 * The cursor stays on the same character. */
static void editorToggleLineComment(int filerow) {
    if (!editorCanToggleComment(filerow)) return;

    erow *row = &E.row[filerow];
    const hldelim *scs = &E.syntax->line_comment;
    const hldelim *mcs = &E.syntax->block_start;
    const hldelim *mce = &E.syntax->block_end;
    int col = E.cx;
    int on_row = filerow == E.cy;

    int indent = 0;
    while (indent < row->size && isspace((unsigned char)row->chars[indent])) indent++;

    if (scs->len) {
        /* "// " with one space of padding, taken away again on uncomment.
         * An empty line gets no padding, which would be trailing space. */
        if (hlAt(row->chars, indent, row->size, scs)) {
            int n = scs->len;
            if (indent + n < row->size && row->chars[indent + n] == ' ') n++;
            editorRowDelRange(row, indent, n);
            col = editorColAfterDel(col, indent, n);
        } else {
            int pad = indent < row->size;
            if (pad) editorRowInsRange(row, indent, " ", 1);
            editorRowInsRange(row, indent, scs->s, scs->len);
            if (col >= indent) col += scs->len + pad;
        }
    } else {
        int end = row->size;
        while (end > indent && isspace((unsigned char)row->chars[end - 1])) end--;
        if (editorRowIsOneBlockComment(row, indent, end)) {
            /* Unwrap, taking one space of padding on each side with it. */
            int close = end - mce->len;
            if (close > indent + mcs->len && row->chars[close - 1] == ' ') close--;
            editorRowDelRange(row, close, end - close);
            col = editorColAfterDel(col, close, end - close);
            int open = mcs->len;
            if (indent + open < close && row->chars[indent + open] == ' ') open++;
            editorRowDelRange(row, indent, open);
            col = editorColAfterDel(col, indent, open);
        } else {
            editorRowInsRange(row, end, mce->s, mce->len);
            editorRowInsRange(row, end, " ", 1);
            if (col > end) col += mce->len + 1;
            editorRowInsRange(row, indent, " ", 1);
            editorRowInsRange(row, indent, mcs->s, mcs->len);
            if (col >= indent) col += mcs->len + 1;
        }
    }
    editorUpdateRow(row);
    E.dirty++;
    if (on_row) E.cx = col;
}

/* True if 'filerow' starts with at least one tab stop's worth of spaces. */
static int editorCanOutdent(int filerow) {
    if (filerow >= E.numrows || E.row[filerow].size < QUIP_TAB_STOP) return 0;
    for (int i = 0; i < QUIP_TAB_STOP; i++)
        if (E.row[filerow].chars[i] != ' ') return 0;
    return 1;
}

/* Shift-Tab: drop one tab stop's worth of leading spaces from the cursor's
 * line. The cursor and a selection anchor on that line shift left with the
 * text, stopping at the start of the line. */
static void editorOutdentLine(void) {
    int filerow = E.cy;
    if (!editorCanOutdent(filerow)) return;
    erow *row = &E.row[filerow];
    int n = QUIP_TAB_STOP;
    memmove(row->chars, row->chars + n, row->size - n + 1);
    row->size -= n;
    editorUpdateRow(row);
    E.dirty++;
    E.cx = E.cx > n ? E.cx - n : 0;
    if (E.sel_active && E.sel_anchor_row == filerow)
        E.sel_anchor_col = E.sel_anchor_col > n ? E.sel_anchor_col - n : 0;
}

static void editorDeleteSelection(void) {
    int sr, sc, er, ec;
    if (!editorSelBounds(&sr, &sc, &er, &ec)) {
        editorSelClear();
        return;
    }

    if (sr == er) {
        erow *row = &E.row[sr];
        int count = ec - sc;
        memmove(row->chars + sc, row->chars + ec, row->size - ec + 1);
        row->size -= count;
        editorUpdateRow(row);
        E.dirty++;
    } else {
        E.row[sr].chars[sc] = '\0';
        E.row[sr].size = sc;
        editorRowAppendString(&E.row[sr], E.row[er].chars + ec, E.row[er].size - ec);
        for (int i = er; i > sr; i--) editorDelRow(i);
    }

    editorSetCursorPos(sr, sc);
    E.sel_active = 0;
}

/* Put the cursor as near screen column 'rx' of its line as it can go
 * without moving the view. If the line ends left of the view, the cursor
 * parks at the left edge past the end of the line, and the next key that
 * acts on it snaps it back (see editorProcessKeypress). */
static void editorPlaceCursorInView(int rx) {
    erow *row = editorCursorRow();
    int size = row ? row->size : 0;
    int width = row ? rowCxToRx(row, size) : 0;
    int lo = E.coloff, hi = E.coloff + E.screencols - 1;
    if (rx > width) rx = width;
    if (rx < lo) rx = lo;
    if (rx > hi) rx = hi;
    if (rx >= width) {
        E.cx = size + (rx - width);
        return;
    }
    int cx = rowRxToCx(row, rx);
    /* Not on a wide character cut by either edge, which would scroll. */
    if (rowCxToRx(row, cx) < lo) cx = rowNextCx(row, cx);
    int k;
    if (cx < size && rowCxToRx(row, cx) + textCell(row->chars, cx, size, 0, &k) - 1 > hi) {
        int prev = rowPrevCx(row, cx);
        if (rowCxToRx(row, prev) >= lo) cx = prev;
    }
    E.cx = cx;
}

/* Scroll the view sideways by 'delta' columns, stopping once the end of the
 * longest line on screen is in view. The cursor keeps its column when it can
 * and is otherwise dragged along with the view. */
static void editorScrollCols(int delta) {
    int rx = editorCursorRx();
    int maxlen = 0;
    for (int y = 0; y < E.screenrows && E.rowoff + y < E.numrows; y++) {
        erow *row = &E.row[E.rowoff + y];
        int len = rowCxToRx(row, row->size);
        if (len > maxlen) maxlen = len;
    }
    int limit = maxlen - E.screencols + 1; /* +1 leaves room for the cursor */
    int coloff = E.coloff + delta;
    if (delta > 0 && coloff > limit) coloff = (limit > E.coloff) ? limit : E.coloff;
    if (coloff < 0) coloff = 0;
    E.coloff = coloff;
    editorPlaceCursorInView(rx);
}

/* Scroll the view up or down by 'delta' rows, as far as the file goes. The
 * cursor keeps its place in the file while that stays on screen, and is
 * otherwise dragged along with the view, keeping its column. */
static void editorScrollRows(int delta) {
    int max_rowoff = (E.numrows > E.screenrows) ? E.numrows - E.screenrows : 0;
    int rowoff = E.rowoff + delta;
    /* A view already past that (a search puts its match at the top) does
     * not jump back. */
    if (delta > 0 && rowoff > max_rowoff) rowoff = E.rowoff > max_rowoff ? E.rowoff : max_rowoff;
    if (rowoff < 0) rowoff = 0;
    E.rowoff = rowoff;
    int rx = editorCursorRx();
    int cy = E.cy;
    if (cy < E.rowoff) cy = E.rowoff;
    if (cy > E.rowoff + E.screenrows - 1) cy = E.rowoff + E.screenrows - 1;
    if (cy != E.cy) editorMoveToRow(cy, rx);
    editorPlaceCursorInView(editorCursorRx());
}

/* True if the cursor sits past the end of its line, which only a scroll can
 * cause. */
static int editorCursorPastEnd(void) {
    erow *row = editorCursorRow();
    return E.cx > (row ? row->size : 0);
}

/* Move the cursor to the cell the mouse is on (E.mouse_cx/cy), or to the
 * nearest character boundary: a click on the far half of a TAB or a wide
 * character lands after it. A click below the last line lands at the end of
 * the last line, and one left of the text (on the gutter, or a drag into
 * the sidebar) at the left edge. */
static void editorMouseSetCursor(void) {
    if (E.mouse_cy >= 0 && E.mouse_cy < E.screenrows) E.cy = E.rowoff + E.mouse_cy;
    if (E.cy >= E.numrows) {
        E.cy = E.numrows ? E.numrows - 1 : 0;
        E.cx = E.numrows ? E.row[E.cy].size : 0;
        return;
    }
    erow *row = &E.row[E.cy];
    int text_cx = E.mouse_cx - E.ft_width - E.ln_width; /* physical → text */
    if (text_cx < 0) text_cx = 0;
    if (text_cx > E.screencols - 1) text_cx = E.screencols - 1;
    int rx = E.coloff + text_cx;
    int cx = rowRxToCx(row, rx);
    if (cx < row->size && rowCxToRx(row, cx) < rx) cx = rowNextCx(row, cx);
    E.cx = cx;
}

/* Character class for double-click word selection: a run of word
 * characters, of whitespace, or of punctuation selects as one unit. Bytes
 * >= 0x80 count as word characters so UTF-8 words stay whole. */
static int editorWordClass(int c) {
    if (isalnum(c) || c == '_' || c >= 0x80) return 2;
    if (isspace(c)) return 0;
    return 1;
}

/* Bounds [*start,*end) of the run of same-class characters at 'col' in
 * 'row'. A column past the end of the line picks the line's last run. */
static void editorWordAt(erow *row, int col, int *start, int *end) {
    if (row->size == 0) {
        *start = *end = 0;
        return;
    }
    if (col >= row->size) col = row->size - 1;
    unsigned char *s = (unsigned char *)row->chars;
    int cls = editorWordClass(s[col]);
    int a = col, b = col + 1;
    while (a > 0 && editorWordClass(s[a - 1]) == cls) a--;
    while (b < row->size && editorWordClass(s[b]) == cls) b++;
    *start = a;
    *end = b;
}

/* File column of the character under the mouse, after editorMouseSetCursor.
 * That puts the cursor on the nearest boundary, which for a click on the far
 * half of a TAB or wide character is just past it; step back so that the
 * character itself counts as clicked. */
static int editorMouseCharCol(void) {
    int col = E.cx;
    if (E.cy >= E.numrows || col == 0) return col;
    erow *row = &E.row[E.cy];
    int rx = E.coloff + E.mouse_cx - E.ft_width - E.ln_width;
    if (rowCxToRx(row, col) > rx) col = rowPrevCx(row, col);
    return col;
}

/* The unit under the mouse for a multi-click: the word (clicks == 2) or the
 * whole line including its newline (clicks == 3), as (sr,sc) up to (er,ec).
 * The last line has no newline, so its unit ends at the end of the line.
 * Moves the cursor to the mouse first. Returns 0 if there is no text. */
static int editorMouseUnit(int clicks, int *sr, int *sc, int *er, int *ec) {
    editorMouseSetCursor();
    int filerow = E.cy;
    if (filerow >= E.numrows) return 0;
    *sr = *er = filerow;
    if (clicks == 2) {
        editorWordAt(&E.row[filerow], editorMouseCharCol(), sc, ec);
    } else if (filerow + 1 < E.numrows) {
        *sc = 0;
        *er = filerow + 1;
        *ec = 0;
    } else {
        *sc = 0;
        *ec = E.row[filerow].size;
    }
    return 1;
}

/* Select exactly the clicked unit. The cursor goes to the unit's end when
 * that is on the clicked row, else to its start: a line's newline ends on
 * the next row, which may be off screen, and the view must not scroll. */
static void editorMouseSelectClickedUnit(void) {
    E.sel_active = 1;
    if (E.unit_er == E.unit_sr) {
        E.sel_anchor_row = E.unit_sr;
        E.sel_anchor_col = E.unit_sc;
        editorSetCursorPos(E.unit_er, E.unit_ec);
    } else {
        E.sel_anchor_row = E.unit_er;
        E.sel_anchor_col = E.unit_ec;
        editorSetCursorPos(E.unit_sr, E.unit_sc);
    }
}

/* Start a double- or triple-click selection of the unit under the mouse. */
static void editorMouseSelectUnit(int clicks) {
    int sr, sc, er, ec;
    if (!editorMouseUnit(clicks, &sr, &sc, &er, &ec)) {
        sr = er = E.cy;
        sc = ec = E.cx;
    }
    E.unit_sr = sr;
    E.unit_sc = sc;
    E.unit_er = er;
    E.unit_ec = ec;
    editorMouseSelectClickedUnit();
}

/* Extend a multi-click selection by whole units (words or lines) toward the
 * mouse, keeping the originally clicked unit selected. */
static void editorMouseExtendUnits(int clicks) {
    int sr, sc, er, ec;
    if (!editorMouseUnit(clicks, &sr, &sc, &er, &ec)) return;
    if (sr < E.unit_sr || (sr == E.unit_sr && sc < E.unit_sc)) {
        E.sel_anchor_row = E.unit_er;
        E.sel_anchor_col = E.unit_ec;
        editorSetCursorPos(sr, sc);
    } else if (er > E.unit_er || (er == E.unit_er && ec > E.unit_ec)) {
        E.sel_anchor_row = E.unit_sr;
        E.sel_anchor_col = E.unit_sc;
        editorSetCursorPos(er, ec);
    } else {
        editorMouseSelectClickedUnit(); /* back inside the clicked unit */
    }
}

/* Register a left press and return how many presses in a row it makes:
 * 2 is a double-click, 3 a triple-click (same cell, each press within 400ms
 * of the previous one). */
static int editorCountClick(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long ms =
        (now.tv_sec - E.click_time.tv_sec) * 1000 + (now.tv_nsec - E.click_time.tv_nsec) / 1000000;
    if (E.click_count && ms <= 400 && E.mouse_cx == E.click_cx && E.mouse_cy == E.click_cy)
        E.click_count++;
    else
        E.click_count = 1;
    if (E.click_count > 3) E.click_count = 1; /* a fourth press starts over */
    E.click_time = now;
    E.click_cx = E.mouse_cx;
    E.click_cy = E.mouse_cy;
    return E.click_count;
}

/* Process events arriving from the standard input, which is, the user
 * is typing stuff on the terminal. */
#define QUIP_QUIT_TIMES   3
#define QUIP_HSCROLL_COLS 4 /* Columns moved per horizontal wheel step. */
#define QUIP_VSCROLL_ROWS 3 /* Rows moved per vertical wheel step. */
void editorProcessKeypress(int fd) {
    /* When the file is modified, requires Ctrl-q to be pressed N times
     * before actually quitting. */
    static int quit_times = QUIP_QUIT_TIMES;

    int c = editorReadKey(fd);

    /* While the sidebar has focus, navigation keys drive the tree. Ctrl-E
     * closes it and Ctrl-Q still quits; everything else is consumed. */
    if (E.ft_open && E.ft_focus) {
        if (c == CTRL_E) {
            ftToggle();
            return;
        }
        /* Mouse buttons go to the main handler, which knows whether the
         * press landed in the sidebar or the text. */
        if (c != CTRL_Q && c != SCROLL_LEFT && c != SCROLL_RIGHT && c != MOUSE_CLICK &&
            c != MOUSE_DRAG && c != MOUSE_RELEASE) {
            ftHandleKey(c);
            return;
        }
    }

    /* After a sideways scroll the cursor may be parked past the end of a
     * short line. Snap it back to the line end before a key acts on it, so
     * edits and selections never see a column beyond the line. Vertical
     * motion, scrolling, mouse events and view-neutral commands keep the
     * view where the user put it. */
    if (editorCursorPastEnd()) {
        switch (c) {
        case KEY_NULL:
        case CTRL_Q:
        case CTRL_S:
        case CTRL_E:
        case CTRL_F:
        case CTRL_L:
        case ARROW_UP:
        case ARROW_DOWN:
        case PAGE_UP:
        case PAGE_DOWN:
        case ALT_ARROW_UP:
        case ALT_ARROW_DOWN:
        case SCROLL_UP:
        case SCROLL_DOWN:
        case SCROLL_LEFT:
        case SCROLL_RIGHT:
        case MOUSE_CLICK:
        case MOUSE_DRAG:
        case MOUSE_RELEASE: break;
        default: {
            E.cx = E.cy < E.numrows ? E.row[E.cy].size : 0;
            break;
        }
        }
    }

    switch (c) {
    case CTRL_E: /* Ctrl-e — toggle / focus the file tree */
        if (E.ft_open)
            E.ft_focus = 1;
        else
            ftToggle();
        break;
    case CTRL_Z: /* Ctrl-z — undo */ editorUndo(); break;
    case CTRL_Y: /* Ctrl-y — redo */ editorRedo(); break;
    case ENTER: /* Enter */
        editorPushSnapshot();
        undo_last_insert = 0;
        if (E.sel_active) editorDeleteSelection();
        editorInsertNewline();
        break;
    case CTRL_C: /* Ctrl-c — copy selection */
        if (E.sel_active) editorCopySelection();
        break;
    case CTRL_X: /* Ctrl-x — cut selection */
        if (E.sel_active) {
            editorPushSnapshot();
            undo_last_insert = 0;
            editorCopySelection();
            editorDeleteSelection();
        }
        break;
    case CTRL_V: /* Ctrl-v — paste */
        editorPushSnapshot();
        undo_last_insert = 0;
        editorSelClear();
        editorPaste();
        break;
    case CTRL_Q: /* Ctrl-q */
        /* Quit if the file was already saved. */
        if (E.dirty && quit_times) {
            editorSetStatusMessage("WARNING!!! File has unsaved changes. "
                                   "Press Ctrl-Q %d more times to quit.",
                                   quit_times);
            quit_times--;
            return;
        }
        ftFreeNode(E.ft_root);
        exit(0);
        break;
    case CTRL_S: /* Ctrl-s */
        /* Make the saved text a step of its own, so undo and redo know
         * when the document matches the file again. */
        editorCommitPending();
        if (editorSave() == 0) editorUndoMarkSaved();
        break;
    case CTRL_F: editorFind(fd); break;
    case CTRL_SLASH:
        if (!editorCanToggleComment(E.cy)) break;
        editorPushSnapshot();
        undo_last_insert = 0;
        editorSelClear();
        editorToggleLineComment(E.cy);
        break;
    case SHIFT_TAB: /* Shift-Tab — outdent the current line */
        if (!editorCanOutdent(E.cy)) break;
        editorPushSnapshot();
        undo_last_insert = 0;
        editorOutdentLine();
        break;
    case BACKSPACE: /* Backspace */
    case CTRL_H:    /* Ctrl-h */
    case DEL_KEY:
        editorPushSnapshot();
        undo_last_insert = 0;
        if (E.sel_active)
            editorDeleteSelection();
        else
            editorDelChar();
        break;
    case MOUSE_CLICK:
        undo_last_insert = 0;
        if (E.ft_open && E.mouse_cx < E.ft_width) {
            /* Click inside the sidebar: focus it and act on the row. */
            E.click_count = 0;
            E.ft_focus = 1;
            int idx = E.ft_scroll + E.mouse_cy;
            if (idx >= 0 && idx < E.ft_vcount) {
                E.ft_sel = idx;
                ftActivate();
            }
            break;
        }
        E.ft_focus = 0;
        if (editorCountClick() >= 2) {
            /* Double-click selects the word, triple-click the line. */
            editorMouseSelectUnit(E.click_count);
            break;
        }
        editorMouseSetCursor();
        E.sel_active = 1;
        E.sel_anchor_row = E.cy;
        E.sel_anchor_col = E.cx;
        break;
    case MOUSE_DRAG:
        if (!E.sel_active) break;
        if (E.click_count >= 2)
            editorMouseExtendUnits(E.click_count);
        else
            editorMouseSetCursor();
        break;
    case MOUSE_RELEASE:
        if (E.ft_open && E.mouse_cx < E.ft_width) break;
        if (E.click_count >= 2) break; /* keep the word/line selection */
        editorMouseSetCursor();
        /* If anchor == cursor the user just clicked; clear the selection. */
        if (E.sel_anchor_row == E.cy && E.sel_anchor_col == E.cx) E.sel_active = 0;
        break;
    case SCROLL_UP:
        undo_last_insert = 0;
        editorScrollRows(-QUIP_VSCROLL_ROWS);
        break;
    case SCROLL_DOWN:
        undo_last_insert = 0;
        editorScrollRows(QUIP_VSCROLL_ROWS);
        break;
    case SCROLL_LEFT:
        undo_last_insert = 0;
        editorScrollCols(-QUIP_HSCROLL_COLS);
        break;
    case SCROLL_RIGHT:
        undo_last_insert = 0;
        editorScrollCols(QUIP_HSCROLL_COLS);
        break;
    case KEY_NULL: break;
    case PAGE_UP:
    case PAGE_DOWN: {
        undo_last_insert = 0;
        editorSelClear();
        /* A screenful beyond the top or bottom line on screen, so the line
         * at one edge ends up at the other, keeping the column. Not below
         * the line after the last one, where Down stops too. */
        int row = c == PAGE_UP ? E.rowoff - E.screenrows : E.rowoff + 2 * E.screenrows - 1;
        if (row < 0) row = 0;
        if (row > E.numrows) row = E.numrows;
        editorMoveToRow(row, editorCursorRx());
        break;
    }

    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
        undo_last_insert = 0;
        editorSelClear();
        editorMoveCursor(c);
        break;
    case SHIFT_ARROW_UP:
        undo_last_insert = 0;
        editorSelSetAnchor();
        editorMoveCursor(ARROW_UP);
        break;
    case SHIFT_ARROW_DOWN:
        undo_last_insert = 0;
        editorSelSetAnchor();
        editorMoveCursor(ARROW_DOWN);
        break;
    case SHIFT_ARROW_LEFT:
        undo_last_insert = 0;
        editorSelSetAnchor();
        editorMoveCursor(ARROW_LEFT);
        break;
    case SHIFT_ARROW_RIGHT:
        undo_last_insert = 0;
        editorSelSetAnchor();
        editorMoveCursor(ARROW_RIGHT);
        break;
    case SHIFT_ALT_ARROW_LEFT:
        undo_last_insert = 0;
        editorSelSetAnchor();
        editorMoveWordLeft();
        break;
    case SHIFT_ALT_ARROW_RIGHT:
        undo_last_insert = 0;
        editorSelSetAnchor();
        editorMoveWordRight();
        break;
    case SHIFT_ALT_ARROW_UP:
        undo_last_insert = 0;
        editorSelSetAnchor();
        editorMoveCursor(ARROW_UP);
        break;
    case SHIFT_ALT_ARROW_DOWN:
        undo_last_insert = 0;
        editorSelSetAnchor();
        editorMoveCursor(ARROW_DOWN);
        break;
    case SHIFT_CTRL_ARROW_LEFT:
        undo_last_insert = 0;
        editorSelSetAnchor();
        editorMoveLineStart();
        break;
    case SHIFT_CTRL_ARROW_RIGHT:
        undo_last_insert = 0;
        editorSelSetAnchor();
        editorMoveLineEnd();
        break;
    case SHIFT_CTRL_ARROW_UP:
        undo_last_insert = 0;
        editorSelSetAnchor();
        editorMoveFileStart();
        break;
    case SHIFT_CTRL_ARROW_DOWN:
        undo_last_insert = 0;
        editorSelSetAnchor();
        editorMoveFileEnd();
        break;
    case CTRL_ARROW_UP:
        undo_last_insert = 0;
        editorSelClear();
        editorMoveFileStart();
        break;
    case CTRL_ARROW_DOWN:
        undo_last_insert = 0;
        editorSelClear();
        editorMoveFileEnd();
        break;
    case CTRL_ARROW_LEFT:
    case HOME_KEY:
        undo_last_insert = 0;
        editorSelClear();
        editorMoveLineStart();
        break;
    case CTRL_ARROW_RIGHT:
    case END_KEY:
        undo_last_insert = 0;
        editorSelClear();
        editorMoveLineEnd();
        break;
    case ALT_ARROW_LEFT:
        undo_last_insert = 0;
        editorSelClear();
        editorMoveWordLeft();
        break;
    case ALT_ARROW_RIGHT:
        undo_last_insert = 0;
        editorSelClear();
        editorMoveWordRight();
        break;
    case ALT_ARROW_UP:
        undo_last_insert = 0;
        editorSelClear();
        editorMoveCursor(ARROW_UP);
        break;
    case ALT_ARROW_DOWN:
        undo_last_insert = 0;
        editorSelClear();
        editorMoveCursor(ARROW_DOWN);
        break;
    case CTRL_L: undo_last_insert = 0; break;
    case ESC:
        undo_last_insert = 0;
        editorSelClear();
        break;
    default:
        /* Soft key codes are not text; never insert one as a byte. Nor
         * an unbound Ctrl key: it would put a control byte in the file. */
        if (c >= ARROW_LEFT && c < KEY_CHAR) break;
        if (c >= 0 && c < 0x20 && c != TAB) break; /* UTF-8 bytes are < 0 */
        if (!undo_last_insert || E.sel_active) {
            editorPushSnapshot();
            undo_last_insert = 1;
        }
        if (E.sel_active) editorDeleteSelection();
        if (c == TAB) {
            editorInsertTab();
        } else if (c >= KEY_CHAR) {
            char u[4];
            editorInsertBytes(u, utf8Encode(u, (uint32_t)(c - KEY_CHAR)));
        } else {
            editorInsertChar(c);
        }
        break;
    }

    quit_times = QUIP_QUIT_TIMES; /* Reset it to the original value. */
}
