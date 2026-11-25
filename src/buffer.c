/* Rows of the document: editing primitives, loading and saving. */
#include "quip.h"

/* Ensure 'buf' can hold at least 'need' bytes, growing its capacity
 * geometrically. '*cap' tracks the current capacity and is updated in place.
 * Never shrinks; returns the (possibly reallocated) buffer. Never returns NULL
 * for a need of 0 either, so callers may memset or memcpy into it. */
void *growBuf(void *buf, int *cap, int need) {
    if (buf && need <= *cap) return buf;
    int nc = *cap ? *cap : 8;
    while (nc < need) nc *= 2;
    *cap = nc;
    return realloc(buf, nc);
}

/* Update the rendered version and the syntax highlight of a row. The
 * rendered version is the text with each TAB turned into the spaces up to
 * the next tab stop, so that 'render' and 'hl' line up byte for byte. */
void editorUpdateRow(erow *row) {
    int tabs = 0, j, idx;

    for (j = 0; j < row->size; j++)
        if (row->chars[j] == TAB) tabs++;

    unsigned long long allocsize = (unsigned long long)row->size + tabs * QUIP_TAB_STOP + 1;
    if (allocsize > UINT32_MAX) {
        editorDie("Some line of the edited file is too long for quip", 0);
    }

    row->render = growBuf(row->render, &row->render_cap, row->size + tabs * QUIP_TAB_STOP + 1);
    if (tabs == 0) {
        memcpy(row->render, row->chars, row->size);
        idx = row->size;
    } else {
        /* Tab stops count screen columns, which wide characters make
         * differ from bytes. */
        int col = 0, k;
        idx = 0;
        for (j = 0; j < row->size; j += k) {
            int w = textCell(row->chars, j, row->size, col, &k);
            if (row->chars[j] == TAB) {
                memset(row->render + idx, ' ', w);
                idx += w;
            } else {
                memcpy(row->render + idx, row->chars + j, k);
                idx += k;
            }
            col += w;
        }
    }
    row->rsize = idx;
    row->render[idx] = '\0';

    /* Update the syntax highlighting attributes of the row. */
    editorUpdateSyntax(row);
}

/* Positions in a row come in three kinds: a byte offset in 'chars' (cx),
 * a byte offset in 'render' (ridx), and a screen column counted from the
 * start of the line (rx). They differ by TABs, by UTF-8 sequences, which
 * are several bytes, and by wide characters, which take two columns. */

/* Screen column where byte 'cx' of 'row' is drawn. Past the end of the line
 * (a parked cursor) each byte counts as one more column. */
int rowCxToRx(const erow *row, int cx) {
    int rx = 0, i = 0, k;
    while (i < cx && i < row->size) {
        rx += textCell(row->chars, i, row->size, rx, &k);
        i += k;
    }
    if (cx > i) rx += cx - i;
    return rx;
}

/* Byte offset of the character drawn at screen column 'rx': the one that
 * covers it, or the end of the line when rx is past it. Zero width
 * characters go with the character before them. */
int rowRxToCx(const erow *row, int rx) {
    int col = 0, i = 0, k;
    while (i < row->size) {
        int w = textCell(row->chars, i, row->size, col, &k);
        if (w && col + w > rx) break;
        col += w;
        i += k;
    }
    return i;
}

/* Offset in 'render' of byte 'cx' of 'row'. */
int rowCxToRidx(const erow *row, int cx) {
    int rx = 0, ridx = 0, i = 0, k;
    while (i < cx && i < row->size) {
        int w = textCell(row->chars, i, row->size, rx, &k);
        ridx += row->chars[i] == TAB ? w : k;
        rx += w;
        i += k;
    }
    return ridx;
}

/* Inverse of rowCxToRidx: the byte in 'chars' rendered at 'ridx'. An
 * offset inside the spaces of a TAB lands just after it. */
int rowRidxToCx(const erow *row, int ridx) {
    int rx = 0, r = 0, i = 0, k;
    while (i < row->size && r < ridx) {
        int w = textCell(row->chars, i, row->size, rx, &k);
        r += row->chars[i] == TAB ? w : k;
        rx += w;
        i += k;
    }
    return i;
}

