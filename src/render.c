/* Drawing the screen: text, gutter, sidebar and status bar. */
#include "quip.h"

/* Replace control characters in 's', and bytes that are not valid UTF-8,
 * with '?', one per byte, so that the text is safe to print and its width
 * is known. */
void editorSanitize(char *s, int n) {
    int i = 0;
    while (i < n) {
        int k = editorCtrlLen(s, i, n);
        if (k == 0) {
            uint32_t cp;
            k = utf8Decode(s, i, n, &cp);
            if (k) {
                i += k;
                continue;
            }
            k = 1;
        }
        while (k--) s[i++] = '?';
    }
}

void abAppend(struct abuf *ab, const char *s, int len) {
    if (ab->len + len > ab->cap) {
        int nc = ab->cap ? ab->cap : 1024;
        while (nc < ab->len + len) nc *= 2;
        char *new = realloc(ab->b, nc);
        if (new == NULL) return;
        ab->b = new;
        ab->cap = nc;
    }
    memcpy(ab->b + ab->len, s, len);
    ab->len += len;
}

static void abFree(struct abuf *ab) { free(ab->b); }

/* Character column of the cursor: UTF-8 continuation bytes before it are not
 * counted, so a multibyte character advances the column by one. */
static int editorCursorCharCol(void) {
    if (E.cy >= E.numrows) return E.cx;
    erow *row = &E.row[E.cy];
    int bytes = E.cx < row->size ? E.cx : row->size, n = E.cx - bytes;
    for (int j = 0; j < bytes; j++)
        if (((unsigned char)row->chars[j] & 0xC0) != 0x80) n++;
    return n;
}

/* Bracket matching. When the cursor is over a bracket, it and its partner
 * get a gray background; a bracket without a partner gets a red one. */
#define BRACKET_NONE      0
#define BRACKET_MATCHED   1
#define BRACKET_UNMATCHED 2

/* True if the render column holds code: brackets in strings and comments
 * are text, so they neither match nor count toward nesting. */
static int editorIsCodeAt(const erow *row, int rcol) {
    unsigned char h = row->hl[rcol];
    return h != HL_STRING && h != HL_COMMENT && h != HL_MLCOMMENT;
}

/* If the cursor is over a bracket in code, store its row and render column
 * in *brow,*bcol and look for its partner of the same kind, (), [] or {},
 * skipping nested pairs. On BRACKET_MATCHED the partner's position is in
 * *mrow,*mcol. Scans the whole file at worst, which is a plain byte loop. */
static int editorFindBracketPair(int *brow, int *bcol, int *mrow, int *mcol) {
    static const char open[] = "([{", close[] = ")]}";
    int y = E.cy, col = E.cx;
    if (y >= E.numrows) return BRACKET_NONE;
    erow *row = &E.row[y];
    if (col >= row->size) return BRACKET_NONE;

    char c = row->chars[col];
    const char *o = c ? strchr(open, c) : NULL;
    const char *cl = c ? strchr(close, c) : NULL;
    if (!o && !cl) return BRACKET_NONE;
    int x = rowCxToRidx(row, col);
    if (!editorIsCodeAt(row, x)) return BRACKET_NONE;

    char want = o ? close[o - open] : open[cl - close];
    int dir = o ? 1 : -1, depth = 0;
    *brow = y;
    *bcol = x;
    /* The row in a local, so the loop never goes back to the global E. */
    const erow *r = row;
    for (;;) {
        x += dir;
        while (x < 0 || x >= r->rsize) {
            y += dir;
            if (y < 0 || y >= E.numrows) return BRACKET_UNMATCHED;
            r = &E.row[y];
            x = dir > 0 ? 0 : r->rsize - 1;
        }
        char ch = r->render[x];
        if (ch != c && ch != want) continue;
        if (!editorIsCodeAt(r, x)) continue;
        if (ch == c) {
            depth++;
        } else if (depth-- == 0) {
            *mrow = y;
            *mcol = x;
            return BRACKET_MATCHED;
        }
    }
}

