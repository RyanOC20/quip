/* Keys and mouse: cursor motion, selection, editing commands and the key dispatcher. */
#include "quip.h"

/* Insert spaces up to the next tab stop instead of a TAB character. */
static void editorInsertTab(void) {
    int filerow = E.rowoff + E.cy;
    int filecol = E.coloff + E.cx;
    int rcol = filerow < E.numrows ? editorCharToRenderCol(&E.row[filerow], filecol) : 0;
    int n = QUIP_TAB_STOP - (rcol % QUIP_TAB_STOP);
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
    int cr = E.rowoff + E.cy, cc = E.coloff + E.cx;
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
        E.sel_anchor_row = E.rowoff + E.cy;
        E.sel_anchor_col = E.coloff + E.cx;
    }
}

void editorSetCol(int new_col) {
    if (new_col < E.screencols) {
        E.cx = new_col;
        E.coloff = 0;
    } else {
        E.coloff = new_col - E.screencols + 1;
        E.cx = E.screencols - 1;
    }
}

/* Move the cursor to file position (row,col), scrolling it into view. */
void editorSetCursorPos(int row, int col) {
    if (row < E.rowoff) {
        E.rowoff = row;
        E.cy = 0;
    } else if (row >= E.rowoff + E.screenrows) {
        E.rowoff = row - E.screenrows + 1;
        E.cy = E.screenrows - 1;
    } else {
        E.cy = row - E.rowoff;
    }
    editorSetCol(col);
}

/* Handle cursor position change because arrow keys were pressed. */
static void editorMoveCursor(int key) {
    int filerow = E.rowoff + E.cy;
    int filecol = E.coloff + E.cx;
    int rowlen;
    erow *row = (filerow >= E.numrows) ? NULL : &E.row[filerow];

    switch (key) {
    case ARROW_LEFT:
        if (E.cx == 0) {
            if (E.coloff) {
                E.coloff--;
            } else {
                /* To the end of the previous line, scrolling up when the
                 * cursor is on the top screen row. */
                if (filerow > 0) editorSetCursorPos(filerow - 1, E.row[filerow - 1].size);
            }
        } else {
            E.cx -= 1;
        }
        break;
    case ARROW_RIGHT:
        if (row && filecol < row->size) {
            if (E.cx == E.screencols - 1) {
                E.coloff++;
            } else {
                E.cx += 1;
            }
        } else if (row && filecol == row->size) {
            E.cx = 0;
            E.coloff = 0;
            if (E.cy == E.screenrows - 1) {
                E.rowoff++;
            } else {
                E.cy += 1;
            }
        }
        break;
    case ARROW_UP:
        if (E.cy == 0) {
            if (E.rowoff) E.rowoff--;
        } else {
            E.cy -= 1;
        }
        break;
    case ARROW_DOWN:
        if (filerow < E.numrows) {
            if (E.cy == E.screenrows - 1) {
                E.rowoff++;
            } else {
                E.cy += 1;
            }
        }
        break;
    }
    /* Fix cx if the current line has not enough chars. */
    filerow = E.rowoff + E.cy;
    filecol = E.coloff + E.cx;
    row = (filerow >= E.numrows) ? NULL : &E.row[filerow];
    rowlen = row ? row->size : 0;
    if (filecol > rowlen) {
        E.cx -= filecol - rowlen;
        /* The line ends left of the view: scroll back so it is visible. */
        if (E.cx < 0) editorSetCol(rowlen);
    }
}

static void editorMoveWordRight(void) {
    int filerow = E.rowoff + E.cy;
    if (filerow >= E.numrows) return;
    erow *row = &E.row[filerow];
    int pos = E.coloff + E.cx;

    if (pos >= row->size) {
        if (filerow + 1 < E.numrows) {
            editorMoveCursor(ARROW_DOWN);
            E.cx = 0;
            E.coloff = 0;
        }
        return;
    }
    while (pos < row->size && editorIsSeparator(row->chars[pos])) pos++;
    while (pos < row->size && !editorIsSeparator(row->chars[pos])) pos++;
    editorSetCol(pos);
}

static void editorMoveWordLeft(void) {
    int filerow = E.rowoff + E.cy;
    if (filerow >= E.numrows) return;
    erow *row = &E.row[filerow];
    int pos = E.coloff + E.cx;

    if (pos == 0) {
        if (filerow > 0) {
            editorMoveCursor(ARROW_UP);
            editorSetCol(E.row[filerow - 1].size);
        }
        return;
    }
    pos--;
    while (pos > 0 && editorIsSeparator(row->chars[pos])) pos--;
    while (pos > 0 && !editorIsSeparator(row->chars[pos - 1])) pos--;
    editorSetCol(pos);
}

