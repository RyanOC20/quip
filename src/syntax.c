/* The syntax highlighting lexer and its colors. */
#include "quip.h"

int is_separator(int c) { return c == '\0' || isspace(c) || strchr(",.()+-/*=~%[];", c) != NULL; }

/* True if delimiter 'd' (never, if the language has none) starts at
 * offset i of the n-byte row text p. */
int hlAt(const char *p, int i, int n, const hldelim *d) {
    return d->len && i + d->len <= n && !memcmp(p + i, d->s, d->len);
}

/* True for chars that a keyword cannot touch: identifier chars ('$' for
 * JavaScript and shell names, UTF-8 bytes) and '#', so the "if" in C's "#if"
 * stays uncolored like the rest of the preprocessor directives. */
static int hlIsWordChar(int c) {
    return isalnum(c) || c == '_' || c == '$' || c == '#' || c >= 0x80;
}

/* True for chars that end a shell word, so a # after them starts a comment:
 * ${#arr}, x#y and a=#x are not comments, but cmd;# and (# are. */
static int hlIsShellWordBreak(int c) { return isspace(c) || (c && strchr(";&|()<>", c)); }

/* True for chars that continue an identifier, OCaml primes included. */
static int hlIsIdentChar(int c) { return isalnum(c) || c == '_' || c == '\''; }

/* Length of the OCaml quoted string id ({id|...|id}) at p: lowercase
 * letters and underscores, possibly none. */
static int hlQuotedIdLen(const char *p, int n) {
    int j = 0;
    while (j < n && (islower((unsigned char)p[j]) || p[j] == '_')) j++;
    return j;
}

/* Hash of a quoted string id, small enough to fit a lexer state argument. */
static int hlQuotedIdHash(const char *s, int len) {
    unsigned int h = 2166136261u;
    for (int k = 0; k < len; k++) h = (h ^ (unsigned char)s[k]) * 16777619u;
    return (int)(h & 0x7fffff);
}

/* Length of the OCaml char literal starting at the quote at p ('a', '\n',
 * '\'', '\065', '\xff', '\o377'), or 0 if none fits there. */
static int hlCharLiteralLen(const char *p, int n) {
    if (n >= 3 && p[1] != '\\' && p[2] == '\'') return 3;
    if (n >= 4 && p[1] == '\\')
        for (int j = 3; j < n && j <= 6; j++)
            if (p[j] == '\'') return j + 1;
    return 0;
}

/* Set row->hl for every rendered char of the row, starting from lexer
 * 'state' (the state the previous row ended in). Returns the state this
 * row ends in. */