#define LN_MIN_DIGITS 3  /* Gutter fits at least this many digits. */
#define LN_MIN_TEXT   20 /* Hide the gutter rather than leave fewer text cols. */

/* Recompute the text width from the terminal width, the sidebar and the line
 * number gutter, whose width follows the number of digits in the last line
 * number. 'E.screencols' always means text columns, so the scroll math never
 * needs to know about either pane. */
void editorUpdateGeometry(void) {
    int digits = 1;
    for (int n = E.numrows; n >= 10; n /= 10) digits++;
    if (digits < LN_MIN_DIGITS) digits = LN_MIN_DIGITS;
    int avail = E.termcols - E.ft_width;
    E.ln_width = (avail - (digits + 1) >= LN_MIN_TEXT) ? digits + 1 : 0;
    E.screencols = avail - E.ln_width;
    if (E.screencols < 1) E.screencols = 1;
}

/* Scroll the view so that the cursor is on screen. This runs before every
 * redraw, so commands only move the cursor, in file coordinates, and leave
 * the view to this. It scrolls as little as shows the cursor and the whole
 * character under it, except that a cursor gone off the left edge on a line
 * that fits on screen takes the view back to column 0, to show all of it. */
void editorScroll(void) {
    if (E.cy < E.rowoff) E.rowoff = E.cy;
    if (E.cy >= E.rowoff + E.screenrows) E.rowoff = E.cy - E.screenrows + 1;
    if (E.rowoff < 0) E.rowoff = 0;

    erow *row = E.cy < E.numrows ? &E.row[E.cy] : NULL;
    int rx = row ? rowCxToRx(row, E.cx) : E.cx, w = 1;
    if (row && E.cx < row->size && row->chars[E.cx] != TAB) {
        int k;
        w = textCell(row->chars, E.cx, row->size, rx, &k);
        if (w > E.screencols) w = E.screencols;
        if (w < 1) w = 1;
    }
    if (rx < E.coloff) {
        E.coloff = rx;
        if (!row || rowCxToRx(row, row->size) < E.screencols) E.coloff = 0;
    }
    if (rx + w > E.coloff + E.screencols) E.coloff = rx + w - E.screencols;
}
/* Draw the gutter cell for 'filerow': its right-aligned line number and a
 * separating space, with the cursor's line brighter than the rest. Rows past
 * the end of the file get a '~' marker instead. */
static void editorDrawGutter(struct abuf *ab, int filerow) {
    char buf[32];
    if (filerow >= E.numrows) {
        int len = snprintf(buf, sizeof(buf), "%-*s", E.ln_width, "~");
        abAppend(ab, buf, len);
        return;
    }
    int current = (filerow == E.cy);
    if (current)
        abAppendLit(ab, VT_BOLD);
    else
        abAppendLit(ab, VT_FG_GRAY);
    int len = snprintf(buf, sizeof(buf), "%*d ", E.ln_width - 1, filerow + 1);
    abAppend(ab, buf, len);
    abAppendLit(ab, VT_RESET);
}

/* This function writes the whole screen using VT100 escape characters
 * starting from the logical state of the editor in the global state 'E'. */