/* Cells the character at byte 'i' of 'row' takes; 0 for a combining mark. */
static int rowCellsAt(const erow *row, int i) {
    int k;
    return textCell(row->chars, i, row->size, 0, &k);
}

/* Start of the character that ends at byte 'cx': a whole UTF-8 sequence,
 * or a single byte when the bytes before cx are not a valid one. */
static int rowCharBefore(const erow *row, int cx) {
    int i = cx - 1, lim = cx > 4 ? cx - 4 : 0;
    while (i > lim && ((unsigned char)row->chars[i] & 0xc0) == 0x80) i--;
    uint32_t cp;
    int k = utf8Decode(row->chars, i, row->size, &cp);
    return (k && i + k == cx) ? i : cx - 1;
}

/* Where the cursor goes from 'cx' when it moves one character right or
 * left: over a whole UTF-8 sequence, and over any combining marks that
 * follow it, so that it never lands inside what shows as one character. */
int rowNextCx(const erow *row, int cx) {
    if (cx >= row->size) return cx;
    int k;
    textCell(row->chars, cx, row->size, 0, &k);
    cx += k;
    while (cx < row->size && rowCellsAt(row, cx) == 0) {
        textCell(row->chars, cx, row->size, 0, &k);
        cx += k;
    }
    return cx;
}

int rowPrevCx(const erow *row, int cx) {
    if (cx > row->size) return row->size;
    if (cx <= 0) return 0;
    int i = rowCharBefore(row, cx);
    while (i > 0 && rowCellsAt(row, i) == 0) i = rowCharBefore(row, i);
    return i;
}
/* Insert a row at the specified position, shifting the other rows on the bottom
 * if required. */
static void editorInsertRow(int at, const char *s, size_t len) {
    if (at > E.numrows) return;
    if (len > INT_MAX / 2) editorDie("Some line of the edited file is too long for quip", 0);
    if (E.numrows + 1 > E.rowcap) {
        E.rowcap = E.rowcap ? E.rowcap * 2 : 16;
        E.row = realloc(E.row, sizeof(erow) * E.rowcap);
    }
    if (at != E.numrows) {
        memmove(E.row + at + 1, E.row + at, sizeof(E.row[0]) * (E.numrows - at));
        for (int j = at + 1; j <= E.numrows; j++) E.row[j].idx++;
    }
    E.row[at].size = len;
    E.row[at].chars_cap = len + 1;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';
    E.row[at].hl = NULL;
    E.row[at].hl_cap = 0;
    E.row[at].hl_state = HL_ST_INVALID; /* So the rows below get re-lexed. */
    E.row[at].render = NULL;
    E.row[at].render_cap = 0;
    E.row[at].rsize = 0;
    E.row[at].idx = at;
    E.numrows++; /* First, so highlight changes can reach the last row. */
    editorUpdateRow(E.row + at);
    E.dirty++;
}

/* Free row's heap allocated stuff. */
static void editorFreeRow(erow *row) {
    free(row->render);
    free(row->chars);
    free(row->hl);
}

/* Remove the row at the specified position, shifting the remainign on the
 * top. */
void editorDelRow(int at) {
    erow *row;

    if (at >= E.numrows) return;
    row = E.row + at;
    editorFreeRow(row);
    memmove(E.row + at, E.row + at + 1, sizeof(E.row[0]) * (E.numrows - at - 1));
    for (int j = at; j < E.numrows - 1; j++) E.row[j].idx--;
    E.numrows--;
    E.dirty++;
    /* The row that moved up now follows a different row: re-lex it. */
    if (at < E.numrows) editorUpdateSyntax(E.row + at);
}

/* Turn the editor rows into a single heap-allocated string, each row ending
 * in '\n', and store its length, not counting the final NUL, in *buflen.
 * The document may be past 2 GB, so lengths are size_t. */