static int editorHighlightRow(erow *row, int state) {
    char **keywords = E.syntax->keywords;
    const hldelim *scs = &E.syntax->line_comment;
    const hldelim *mcs = &E.syntax->block_start;
    const hldelim *mce = &E.syntax->block_end;
    int flags = E.syntax->flags;
    char *p = row->render;
    unsigned char *hl = row->hl;
    int n = row->rsize;
    int kind = state & 0xff, arg = state >> 8;
    int i = 0;
    int escaped_eol = 0; /* Row ends in a backslash inside a string. */

    while (i < n) {
        int c = (unsigned char)p[i];

        /* Inside a block comment, possibly opened on an earlier row. */
        if (kind == HL_ST_COMMENT) {
            if (hlAt(p, i, n, mce)) {
                memset(hl + i, HL_MLCOMMENT, mce->len);
                i += mce->len;
                if (--arg == 0) kind = HL_ST_NORMAL;
            } else if ((flags & HL_NESTED_COMMENTS) && hlAt(p, i, n, mcs)) {
                memset(hl + i, HL_MLCOMMENT, mcs->len);
                i += mcs->len;
                if (arg < 0x7fffff) arg++;
            } else {
                hl[i++] = HL_MLCOMMENT;
            }
            continue;
        }

        /* Inside a "" or '' string. */
        if (kind == HL_ST_STRING) {
            hl[i] = HL_STRING;
            if (c == '\\') {
                if (i + 1 < n) {
                    hl[i + 1] = HL_STRING;
                    i += 2;
                } else {
                    escaped_eol = 1;
                    i++;
                }
                continue;
            }
            i++;
            if (c == arg) kind = HL_ST_NORMAL;
            continue;
        }

        /* Inside a """ or ''' string: only three of the same quote end it,
         * and a backslash escapes the char after it, as in Python, raw
         * strings included. A newline never ends it. */
        if (kind == HL_ST_TSTRING) {
            hl[i] = HL_STRING;
            if (c == '\\') {
                if (i + 1 < n) hl[i + 1] = HL_STRING;
                i += 2;
                continue;
            }
            if (c == arg && i + 2 < n && p[i + 1] == arg && p[i + 2] == arg) {
                memset(hl + i, HL_STRING, 3);
                i += 3;
                kind = HL_ST_NORMAL;
                continue;
            }
            i++;
            continue;
        }

        /* Inside a {id|...|id} string: only |id} with the same id ends it. */
        if (kind == HL_ST_QSTRING) {
            hl[i] = HL_STRING;
            if (c == '|') {
                int idlen = hlQuotedIdLen(p + i + 1, n - i - 1);
                int end = i + 1 + idlen;
                if (end < n && p[end] == '}' && hlQuotedIdHash(p + i + 1, idlen) == arg) {
                    memset(hl + i, HL_STRING, end + 1 - i);
                    i = end + 1;
                    kind = HL_ST_NORMAL;
                    continue;
                }
            }
            i++;
            continue;
        }

        /* Single line comment: the rest of the row. It starts anywhere
         * outside a string, or only at the start of a word in shell. */
        if (hlAt(p, i, n, scs) && (!(flags & HL_WORD_START_COMMENTS) || i == 0 ||
                                   hlIsShellWordBreak((unsigned char)p[i - 1]))) {
            memset(hl + i, HL_COMMENT, n - i);
            break;
        }

        /* Block comment start. */
        if (hlAt(p, i, n, mcs)) {
            memset(hl + i, HL_MLCOMMENT, mcs->len);
            i += mcs->len;
            kind = HL_ST_COMMENT;
            arg = 1;
            continue;
        }

        /* OCaml quote: a char literal if one fits, else a type variable
         * ('a), unless it is the prime of an identifier (x'). */
        if ((flags & HL_PRIME_QUOTES) && c == '\'' &&
            !(i > 0 && hlIsIdentChar((unsigned char)p[i - 1]))) {
            int len = (flags & HL_HIGHLIGHT_STRINGS) ? hlCharLiteralLen(p + i, n - i) : 0;
            if (len) {
                memset(hl + i, HL_STRING, len);
                i += len;
                continue;
            }
            if (i + 1 < n && (isalpha((unsigned char)p[i + 1]) || p[i + 1] == '_')) {
                int j = i + 1;
                while (j < n && (isalnum((unsigned char)p[j]) || p[j] == '_')) j++;
                memset(hl + i, HL_KEYWORD2, j - i);
                i = j;
                continue;
            }
        }

        /* String starts. */
        if (flags & HL_HIGHLIGHT_STRINGS) {
            if (c == '"' || (c == '\'' && !(flags & HL_PRIME_QUOTES))) {
                if ((flags & HL_TRIPLE_QUOTES) && i + 2 < n && p[i + 1] == c && p[i + 2] == c) {
                    memset(hl + i, HL_STRING, 3);
                    i += 3;
                    kind = HL_ST_TSTRING;
                    arg = c;
                    continue;
                }
                hl[i++] = HL_STRING;
                kind = HL_ST_STRING;
                arg = c;
                continue;
            }
            if ((flags & HL_QUOTED_STRINGS) && c == '{') {
                int idlen = hlQuotedIdLen(p + i + 1, n - i - 1);
                int end = i + 1 + idlen;
                if (end < n && p[end] == '|') {
                    memset(hl + i, HL_STRING, end + 1 - i);
                    arg = hlQuotedIdHash(p + i + 1, idlen);
                    kind = HL_ST_QSTRING;
                    i = end + 1;
                    continue;
                }
            }
        }

        /* Control characters. Bytes >= 0x80 are UTF-8 text. */
        if (c < 0x20 || c == 0x7f) {
            hl[i++] = HL_NONPRINT;
            continue;
        }

        /* Numbers: a digit at the start of a word, then the rest of the
         * literal (0x1F, 1_000, 3.14, 1e-5, 0x1p-3, 10UL, 42L). Any char
         * that cannot be part of a word starts one, as for keywords, so the
         * 2 in a[1:2], {1:2} and y?1:2 is a number but the 1 in x1 is not. */
        if ((flags & HL_HIGHLIGHT_NUMBERS) && isdigit(c) &&
            (i == 0 || !hlIsWordChar((unsigned char)p[i - 1]))) {
            int hex = c == '0' && i + 1 < n && (p[i + 1] == 'x' || p[i + 1] == 'X');
            int j = i + 1;
            while (j < n) {
                int d = (unsigned char)p[j], e = (unsigned char)p[j - 1];
                if (isalnum(d) || d == '_' || d == '.') {
                    j++;
                } else if ((d == '+' || d == '-') &&
                           (hex ? (e == 'p' || e == 'P') : (e == 'e' || e == 'E'))) {
                    j++;
                } else {
                    break;
                }
            }
            memset(hl + i, HL_NUMBER, j - i);
            i = j;
            continue;
        }

        /* Keywords and types: a whole word, so "int" matches in "vector<int>"
         * and "std::string" but not in "print". */
        if (i == 0 || !hlIsWordChar((unsigned char)p[i - 1])) {
            int j;
            for (j = 0; keywords[j]; j++) {
                int klen = strlen(keywords[j]);
                int kw2 = keywords[j][klen - 1] == '|';
                if (kw2) klen--;

                /* render is NUL terminated, so p[n] ends a word. */
                if (i + klen <= n && !memcmp(p + i, keywords[j], klen) &&
                    !hlIsWordChar((unsigned char)p[i + klen])) {
                    memset(hl + i, kw2 ? HL_KEYWORD2 : HL_KEYWORD1, klen);
                    i += klen;
                    break;
                }
            }
            if (keywords[j] != NULL) continue;
        }

        /* Not special chars */
        i++;
    }

    /* A string left open at the end of the row only carries over if the
     * language allows multi row strings or the newline is escaped. */
    if (kind == HL_ST_STRING && !(flags & HL_MULTILINE_STRINGS) && !escaped_eol)
        kind = HL_ST_NORMAL;
    return kind == HL_ST_NORMAL ? HL_ST_NORMAL : HL_ST(kind, arg);
}