void editorRefreshScreen(void) {
    int y;
    erow *r;
    char buf[32];
    struct abuf ab = ABUF_INIT;

    editorUpdateGeometry();
    editorScroll();
    abAppendLit(&ab, VT_CURSOR_HIDE); /* Hide cursor. */
    abAppendLit(&ab, VT_HOME);        /* Go home. */

    /* The bracket under the cursor and its partner, if any. No cursor is
     * shown in the text while the file tree has focus. */
    int br_row = -1, br_col = -1, bm_row = -1, bm_col = -1;
    int br_bg = BG_DEFAULT;
    if (!(E.ft_open && E.ft_focus)) {
        int kind = editorFindBracketPair(&br_row, &br_col, &bm_row, &bm_col);
        if (kind == BRACKET_MATCHED)
            br_bg = BG_BRACKET_MATCH;
        else if (kind == BRACKET_UNMATCHED)
            br_bg = BG_BRACKET_UNMATCHED;
    }

    for (y = 0; y < E.screenrows; y++) {
        int filerow = E.rowoff + y;

        if (E.ft_open) {
            /* Draw the sidebar cell for this row, then move to the text start
             * column so the existing text-drawing code runs unchanged. */
            char pos[32];
            int pl = snprintf(pos, sizeof(pos), "\x1b[%d;1H", y + 1);
            abAppend(&ab, pos, pl);
            ftDrawSidebarRow(&ab, y);
        }

        if (E.ln_width) editorDrawGutter(&ab, filerow);

        if (filerow >= E.numrows) {
            if (!E.ln_width) abAppendLit(&ab, "~");
            abAppendLit(&ab, VT_CLEAR_EOL "\r\n");
            continue;
        }

        r = &E.row[filerow];

        /* Compute selection render-column range for this row. */
        int sel_rcol_start = -1, sel_rcol_end = -1;
        int sel_newline = 0; /* Is this row's newline inside the selection? */
        int sr, sc, er, ec;
        if (editorSelBounds(&sr, &sc, &er, &ec)) {
            if (filerow >= sr && filerow <= er) {
                sel_newline = filerow < er;
                int char_start = (filerow == sr) ? sc : 0;
                int char_end = (filerow == er) ? ec : r->size;
                sel_rcol_start = rowCxToRidx(r, char_start);
                sel_rcol_end = (filerow < er) ? r->rsize : rowCxToRidx(r, char_end);
            }
        }

        /* Spaces that end the line get a red background. */
        int trail = r->size;
        while (trail > 0 && r->chars[trail - 1] == ' ') trail--;
        int trail_rcol = rowCxToRidx(r, trail);

        int current_color = -1;
        int in_selection = 0;
        int cur_bg = BG_DEFAULT; /* Background SGR in effect, a BG_* value. */
        char *c = r->render;
        unsigned char *hl = r->hl;
        int n = r->rsize, k;

        /* The view starts at screen column 'coloff' of the line. A wide
         * character cut by the left edge shows as a blank, and combining
         * marks whose base scrolled away are skipped: the terminal would
         * put them on the gutter. */
        int start = 0, col = 0;
        while (start < n && col < E.coloff) {
            col += textCell(c, start, n, col, &k);
            start += k;
        }
        if (E.coloff)
            while (start < n && textCell(c, start, n, col, &k) == 0) start += k;
        int cells = col > E.coloff ? col - E.coloff : 0; /* Screen cells the text took. */
        if (cells) abAppend(&ab, " ", cells);
        /* It ends with the last character that fits whole. */
        int end = start;
        while (end < n) {
            int w = textCell(c, end, n, col, &k);
            if (cells + w > E.screencols) break;
            cells += w;
            col += w;
            end += k;
        }

        /* Coalesce consecutive characters that share the same style into a
         * single append. 'run_start' marks the first not-yet-flushed byte;
         * we flush the pending slice right before emitting any escape. */
        int run_start = start, j;
        for (j = start; j < end; j++) {
            int rcol = j; /* Offsets in 'render' and 'hl' are the same. */

            /* Handle selection highlight transitions. */
            if (sel_rcol_start >= 0) {
                if (!in_selection && rcol >= sel_rcol_start && rcol < sel_rcol_end) {
                    if (j > run_start) abAppend(&ab, c + run_start, j - run_start);
                    run_start = j;
                    abAppendLit(&ab, VT_REVERSE);
                    in_selection = 1;
                    current_color = -1;
                } else if (in_selection && rcol >= sel_rcol_end) {
                    if (j > run_start) abAppend(&ab, c + run_start, j - run_start);
                    run_start = j;
                    abAppendLit(&ab, VT_REVERSE_OFF);
                    in_selection = 0;
                    current_color = -1;
                }
            }

            /* Background: a selection takes precedence, then the
             * matched or unmatched brackets, then trailing spaces. */
            int want_bg = BG_DEFAULT;
            if (!in_selection) {
                if (filerow == br_row && rcol == br_col)
                    want_bg = br_bg;
                else if (filerow == bm_row && rcol == bm_col)
                    want_bg = br_bg;
                else if (rcol >= trail_rcol)
                    want_bg = BG_TRAILING_SPACE;
            }
            if (want_bg != cur_bg) {
                if (j > run_start) abAppend(&ab, c + run_start, j - run_start);
                run_start = j;
                char sgr[8];
                int slen = snprintf(sgr, sizeof(sgr), "\x1b[%dm", want_bg);
                abAppend(&ab, sgr, slen);
                cur_bg = want_bg;
            }

            /* Past ASCII, a whole UTF-8 sequence is one step, so no escape
             * ever lands inside one. A byte that does not start a valid
             * sequence is drawn like a control character. */
            unsigned char b = (unsigned char)c[j];
            int ctrl = 0, clen = 1;
            if (b < 0x20 || b >= 0x7f) {
                ctrl = editorCtrlLen(c, j, n);
                if (!ctrl && b >= 0x80) {
                    uint32_t cp;
                    clen = utf8Decode(c, j, n, &cp);
                    if (clen == 0) ctrl = clen = 1;
                }
            }
            if (ctrl) {
                /* A control character is never sent as is, whatever the
                 * lexer made of it: draw it reversed in caret notation,
                 * ^A as 'A' and ESC as '[', and DEL, a C1 control or a
                 * stray byte as '?', one cell per byte. */
                char sym[2] = {'?', '?'};
                if (b < 0x20) sym[0] = '@' + b;
                if (j > run_start) abAppend(&ab, c + run_start, j - run_start);
                if (!in_selection) abAppendLit(&ab, VT_REVERSE);
                abAppend(&ab, sym, ctrl);
                abAppendLit(&ab, VT_RESET);
                current_color = -1;
                cur_bg = BG_DEFAULT; /* VT_RESET cleared it too. */
                if (in_selection) abAppendLit(&ab, VT_REVERSE);
                j += ctrl - 1;
                run_start = j + 1; /* 'sym' replaces the bytes. */
                continue;
            } else if (hl[j] == HL_NORMAL) {
                if (current_color != -1) {
                    if (j > run_start) abAppend(&ab, c + run_start, j - run_start);
                    run_start = j;
                    abAppendLit(&ab, VT_FG_DEFAULT);
                    current_color = -1;
                }
                /* Character joins the pending run. */
            } else {
                int color = editorSyntaxToColor(hl[j]);
                if (color != current_color) {
                    if (j > run_start) abAppend(&ab, c + run_start, j - run_start);
                    run_start = j;
                    char buf[16];
                    int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", color);
                    current_color = color;
                    abAppend(&ab, buf, clen);
                }
                /* Character joins the pending run. */
            }
            j += clen - 1;
        }
        /* Flush the trailing run. */
        if (j > run_start) abAppend(&ab, c + run_start, j - run_start);
        abAppendLit(&ab, VT_FG_DEFAULT);
        /* Keep a colored background out of the EOL erase. */
        if (cur_bg != BG_DEFAULT) abAppendLit(&ab, VT_BG_DEFAULT);
        /* A selected newline extends the highlight to the edge of the screen
         * (erase to EOL with a reverse-video bg); otherwise it stops at the
         * last selected character. */
        if (sel_newline && !in_selection) abAppendLit(&ab, VT_REVERSE);
        if (!sel_newline && in_selection) abAppendLit(&ab, VT_REVERSE_OFF);
        /* A line that fills the text area leaves the terminal at its last
         * column, where an erase would wipe the character just drawn. */
        if (cells < E.screencols) abAppendLit(&ab, VT_CLEAR_EOL);
        if (sel_newline) abAppendLit(&ab, VT_REVERSE_OFF);
        abAppendLit(&ab, "\r\n");
    }

    /* Create a two rows status, spanning the gutter and text. First row: */
    int statuscols = E.ln_width + E.screencols;
    if (E.ft_open) ftBlankCell(&ab); /* continue the divider under the panes */
    abAppendLit(&ab, VT_CLEAR_EOL);
    abAppendLit(&ab, VT_REVERSE);
    /* The file name takes 20 columns at most. It may hold anything. */
    char name[64], status[160], rstatus[80];
    int namelen = snprintf(name, sizeof(name), "%s", E.filename);
    if (namelen >= (int)sizeof(name)) namelen = sizeof(name) - 1;
    editorSanitize(name, namelen);
    int cols;
    namelen = textFit(name, namelen, 20, &cols);
    int len = snprintf(status, sizeof(status), "%.*s - %d line%s %s", namelen, name, E.numrows,
                       E.numrows == 1 ? "" : "s", E.dirty ? "(modified)" : "");
    /* The cursor as line:column, both from 1, as compilers print them. */
    long selchars = editorSelCharCount();
    int rlen =
        selchars ? snprintf(rstatus, sizeof(rstatus), "%ld selected  %d:%d", selchars, E.cy + 1,
                            editorCursorCharCol() + 1)
                 : snprintf(rstatus, sizeof(rstatus), "%d:%d", E.cy + 1, editorCursorCharCol() + 1);
    len = textFit(status, len, statuscols, &cols);
    abAppend(&ab, status, len);
    /* Columns, not bytes, decide where the position goes. */
    while (cols < statuscols) {
        if (statuscols - cols == rlen) {
            abAppend(&ab, rstatus, rlen);
            break;
        } else {
            abAppendLit(&ab, " ");
            cols++;
        }
    }
    abAppendLit(&ab, VT_RESET "\r\n");

    /* Second row depends on E.statusmsg and the status message update time. */
    if (E.ft_open) ftBlankCell(&ab);
    abAppendLit(&ab, VT_CLEAR_EOL);
    int msglen = strlen(E.statusmsg);
    if (msglen && time(NULL) - E.statusmsg_time < 5)
        abAppend(&ab, E.statusmsg, textFit(E.statusmsg, msglen, statuscols, &cols));

    /* Put the cursor at its current position. */
    if (E.ft_open && E.ft_focus) {
        /* Park the hardware cursor on the selected sidebar row. */
        int srow = E.ft_sel - E.ft_scroll;
        if (srow < 0) srow = 0;
        if (srow >= E.screenrows) srow = E.screenrows - 1;
        snprintf(buf, sizeof(buf), "\x1b[%d;1H", srow + 1);
        abAppend(&ab, buf, strlen(buf));
    } else {
        /* The cursor goes at its screen column, counted from the first one
         * on screen, past the sidebar and gutter. */
        int rx = E.cy < E.numrows ? rowCxToRx(&E.row[E.cy], E.cx) : E.cx;
        int tx = rx - E.coloff;
        if (tx < 0) tx = 0;
        snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy - E.rowoff + 1,
                 1 + E.ft_width + E.ln_width + tx);
        abAppend(&ab, buf, strlen(buf));
    }
    abAppendLit(&ab, VT_CURSOR_SHOW); /* Show cursor. */
    writeAll(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

/* Set an editor status message for the second line of the status, at the
 * end of the screen. */
/* Clear the message line. */
void editorClearStatusMessage(void) {
    E.statusmsg[0] = '\0';
    E.statusmsg_time = time(NULL);
}

void editorSetStatusMessage(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    /* Messages quote paths and file names, which may hold anything. */
    editorSanitize(E.statusmsg, (int)strlen(E.statusmsg));
    E.statusmsg_time = time(NULL);
}