static void editorMoveLineStart(void) {
    int filerow = E.rowoff + E.cy;
    if (filerow >= E.numrows) {
        E.cx = 0;
        E.coloff = 0;
        return;
    }
    erow *row = &E.row[filerow];

    int indent = 0;
    while (indent < row->size && isspace(row->chars[indent])) indent++;

    int filecol = E.coloff + E.cx;
    if (filecol != indent)
        editorSetCol(indent);
    else {
        E.cx = 0;
        E.coloff = 0;
    }
}

static void editorMoveLineEnd(void) {
    int filerow = E.rowoff + E.cy;
    if (filerow >= E.numrows) return;
    editorSetCol(E.row[filerow].size);
}

static void editorMoveFileStart(void) {
    E.cx = 0;
    E.cy = 0;
    E.rowoff = 0;
    E.coloff = 0;
}

static void editorMoveFileEnd(void) {
    if (E.numrows == 0) return;
    int last = E.numrows - 1;
    if (last < E.screenrows) {
        E.cy = last;
        E.rowoff = 0;
    } else {
        E.rowoff = last - E.screenrows + 1;
        E.cy = E.screenrows - 1;
    }
    editorSetCol(E.row[last].size);
}

/* Remove 'len' chars at 'at' from 'row'. */
static void editorRowDelRange(erow *row, int at, int len) {
    memmove(row->chars + at, row->chars + at + len, row->size - at - len + 1);
    row->size -= len;
}