/* Set every byte of row->hl (that corresponds to every character in the line)
 * to the right syntax highlight type (HL_* defines). The state a row ends in
 * is where the next row starts, so while that state changes, the following
 * rows are highlighted again too. This is a loop, not recursion: opening a
 * comment at the top of a huge file must not overflow the stack. */
void editorUpdateSyntax(erow *row) {
    while (1) {
        row->hl = growBuf(row->hl, &row->hl_cap, row->rsize);
        memset(row->hl, HL_NORMAL, row->rsize);
        if (E.syntax == NULL) { /* No syntax, everything is HL_NORMAL. */
            row->hl_state = HL_ST_NORMAL;
            return;
        }

        int in = row->idx > 0 ? E.row[row->idx - 1].hl_state : HL_ST_NORMAL;
        if (in == HL_ST_INVALID) in = HL_ST_NORMAL;
        int out = editorHighlightRow(row, in);
        int changed = out != row->hl_state;
        row->hl_state = out;
        if (!changed || row->idx + 1 >= E.numrows) return;
        row = &E.row[row->idx + 1];
    }
}

/* Maps syntax highlight token types to terminal colors. */
int editorSyntaxToColor(int hl) {
    switch (hl) {
    case HL_COMMENT:
    case HL_MLCOMMENT: return 36; /* cyan */
    case HL_KEYWORD1: return 33;  /* yellow */
    case HL_KEYWORD2: return 32;  /* green */
    case HL_STRING: return 35;    /* magenta */
    case HL_NUMBER: return 31;    /* red */
    case HL_MATCH: return 34;     /* blu */
    default: return 37;           /* white */
    }
}

/* Select the syntax highlight scheme depending on the filename,
 * setting it in the global state E.syntax (NULL if nothing matches). */
void editorSelectSyntaxHighlight(char *filename) {
    const char *base = strrchr(filename, '/');
    base = base ? base + 1 : filename;
    size_t blen = strlen(base);
    E.syntax = NULL;
    for (int j = 0; j < HLDB_ENTRIES; j++) {
        struct editorSyntax *s = HLDB[j];
        for (int i = 0; s->filenames[i]; i++) {
            if (!strcmp(base, s->filenames[i])) {
                E.syntax = s;
                return;
            }
        }
        for (int i = 0; s->extensions[i]; i++) {
            size_t elen = strlen(s->extensions[i]);
            if (blen > elen && !strcmp(base + blen - elen, s->extensions[i])) {
                E.syntax = s;
                return;
            }
        }
    }
}