char *editorRowsToString(size_t *buflen) {
    char *buf = NULL, *p;
    size_t totlen = 0;
    int j;

    /* Compute count of bytes */
    for (j = 0; j < E.numrows; j++)
        totlen += E.row[j].size + 1; /* +1 is for "\n" at end of every row */
    *buflen = totlen;
    totlen++; /* Also make space for nulterm */

    p = buf = malloc(totlen);
    for (j = 0; j < E.numrows; j++) {
        memcpy(p, E.row[j].chars, E.row[j].size);
        p += E.row[j].size;
        *p = '\n';
        p++;
    }
    *p = '\0';
    return buf;
}

/* Append the string 's' at the end of a row */
void editorRowAppendString(erow *row, char *s, size_t len) {
    row->chars = growBuf(row->chars, &row->chars_cap, row->size + len + 1);
    memcpy(row->chars + row->size, s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    E.dirty++;
}

/* Remove the 'len' bytes at 'at' from 'row'. The caller updates the row. */
void editorRowDelRange(erow *row, int at, int len) {
    memmove(row->chars + at, row->chars + at + len, row->size - at - len + 1);
    row->size -= len;
}

/* Insert the 'len' bytes of 's' at 'at' in 'row'. The caller updates it. */
void editorRowInsRange(erow *row, int at, const char *s, int len) {
    row->chars = growBuf(row->chars, &row->chars_cap, row->size + len + 1);
    memmove(row->chars + at + len, row->chars + at, row->size - at + 1);
    memcpy(row->chars + at, s, len);
    row->size += len;
}

/* Insert 'len' bytes at the cursor and move the cursor past them. A cursor
 * on the line after the last one first gets that line created, and one past
 * the end of its line gets spaces up to it. */
void editorInsertBytes(const char *s, int len) {
    while (E.numrows <= E.cy) editorInsertRow(E.numrows, "", 0);
    erow *row = &E.row[E.cy];
    while (row->size < E.cx) editorRowInsRange(row, row->size, " ", 1);
    editorRowInsRange(row, E.cx, s, len);
    editorUpdateRow(row);
    E.cx += len;
    E.dirty++;
}

/* Insert the byte 'c' at the cursor. */
void editorInsertChar(int c) {
    char b = (char)c;
    editorInsertBytes(&b, 1);
}

/* Inserting a newline is slightly complex as we have to handle inserting a
 * newline in the middle of a line, splitting the line as needed. */
void editorInsertNewline(void) {
    if (E.cy >= E.numrows) {
        if (E.cy > E.numrows) return;
        editorInsertRow(E.cy, "", 0);
    } else {
        erow *row = &E.row[E.cy];
        /* Past the end of the line counts as just after its last char. */
        int col = E.cx < row->size ? E.cx : row->size;
        if (col == 0) {
            editorInsertRow(E.cy, "", 0);
        } else {
            /* We are in the middle of a line. Split it between two rows. */
            editorInsertRow(E.cy + 1, row->chars + col, row->size - col);
            row = &E.row[E.cy];
            row->chars[col] = '\0';
            row->size = col;
            editorUpdateRow(row);
        }
    }
    E.cy++;
    E.cx = 0;
}

/* Delete the character before the cursor, joining the line to the one above
 * when the cursor is at its start. A UTF-8 sequence goes as a whole. */
void editorDelChar(void) {
    if (E.cy >= E.numrows || (E.cx == 0 && E.cy == 0)) return;
    erow *row = &E.row[E.cy];
    if (E.cx == 0) {
        int col = E.row[E.cy - 1].size;
        editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
        editorDelRow(E.cy);
        E.cy--;
        E.cx = col;
    } else {
        if (E.cx > row->size) E.cx = row->size;
        int at = rowCharBefore(row, E.cx);
        editorRowDelRange(row, at, E.cx - at);
        editorUpdateRow(row);
        E.cx = at;
    }
    E.dirty++;
}
/* Load the specified program in the editor memory and returns 0 on success
 * or 1 on error. */
int editorOpen(char *filename) {
    E.dirty = 0;
    E.crlf = 0;
    E.noeol = 0;
    free(E.filename);
    size_t fnlen = strlen(filename) + 1;
    E.filename = malloc(fnlen);
    memcpy(E.filename, filename, fnlen);

    int fd = open(filename, O_RDONLY);
    if (fd == -1) {
        if (errno != ENOENT) editorDie("Opening file", errno);
        return 1;
    }
    /* The whole file at once: the line endings are known before any line
     * is made into a row. */
    struct stat st;
    size_t cap = (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) ? (size_t)st.st_size + 1 : 65536;
    size_t len = 0;
    char *buf = malloc(cap);
    for (;;) {
        if (len == cap) buf = realloc(buf, cap *= 2);
        /* At most 1 GB a call: macOS refuses reads over INT_MAX bytes. */
        size_t want = cap - len < (1u << 30) ? cap - len : (1u << 30);
        ssize_t n = read(fd, buf + len, want);
        if (n == 0) break;
        if (n == -1) {
            if (errno == EINTR) continue;
            editorDie("Reading file", errno);
        }
        len += (size_t)n;
    }
    close(fd);

    /* CRLF when most lines end that way. A file saves with the endings it
     * was opened with, and without a final newline if it had none. */
    size_t lf = 0, crlf = 0;
    char *p = buf, *end = buf + len;
    while ((p = memchr(p, '\n', end - p))) {
        lf++;
        if (p > buf && p[-1] == '\r') crlf++;
        p++;
    }
    E.crlf = crlf > lf - crlf;
    E.noeol = len > 0 && buf[len - 1] != '\n';

    for (p = buf; p < end;) {
        char *nl = memchr(p, '\n', end - p);
        size_t linelen = nl ? (size_t)(nl - p) : (size_t)(end - p);
        if (E.crlf && nl && linelen && p[linelen - 1] == '\r') linelen--;
        editorInsertRow(E.numrows, p, linelen);
        if (!nl) break;
        p = nl + 1;
    }
    free(buf);
    E.dirty = 0;
    return 0;
}

/* The text as it goes to disk: like editorRowsToString, but each line ends
 * the way the file's lines did, and the last one with nothing when the file
 * had no newline at its end. */
static char *editorRowsToFile(size_t *buflen) {
    if (!E.crlf && !E.noeol) return editorRowsToString(buflen);
    size_t total = 1;
    for (int j = 0; j < E.numrows; j++) total += E.row[j].size + 2;
    char *buf = malloc(total), *p = buf;
    for (int j = 0; j < E.numrows; j++) {
        memcpy(p, E.row[j].chars, E.row[j].size);
        p += E.row[j].size;
        if (j == E.numrows - 1 && E.noeol) break;
        if (E.crlf) *p++ = '\r';
        *p++ = '\n';
    }
    *p = '\0';
    *buflen = (size_t)(p - buf);
    return buf;
}

/* The file a save writes: 'path', or what it links to, so that the link
 * stays a link. A link to a file not created yet names it as well. Stores
 * the result in 'out' (PATH_MAX bytes). */
static void editorSaveTarget(const char *path, char *out) {
    if (realpath(path, out)) return;
    char link[PATH_MAX];
    ssize_t n = readlink(path, link, sizeof(link) - 1);
    const char *slash = strrchr(path, '/');
    size_t dirlen = (n > 0 && link[0] != '/' && slash) ? (size_t)(slash - path + 1) : 0;
    if (n <= 0 || dirlen + (size_t)n >= PATH_MAX) {
        snprintf(out, PATH_MAX, "%s", path);
        return;
    }
    /* A relative link is relative to the directory the link is in. */
    memcpy(out, path, dirlen);
    memcpy(out + dirlen, link, (size_t)n);
    out[dirlen + n] = '\0';
}

/* Rewrite the file at 'path' where it is. Writing first and cutting to size
 * after means a failure never leaves less of the file than before, but it
 * may leave a mix of old and new text. Returns 0 or an errno value. */
static int editorWriteInPlace(const char *path, const char *buf, size_t len, int regular) {
    int fd = open(path, O_WRONLY | O_CREAT, 0644);
    if (fd == -1) return errno;
    if (writeAll(fd, buf, len) == -1 ||
        (regular && (ftruncate(fd, len) == -1 || fsync(fd) == -1))) {
        int err = errno;
        close(fd);
        return err;
    }
    return close(fd) == -1 ? errno : 0;
}

/* Write the text to a new file next to the old one, then rename it over the
 * old one. The rename is atomic, so whatever happens on the way (a full
 * disk, a crash, a kill), the file holds either all of the old text or all
 * of the new. The new file gets the old one's mode and owner. When that
 * cannot work, the file is rewritten in place and *inplace set: a directory
 * quip may not create files in, a file with other hard links, which would
 * keep the old text, an owner quip cannot give the new file, or something
 * other than a regular file. Returns 0 or an errno value. */
static int editorWriteFile(const char *path, const char *buf, size_t len, int *inplace) {
    char target[PATH_MAX];
    editorSaveTarget(path, target);
    struct stat st;
    int exists = stat(target, &st) == 0;
    *inplace = 0;
    if (exists && S_ISDIR(st.st_mode)) return EISDIR;
    if (exists && (!S_ISREG(st.st_mode) || st.st_nlink > 1)) goto in_place;

    /* ".name.XXXXXX" in the same directory: rename only works within one
     * file system, and the dot keeps it out of listings meanwhile. */
    char tmp[PATH_MAX + 16];
    const char *slash = strrchr(target, '/');
    const char *base = slash ? slash + 1 : target;
    int dirlen = slash ? (int)(slash - target + 1) : 0;
    snprintf(tmp, sizeof(tmp), "%.*s.%s.XXXXXX", dirlen, target, base);
    int fd = mkstemp(tmp);
    if (fd == -1) goto in_place;

    mode_t mode;
    if (exists) {
        mode = st.st_mode & 07777;
    } else {
        mode_t mask = umask(0);
        umask(mask);
        mode = 0644 & ~mask;
    }
    struct stat tst;
    if (exists && fstat(fd, &tst) == 0 && (tst.st_uid != st.st_uid || tst.st_gid != st.st_gid) &&
        fchown(fd, st.st_uid, st.st_gid) == -1) {
        close(fd);
        unlink(tmp);
        goto in_place;
    }
    if (fchmod(fd, mode) == -1 || writeAll(fd, buf, len) == -1 || fsync(fd) == -1) {
        int err = errno;
        close(fd);
        unlink(tmp);
        return err;
    }
    if (close(fd) == -1 || rename(tmp, target) == -1) {
        int err = errno;
        unlink(tmp);
        return err;
    }
    /* Make the rename itself durable. */
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%.*s", dirlen ? dirlen : 1, dirlen ? target : ".");
    int dfd = open(dir, O_RDONLY);
    if (dfd != -1) {
        fsync(dfd);
        close(dfd);
    }
    return 0;

in_place:
    *inplace = 1;
    return editorWriteInPlace(target, buf, len, !exists || S_ISREG(st.st_mode));
}

/* Save the current file on disk. Return 0 on success, 1 on error. */
int editorSave(void) {
    size_t len;
    int inplace;
    char *buf = editorRowsToFile(&len);
    int err = editorWriteFile(E.filename, buf, len, &inplace);
    free(buf);
    if (err) {
        editorSetStatusMessage("Can't save! I/O error: %s", strerror(err));
        return 1;
    }
    E.dirty = 0;
    editorSetStatusMessage("%zu bytes written %s", len, inplace ? "in place" : "on disk");
    return 0;
}

/* Free every row buffer and reset the row array to empty. */
void editorFreeAllRows(void) {
    for (int i = 0; i < E.numrows; i++) {
        free(E.row[i].render);
        free(E.row[i].chars);
        free(E.row[i].hl);
    }
    free(E.row);
    E.row = NULL;
    E.numrows = 0;
    E.rowcap = 0;
}

/* Tear down the current rows and rebuild them from serialized 'text' (rows
 * separated by '\n'). Cursor/dirty state is set by the caller. */
void editorRebuildRows(const char *text, size_t len) {
    editorFreeAllRows();

    const char *p = text;
    size_t rem = len;
    while (rem > 0) {
        const char *nl = memchr(p, '\n', rem);
        size_t linelen = nl ? (size_t)(nl - p) : rem;
        editorInsertRow(E.numrows, p, linelen);
        if (!nl) break;
        rem -= linelen + 1;
        p += linelen + 1;
    }
}
