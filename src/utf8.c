/* UTF-8: decoding, character widths in terminal cells, and the cells a run
 * of text takes on screen. */
#include "quip.h"
#include "width.h"

/* Decode the UTF-8 sequence at s[i], at most n - i bytes long. Returns its
 * length and stores the code point in *cp, or returns 0 when the bytes are
 * not valid UTF-8 (overlong forms, surrogates and values past U+10FFFF
 * included), which the caller then treats as one stray byte. */
int utf8Decode(const char *s, int i, int n, uint32_t *cp) {
    const unsigned char *p = (const unsigned char *)s + i;
    int avail = n - i;
    uint32_t c = p[0];
    int len;
    uint32_t min;
    if (c < 0x80) {
        *cp = c;
        return 1;
    } else if (c >= 0xc2 && c <= 0xdf) {
        len = 2;
        c &= 0x1f;
        min = 0x80;
    } else if (c >= 0xe0 && c <= 0xef) {
        len = 3;
        c &= 0x0f;
        min = 0x800;
    } else if (c >= 0xf0 && c <= 0xf4) {
        len = 4;
        c &= 0x07;
        min = 0x10000;
    } else {
        return 0;
    }
    if (avail < len) return 0;
    for (int k = 1; k < len; k++) {
        if ((p[k] & 0xc0) != 0x80) return 0;
        c = c << 6 | (p[k] & 0x3f);
    }
    if (c < min || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return 0;
    *cp = c;
    return len;
}

/* Write code point 'cp' as UTF-8 into out (room for 4 bytes); returns the
 * number of bytes. */
int utf8Encode(char *out, uint32_t cp) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    } else if (cp < 0x800) {
        out[0] = (char)(0xc0 | cp >> 6);
        out[1] = (char)(0x80 | (cp & 0x3f));
        return 2;
    } else if (cp < 0x10000) {
        out[0] = (char)(0xe0 | cp >> 12);
        out[1] = (char)(0x80 | (cp >> 6 & 0x3f));
        out[2] = (char)(0x80 | (cp & 0x3f));
        return 3;
    }
    out[0] = (char)(0xf0 | cp >> 18);
    out[1] = (char)(0x80 | (cp >> 12 & 0x3f));
    out[2] = (char)(0x80 | (cp >> 6 & 0x3f));
    out[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}

/* True if 'cp' is in one of the packed ranges of 'tab' (see width.h). */
static int inTable(const uint32_t *tab, int n, uint32_t cp) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint32_t start = tab[mid] >> 11;
        if (cp < start)
            hi = mid - 1;
        else if (cp > start + (tab[mid] & 0x7ff))
            lo = mid + 1;
        else
            return 1;
    }
    return 0;
}

/* Cells a printable code point takes: 0 for combining marks and other zero
 * width characters, 2 for East Asian wide and fullwidth characters and most
 * emoji, otherwise 1. */
int utf8Width(uint32_t cp) {
    if (cp < 0x300) return 1;
    if (inTable(width_zero, sizeof(width_zero) / sizeof(width_zero[0]), cp)) return 0;
    if (cp >= 0x1100 && inTable(width_wide, sizeof(width_wide) / sizeof(width_wide[0]), cp))
        return 2;
    return 1;
}

/* Bytes that a terminal would obey instead of print: a C0 control or DEL
 * (1 byte), or a C1 control U+0080-U+009F written in UTF-8 (2 bytes).
 * Returns how many bytes at s[i] form one, or 0. Text from files, file names
 * and messages must never reach the terminal with these intact. */
int editorCtrlLen(const char *s, int i, int n) {
    unsigned char b = (unsigned char)s[i];
    if (b < 0x20 || b == 0x7f) return 1;
    if (b == 0xc2 && i + 1 < n && (unsigned char)s[i + 1] >= 0x80 &&
        (unsigned char)s[i + 1] <= 0x9f)
        return 2;
    return 0;
}

/* The character at s[i] (n bytes in all) as it is drawn: stores its length
 * in bytes in *len and returns the cells it takes when it starts at screen
 * column 'col'. A TAB reaches the next tab stop. A control character is
 * drawn in caret notation and a byte that is not valid UTF-8 as '?', one
 * cell per byte either way. */
int textCell(const char *s, int i, int n, int col, int *len) {
    unsigned char b = (unsigned char)s[i];
    if (b >= 0x20 && b < 0x7f) {
        *len = 1;
        return 1;
    }
    if (b == '\t') {
        *len = 1;
        return QUIP_TAB_STOP - col % QUIP_TAB_STOP;
    }
    int k = editorCtrlLen(s, i, n);
    if (k) {
        *len = k;
        return k;
    }
    uint32_t cp;
    k = utf8Decode(s, i, n, &cp);
    if (k == 0) {
        *len = 1;
        return 1;
    }
    *len = k;
    return utf8Width(cp);
}

/* How many of the n bytes of 's' fit in 'maxcols' screen columns, in whole
 * characters; the columns they take go in *cols. */
int textFit(const char *s, int n, int maxcols, int *cols) {
    int i = 0, col = 0, k;
    while (i < n) {
        int w = textCell(s, i, n, col, &k);
        if (col + w > maxcols) break;
        col += w;
        i += k;
    }
    *cols = col;
    return i;
}
