/* Drawing the screen: text, gutter, sidebar and status bar. */
#include "quip.h"

/* Bytes that a terminal would obey instead of print: a C0 control or DEL
 * (1 byte), or a C1 control U+0080-U+009F written in UTF-8 (2 bytes).
 * Returns how many bytes at s[i] form one, or 0. Text from files, file names
 * and messages must never reach the terminal with these intact. */
static int editorCtrlLen(const char *s, int i, int n) {
    unsigned char b = (unsigned char)s[i];
    if (b < 0x20 || b == 0x7f) return 1;
    if (b == 0xc2 && i + 1 < n && (unsigned char)s[i + 1] >= 0x80 &&
        (unsigned char)s[i + 1] <= 0x9f)
        return 2;
    return 0;
}

/* Replace control characters in 's' with '?', one per byte. */
void editorSanitize(char *s, int n) {
    int i = 0;
    while (i < n) {
        int k = editorCtrlLen(s, i, n);
        if (k == 0) {
            i++;
            continue;
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

int editorCharToRenderCol(erow *row, int char_col) {
    int rcol = 0;
    for (int i = 0; i < char_col && i < row->size; i++) {
        if (row->chars[i] == TAB)
            rcol += QUIP_TAB_STOP - (rcol % QUIP_TAB_STOP);
        else
            rcol++;
    }
    return rcol;
}

/* Inverse of editorCharToRenderCol: the char column whose rendering covers
 * render column 'rcol' (a click inside a TAB lands just after it). */
int editorRenderToCharCol(erow *row, int rcol) {
    int fc = 0, rc = 0;
    while (fc < row->size && rc < rcol) {
        if (row->chars[fc] == TAB)
            rc += QUIP_TAB_STOP - (rc % QUIP_TAB_STOP);
        else
            rc++;
        fc++;
    }
    return fc;
}

/* Character column of the cursor: UTF-8 continuation bytes before it are not
 * counted, so a multibyte character advances the column by one. */
static int editorCursorCharCol(void) {
    int filerow = E.rowoff + E.cy, col = E.coloff + E.cx;
    if (filerow >= E.numrows) return col;
    erow *row = &E.row[filerow];
    int bytes = col < row->size ? col : row->size, n = col - bytes;
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
    int y = E.rowoff + E.cy, col = E.coloff + E.cx;
    if (y >= E.numrows) return BRACKET_NONE;
    erow *row = &E.row[y];
    if (col >= row->size) return BRACKET_NONE;

    char c = row->chars[col];
    const char *o = c ? strchr(open, c) : NULL;
    const char *cl = c ? strchr(close, c) : NULL;
    if (!o && !cl) return BRACKET_NONE;
    int x = editorCharToRenderCol(row, col);
    if (!editorIsCodeAt(row, x)) return BRACKET_NONE;

    char want = o ? close[o - open] : open[cl - close];
    int dir = o ? 1 : -1, depth = 0;
    *brow = y;
    *bcol = x;
    /* The rows in locals, so the loop never goes back to the global E. */
    const erow *rows = E.row, *r = row;
    const int numrows = E.numrows;
    for (;;) {
        x += dir;
        while (x < 0 || x >= r->rsize) {
            y += dir;
            if (y < 0 || y >= numrows) return BRACKET_UNMATCHED;
            r = &rows[y];
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
    /* Keep the cursor inside the (possibly narrower) text area. */
    if (E.cx >= E.screencols) {
        E.coloff += E.cx - (E.screencols - 1);
        E.cx = E.screencols - 1;
    }
    if (E.cx < 0) E.cx = 0;
    if (E.coloff < 0) E.coloff = 0;
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
    int current = (filerow == E.rowoff + E.cy);
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
                sel_rcol_start = editorCharToRenderCol(r, char_start);
                sel_rcol_end = (filerow < er) ? r->rsize : editorCharToRenderCol(r, char_end);
            }
        }

        /* Spaces that end the line get a red background. */
        int trail = r->size;
        while (trail > 0 && r->chars[trail - 1] == ' ') trail--;
        int trail_rcol = editorCharToRenderCol(r, trail);

        int len = r->rsize - E.coloff;
        int current_color = -1;
        int in_selection = 0;
        int cur_bg = BG_DEFAULT; /* Background SGR in effect, a BG_* value. */
        int cells = 0;           /* Screen cells the text took. */
        if (len > 0) {
            if (len > E.screencols) len = E.screencols;
            char *c = r->render + E.coloff;
            unsigned char *hl = r->hl + E.coloff;
            int j;
            /* Coalesce consecutive characters that share the same style into a
             * single append. 'run_start' marks the first not-yet-flushed char;
             * we flush the pending slice right before emitting any escape. */
            int run_start = 0;
            for (j = 0; j < len; j++) {
                int rcol = E.coloff + j;

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

                int ctrl = editorCtrlLen(c, j, len);
                if (ctrl) {
                    /* A control character is never sent as is, whatever the
                     * lexer made of it: draw it reversed in caret notation,
                     * ^A as 'A' and ESC as '[', and DEL or a C1 control as
                     * '?', one cell per byte. */
                    unsigned char b = (unsigned char)c[j];
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
            }
            /* Flush the trailing run. */
            if (j > run_start) abAppend(&ab, c + run_start, j - run_start);
            /* One cell per byte, except that UTF-8 continuation bytes join
             * the character before them; a control is a cell per byte. */
            for (j = 0; j < len; j++) {
                int ctrl = editorCtrlLen(c, j, len);
                if (ctrl) {
                    cells += ctrl;
                    j += ctrl - 1;
                } else if (((unsigned char)c[j] & 0xc0) != 0x80)
                    cells++;
            }
        }
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
    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d line%s %s", E.filename, E.numrows,
                       E.numrows == 1 ? "" : "s", E.dirty ? "(modified)" : "");
    /* The cursor as line:column, both from 1, as compilers print them. */
    long selchars = editorSelCharCount();
    int rlen = selchars ? snprintf(rstatus, sizeof(rstatus), "%ld selected  %d:%d", selchars,
                                   E.rowoff + E.cy + 1, editorCursorCharCol() + 1)
                        : snprintf(rstatus, sizeof(rstatus), "%d:%d", E.rowoff + E.cy + 1,
                                   editorCursorCharCol() + 1);
    if (len > statuscols) len = statuscols;
    editorSanitize(status, len); /* The file name may hold anything. */
    abAppend(&ab, status, len);
    while (len < statuscols) {
        if (statuscols - len == rlen) {
            abAppend(&ab, rstatus, rlen);
            break;
        } else {
            abAppendLit(&ab, " ");
            len++;
        }
    }
    abAppendLit(&ab, VT_RESET "\r\n");

    /* Second row depends on E.statusmsg and the status message update time. */
    if (E.ft_open) ftBlankCell(&ab);
    abAppendLit(&ab, VT_CLEAR_EOL);
    int msglen = strlen(E.statusmsg);
    if (msglen && time(NULL) - E.statusmsg_time < 5)
        abAppend(&ab, E.statusmsg, msglen <= statuscols ? msglen : statuscols);

    /* Put cursor at its current position. Note that the horizontal position
     * at which the cursor is displayed may be different compared to 'E.cx'
     * because of TABs. */
    if (E.ft_open && E.ft_focus) {
        /* Park the hardware cursor on the selected sidebar row. */
        int srow = E.ft_sel - E.ft_scroll;
        if (srow < 0) srow = 0;
        if (srow >= E.screenrows) srow = E.screenrows - 1;
        snprintf(buf, sizeof(buf), "\x1b[%d;1H", srow + 1);
        abAppend(&ab, buf, strlen(buf));
    } else {
        /* The text is drawn from render column 'coloff', so the cursor goes
         * at its char column's render column, past the sidebar and gutter. */
        int filerow = E.rowoff + E.cy;
        erow *row = (filerow >= E.numrows) ? NULL : &E.row[filerow];
        int col = E.coloff + E.cx;
        int tx = E.cx;
        if (row) {
            tx = editorCharToRenderCol(row, col) - E.coloff;
            if (col > row->size) tx += col - row->size; /* past end of line */
            if (tx < 0) tx = 0;
        }
        snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1, 1 + E.ft_width + E.ln_width + tx);
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