/* Insert the 'len' chars of 's' at 'at' in 'row'. */
static void editorRowInsRange(erow *row, int at, const char *s, int len) {
    row->chars = growBuf(row->chars, &row->chars_cap, row->size + len + 1);
    memmove(row->chars + at + len, row->chars + at, row->size - at + 1);
    memcpy(row->chars + at, s, len);
    row->size += len;
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
    int col = E.coloff + E.cx;
    int on_row = filerow == E.rowoff + E.cy;

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
    if (on_row) editorSetCol(col);
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
    int filerow = E.rowoff + E.cy;
    if (!editorCanOutdent(filerow)) return;
    erow *row = &E.row[filerow];
    int n = QUIP_TAB_STOP;
    memmove(row->chars, row->chars + n, row->size - n + 1);
    row->size -= n;
    editorUpdateRow(row);
    E.dirty++;
    int col = E.coloff + E.cx;
    editorSetCol(col > n ? col - n : 0);
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

/* Put the cursor as close to file column 'col' as possible without moving
 * the view. If the cursor's line ends left of the view, the cursor parks at
 * the left edge past the end of the line, and the next key that acts on it
 * snaps it back (see editorProcessKeypress). */
static void editorPlaceCursorInView(int col) {
    int filerow = E.rowoff + E.cy;
    int len = (filerow < E.numrows) ? E.row[filerow].size : 0;
    if (col > len) col = len;
    if (col < E.coloff) col = E.coloff;
    if (col > E.coloff + E.screencols - 1) col = E.coloff + E.screencols - 1;
    E.cx = col - E.coloff;
}

/* Scroll the view sideways by 'delta' columns, stopping once the end of the
 * longest line on screen is in view. The cursor keeps its column when it can
 * and is otherwise dragged along with the view. */
static void editorScrollCols(int delta) {
    int col = E.coloff + E.cx;
    int maxlen = 0;
    for (int y = 0; y < E.screenrows && E.rowoff + y < E.numrows; y++) {
        int len = E.row[E.rowoff + y].rsize;
        if (len > maxlen) maxlen = len;
    }
    int limit = maxlen - E.screencols + 1; /* +1 leaves room for the cursor */
    int coloff = E.coloff + delta;
    if (delta > 0 && coloff > limit) coloff = (limit > E.coloff) ? limit : E.coloff;
    if (coloff < 0) coloff = 0;
    E.coloff = coloff;
    editorPlaceCursorInView(col);
}

/* True if the cursor sits past the end of its line, which only a scroll can
 * cause. */
static int editorCursorPastEnd(void) {
    int filerow = E.rowoff + E.cy;
    int len = (filerow < E.numrows) ? E.row[filerow].size : 0;
    return E.coloff + E.cx > len;
}

/* Moves cursor to E.mouse_cx/cy, converting render column → file column.
 * A click below the last line lands at the end of the last line. */
static void editorMouseSetCursor(void) {
    if (E.mouse_cy >= 0 && E.mouse_cy < E.screenrows) E.cy = E.mouse_cy;
    int last = E.numrows ? E.numrows - 1 : 0;
    if (E.rowoff + E.cy > last) {
        if (last < E.rowoff) E.rowoff = last;
        E.cy = last - E.rowoff;
        editorSetCol(E.numrows ? E.row[last].size : 0);
        return;
    }
    int text_cx = E.mouse_cx - E.ft_width - E.ln_width;       /* physical → text */
    if (text_cx < 0 && E.mouse_cx >= E.ft_width) text_cx = 0; /* in gutter */
    if (text_cx >= 0 && text_cx < E.screencols) {
        int file_row = E.rowoff + E.cy;
        if (file_row < E.numrows) {
            editorSetCol(editorRenderToCharCol(&E.row[file_row], E.coloff + text_cx));
        } else {
            editorSetCol(0);
        }
    }
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
 * That puts the cursor on the nearest boundary, which for a click inside a
 * TAB is just past it; step back so the TAB itself counts as clicked. */
static int editorMouseCharCol(void) {
    int filerow = E.rowoff + E.cy;
    int col = E.coloff + E.cx;
    if (filerow >= E.numrows || col == 0) return col;
    int rcol = E.coloff + E.mouse_cx - E.ft_width - E.ln_width;
    if (editorCharToRenderCol(&E.row[filerow], col) > rcol) col--;
    return col;
}

/* The unit under the mouse for a multi-click: the word (clicks == 2) or the
 * whole line including its newline (clicks == 3), as (sr,sc) up to (er,ec).
 * The last line has no newline, so its unit ends at the end of the line.
 * Moves the cursor to the mouse first. Returns 0 if there is no text. */
static int editorMouseUnit(int clicks, int *sr, int *sc, int *er, int *ec) {
    editorMouseSetCursor();
    int filerow = E.rowoff + E.cy;
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
        int row = E.rowoff + E.cy, col = E.coloff + E.cx;
        sr = er = row;
        sc = ec = col;
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
            int filerow = E.rowoff + E.cy;
            editorSetCol(filerow < E.numrows ? E.row[filerow].size : 0);
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
        if (!editorCanToggleComment(E.rowoff + E.cy)) break;
        editorPushSnapshot();
        undo_last_insert = 0;
        editorSelClear();
        editorToggleLineComment(E.rowoff + E.cy);
        break;
    case SHIFT_TAB: /* Shift-Tab — outdent the current line */
        if (!editorCanOutdent(E.rowoff + E.cy)) break;
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
        E.sel_anchor_row = E.rowoff + E.cy;
        E.sel_anchor_col = E.coloff + E.cx;
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
        if (E.sel_anchor_row == E.rowoff + E.cy && E.sel_anchor_col == E.coloff + E.cx)
            E.sel_active = 0;
        break;
    case SCROLL_UP: {
        undo_last_insert = 0;
        int file_row = E.rowoff + E.cy;
        int new_rowoff = E.rowoff - 3;
        if (new_rowoff < 0) new_rowoff = 0;
        E.rowoff = new_rowoff;
        E.cy = file_row - E.rowoff;
        if (E.cy >= E.screenrows) E.cy = E.screenrows - 1;
        if (E.cy < 0) E.cy = 0;
        editorPlaceCursorInView(E.coloff + E.cx);
        break;
    }
    case SCROLL_DOWN: {
        undo_last_insert = 0;
        int file_row = E.rowoff + E.cy;
        int max_rowoff = (E.numrows > E.screenrows) ? E.numrows - E.screenrows : 0;
        int new_rowoff = E.rowoff + 3;
        if (new_rowoff > max_rowoff) new_rowoff = max_rowoff;
        E.rowoff = new_rowoff;
        E.cy = file_row - E.rowoff;
        if (E.cy >= E.screenrows) E.cy = E.screenrows - 1;
        if (E.cy < 0) E.cy = 0;
        editorPlaceCursorInView(E.coloff + E.cx);
        break;
    }
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
    case PAGE_DOWN:
        undo_last_insert = 0;
        editorSelClear();
        if (c == PAGE_UP && E.cy != 0)
            E.cy = 0;
        else if (c == PAGE_DOWN && E.cy != E.screenrows - 1)
            E.cy = E.screenrows - 1;
        /* Not below the line after the last one, where Down stops too: a
         * short file does not fill the screen. */
        if (E.rowoff + E.cy > E.numrows) E.cy = E.numrows - E.rowoff;
        if (E.cy < 0) E.cy = 0;
        {
            int times = E.screenrows;
            while (times--) editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
        }
        break;

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
        if (c >= ARROW_LEFT) break;
        if (c >= 0 && c < 0x20 && c != TAB) break; /* UTF-8 bytes are < 0 */
        if (!undo_last_insert || E.sel_active) {
            editorPushSnapshot();
            undo_last_insert = 1;
        }
        if (E.sel_active) editorDeleteSelection();
        if (c == TAB)
            editorInsertTab();
        else
            editorInsertChar(c);
        break;
    }

    quit_times = QUIP_QUIT_TIMES; /* Reset it to the original value. */
}
