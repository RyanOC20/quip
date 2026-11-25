/* Terminal screen model for the tests. See screen.h. */
#include "screen.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { ST_GROUND, ST_ESC, ST_CSI, ST_OSC, ST_OSC_ESC };

static void screen_error(screen *s, const char *fmt, ...) {
    s->nerrors++;
    size_t used = strlen(s->error);
    if (s->nerrors > 8 || used + 2 >= sizeof(s->error)) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->error + used, sizeof(s->error) - used, fmt, ap);
    va_end(ap);
    used = strlen(s->error);
    if (used + 2 < sizeof(s->error)) strcpy(s->error + used, "; ");
}

static void blank_cells(screen *s, cell *c, int n) {
    for (int i = 0; i < n; i++) {
        c[i].cp = ' ';
        c[i].fg = 0;
        c[i].bg = s->bg;
        c[i].attr = s->attr & ATTR_REVERSE;
    }
}

void screen_init(screen *s, int rows, int cols) {
    memset(s, 0, sizeof(*s));
    s->rows = rows;
    s->cols = cols;
    s->main_grid = malloc(sizeof(cell) * rows * cols);
    s->alt_grid = malloc(sizeof(cell) * rows * cols);
    blank_cells(s, s->main_grid, rows * cols);
    blank_cells(s, s->alt_grid, rows * cols);
    s->grid = s->main_grid;
    s->cursor_visible = 1;
}

void screen_free(screen *s) {
    free(s->main_grid);
    free(s->alt_grid);
    free(s->osc);
    free(s->clipboard);
    memset(s, 0, sizeof(*s));
}

void screen_resize(screen *s, int rows, int cols) {
    free(s->main_grid);
    free(s->alt_grid);
    s->rows = rows;
    s->cols = cols;
    s->main_grid = malloc(sizeof(cell) * rows * cols);
    s->alt_grid = malloc(sizeof(cell) * rows * cols);
    blank_cells(s, s->main_grid, rows * cols);
    blank_cells(s, s->alt_grid, rows * cols);
    s->grid = s->alt ? s->alt_grid : s->main_grid;
    if (s->x >= cols) s->x = cols - 1;
    if (s->y >= rows) s->y = rows - 1;
    s->wrap_pending = 0;
}

const cell *screen_cell(const screen *s, int y, int x) { return &s->grid[y * s->cols + x]; }

/* A compact width table: combining marks and zero width characters take no
 * cell; East Asian wide and fullwidth characters and emoji take two. */
int screen_cpwidth(uint32_t cp) {
    static const uint32_t zero[][2] = {
        {0x0300, 0x036F}, {0x0483, 0x0489},   {0x0591, 0x05BD}, {0x0610, 0x061A}, {0x064B, 0x065F},
        {0x0E31, 0x0E31}, {0x0E34, 0x0E3A},   {0x1AB0, 0x1AFF}, {0x1DC0, 0x1DFF}, {0x200B, 0x200F},
        {0x202A, 0x202E}, {0x2060, 0x2064},   {0x20D0, 0x20FF}, {0xFE00, 0xFE0F}, {0xFE20, 0xFE2F},
        {0xFEFF, 0xFEFF}, {0xE0100, 0xE01EF},
    };
    static const uint32_t wide[][2] = {
        {0x1100, 0x115F},   {0x231A, 0x231B},   {0x2329, 0x232A},   {0x23E9, 0x23EC},
        {0x23F0, 0x23F0},   {0x23F3, 0x23F3},   {0x25FD, 0x25FE},   {0x2614, 0x2615},
        {0x2648, 0x2653},   {0x267F, 0x267F},   {0x2693, 0x2693},   {0x26A1, 0x26A1},
        {0x26AA, 0x26AB},   {0x26BD, 0x26BE},   {0x26C4, 0x26C5},   {0x26CE, 0x26CE},
        {0x26D4, 0x26D4},   {0x26EA, 0x26EA},   {0x26F2, 0x26F3},   {0x26F5, 0x26F5},
        {0x26FA, 0x26FA},   {0x26FD, 0x26FD},   {0x2705, 0x2705},   {0x270A, 0x270B},
        {0x2728, 0x2728},   {0x274C, 0x274C},   {0x274E, 0x274E},   {0x2753, 0x2755},
        {0x2757, 0x2757},   {0x2795, 0x2797},   {0x27B0, 0x27B0},   {0x27BF, 0x27BF},
        {0x2B1B, 0x2B1C},   {0x2B50, 0x2B50},   {0x2B55, 0x2B55},   {0x2E80, 0x303E},
        {0x3041, 0x33FF},   {0x3400, 0x4DBF},   {0x4E00, 0x9FFF},   {0xA000, 0xA4CF},
        {0xA960, 0xA97F},   {0xAC00, 0xD7A3},   {0xF900, 0xFAFF},   {0xFE10, 0xFE19},
        {0xFE30, 0xFE6F},   {0xFF00, 0xFF60},   {0xFFE0, 0xFFE6},   {0x16FE0, 0x16FE4},
        {0x17000, 0x18CFF}, {0x1B000, 0x1B2FF}, {0x1F004, 0x1F004}, {0x1F0CF, 0x1F0CF},
        {0x1F18E, 0x1F18E}, {0x1F191, 0x1F19A}, {0x1F200, 0x1F251}, {0x1F300, 0x1F64F},
        {0x1F680, 0x1F6FF}, {0x1F7E0, 0x1F7EB}, {0x1F90C, 0x1F9FF}, {0x1FA70, 0x1FAFF},
        {0x20000, 0x2FFFD}, {0x30000, 0x3FFFD},
    };
    if (cp < 0x300) return 1;
    for (size_t i = 0; i < sizeof(zero) / sizeof(zero[0]); i++)
        if (cp >= zero[i][0] && cp <= zero[i][1]) return 0;
    for (size_t i = 0; i < sizeof(wide) / sizeof(wide[0]); i++)
        if (cp >= wide[i][0] && cp <= wide[i][1]) return 2;
    return 1;
}

static void scroll_up(screen *s) {
    memmove(s->grid, s->grid + s->cols, sizeof(cell) * (s->rows - 1) * s->cols);
    blank_cells(s, s->grid + (s->rows - 1) * s->cols, s->cols);
}

static void line_feed(screen *s) {
    s->wrap_pending = 0;
    if (s->y == s->rows - 1)
        scroll_up(s);
    else
        s->y++;
}

static void put_cp(screen *s, uint32_t cp) {
    int w = screen_cpwidth(cp);
    if (w == 0) return; /* Combining: belongs to the previous cell. */
    if (s->wrap_pending || (w == 2 && s->x == s->cols - 1)) {
        s->x = 0;
        line_feed(s);
    }
    cell *c = &s->grid[s->y * s->cols + s->x];
    c->cp = cp;
    c->fg = s->fg;
    c->bg = s->bg;
    c->attr = s->attr;
    if (w == 2) {
        c[1] = c[0];
        c[1].cp = 0;
    }
    s->x += w;
    if (s->x >= s->cols) {
        s->x = s->cols - 1;
        s->wrap_pending = 1;
    }
}

/* Parse the numeric parameters of the CSI in s->buf (after any '?'). */
static int csi_params(const char *p, int *out, int max) {
    int n = 0;
    out[0] = -1;
    if (!*p) return 0;
    while (n < max) {
        if (*p >= '0' && *p <= '9') {
            int v = 0;
            while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
            out[n] = v;
        } else {
            out[n] = -1; /* Omitted: use the default. */
        }
        n++;
        if (*p != ';') break;
        p++;
    }
    return n;
}

static void sgr(screen *s, int *p, int n) {
    if (n == 0) {
        p[0] = 0;
        n = 1;
    }
    for (int i = 0; i < n; i++) {
        int v = p[i] < 0 ? 0 : p[i];
        if (v == 0) {
            s->fg = s->bg = 0;
            s->attr = 0;
        } else if (v == 1)
            s->attr |= ATTR_BOLD;
        else if (v == 7)
            s->attr |= ATTR_REVERSE;
        else if (v == 22)
            s->attr &= ~ATTR_BOLD;
        else if (v == 27)
            s->attr &= ~ATTR_REVERSE;
        else if ((v >= 30 && v <= 37) || (v >= 90 && v <= 97))
            s->fg = v;
        else if (v == 39)
            s->fg = 0;
        else if ((v >= 40 && v <= 47) || (v >= 100 && v <= 107))
            s->bg = v;
        else if (v == 49)
            s->bg = 0;
        else
            screen_error(s, "unknown SGR %d", v);
    }
}

static void set_mode(screen *s, int mode, int on) {
    int bit = 0;
    switch (mode) {
    case 25:
        s->cursor_visible = on;
        if (on) s->frames++;
        return;
    case 1049:
        if (on && !s->alt) {
            s->saved_x = s->x;
            s->saved_y = s->y;
            s->alt = 1;
            s->grid = s->alt_grid;
            blank_cells(s, s->grid, s->rows * s->cols);
        } else if (!on && s->alt) {
            s->alt = 0;
            s->grid = s->main_grid;
            s->x = s->saved_x;
            s->y = s->saved_y;
        }
        s->wrap_pending = 0;
        return;
    case 1002: bit = MODE_MOUSE_BUTTON; break;
    case 1006: bit = MODE_MOUSE_SGR; break;
    case 2004: bit = MODE_BRACKET_PASTE; break;
    default: screen_error(s, "unknown private mode %d", mode); return;
    }
    if (on)
        s->modes |= bit;
    else
        s->modes &= ~bit;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void csi_dispatch(screen *s, char final) {
    s->buf[s->blen] = '\0';
    const char *p = s->buf;
    int private = 0;
    if (*p == '?') {
        private = 1;
        p++;
    }
    /* Intermediate bytes (0x20-0x2F) sit just before the final byte. */
    char inter = 0;
    if (s->blen && s->buf[s->blen - 1] >= 0x20 && s->buf[s->blen - 1] <= 0x2F) {
        inter = s->buf[s->blen - 1];
        s->buf[s->blen - 1] = '\0';
    }
    int prm[16];
    int n = csi_params(p, prm, 16);
    int a = (n > 0 && prm[0] > 0) ? prm[0] : 1;

    if (inter == ' ' && final == 'q' && !private) {
        s->cursor_shape = n > 0 && prm[0] >= 0 ? prm[0] : 0;
        return;
    }
    if (inter) {
        screen_error(s, "unknown CSI '%s%c%c'", s->buf, inter, final);
        return;
    }
    if (private) {
        if (final != 'h' && final != 'l') {
            screen_error(s, "unknown CSI '?%s%c'", p, final);
            return;
        }
        for (int i = 0; i < n; i++) set_mode(s, prm[i], final == 'h');
        return;
    }
    switch (final) {
    case 'H':
    case 'f': {
        int row = (n > 0 && prm[0] > 0) ? prm[0] : 1;
        int col = (n > 1 && prm[1] > 0) ? prm[1] : 1;
        s->y = clampi(row - 1, 0, s->rows - 1);
        s->x = clampi(col - 1, 0, s->cols - 1);
        s->wrap_pending = 0;
        break;
    }
    case 'A':
        s->y = clampi(s->y - a, 0, s->rows - 1);
        s->wrap_pending = 0;
        break;
    case 'B':
        s->y = clampi(s->y + a, 0, s->rows - 1);
        s->wrap_pending = 0;
        break;
    case 'C':
        s->x = clampi(s->x + a, 0, s->cols - 1);
        s->wrap_pending = 0;
        break;
    case 'D':
        s->x = clampi(s->x - a, 0, s->cols - 1);
        s->wrap_pending = 0;
        break;
    case 'K': {
        int mode = (n > 0 && prm[0] >= 0) ? prm[0] : 0;
        cell *row = s->grid + s->y * s->cols;
        /* Erase from the cursor, inclusive, as a VT100 does, even when a
         * character was just printed into the last column. */
        if (mode == 0)
            blank_cells(s, row + s->x, s->cols - s->x);
        else if (mode == 1)
            blank_cells(s, row, s->x + 1);
        else if (mode == 2)
            blank_cells(s, row, s->cols);
        else
            screen_error(s, "unknown EL %d", mode);
        s->wrap_pending = 0;
        break;
    }
    case 'm': sgr(s, prm, n); break;
    case 'n':
        if (n == 1 && prm[0] == 6) {
            if (s->answer_cursor && s->reply) {
                char r[32];
                int len = snprintf(r, sizeof(r), "\x1b[%d;%dR", s->y + 1, s->x + 1);
                s->reply(s->reply_ctx, r, len);
            }
        } else {
            screen_error(s, "unknown DSR '%s'", p);
        }
        break;
    default: screen_error(s, "unknown CSI '%s%c'", s->buf, final);
    }
}

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64val(char c) {
    const char *p = c ? strchr(b64, c) : NULL;
    return p ? (int)(p - b64) : -1;
}

static void osc_dispatch(screen *s) {
    const char *p = s->osc ? s->osc : "";
    if (strncmp(p, "52;c;", 5) != 0) {
        screen_error(s, "unknown OSC '%.20s'", p);
        return;
    }
    p += 5;
    if (strcmp(p, "?") == 0) {
        if (s->answer_clipboard && s->reply) {
            int len = s->clipboard_len;
            int elen = 4 * ((len + 2) / 3);
            char *r = malloc(elen + 16);
            int o = sprintf(r, "\x1b]52;c;");
            const unsigned char *src = (const unsigned char *)s->clipboard;
            for (int i = 0; i < len; i += 3) {
                int v = src[i] << 16;
                if (i + 1 < len) v |= src[i + 1] << 8;
                if (i + 2 < len) v |= src[i + 2];
                r[o++] = b64[(v >> 18) & 63];
                r[o++] = b64[(v >> 12) & 63];
                r[o++] = i + 1 < len ? b64[(v >> 6) & 63] : '=';
                r[o++] = i + 2 < len ? b64[v & 63] : '=';
            }
            r[o++] = '\x07';
            s->reply(s->reply_ctx, r, o);
            free(r);
        }
        return;
    }
    /* A clipboard write: decode it. */
    int plen = (int)strlen(p);
    char *out = malloc(plen * 3 / 4 + 4);
    int o = 0, bits = 0, acc = 0;
    for (int i = 0; i < plen && p[i] != '='; i++) {
        int v = b64val(p[i]);
        if (v < 0) {
            screen_error(s, "bad base64 in OSC 52");
            free(out);
            return;
        }
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (char)((acc >> bits) & 0xFF);
        }
    }
    out[o] = '\0';
    free(s->clipboard);
    s->clipboard = out;
    s->clipboard_len = o;
}

/* Longer than any terminal keeps; quip caps its clipboard writes well
 * below this. */
#define OSC_MAX (64 << 20)

static void osc_add(screen *s, char c) {
    if (s->osclen >= OSC_MAX) return; /* reported already; drop the rest */
    if (s->osclen == OSC_MAX - 1) {
        screen_error(s, "OSC longer than %d bytes", OSC_MAX);
        s->osclen = OSC_MAX;
        return;
    }
    if (s->osclen + 2 > s->osccap) {
        s->osccap = s->osccap ? s->osccap * 2 : 256;
        s->osc = realloc(s->osc, s->osccap);
    }
    s->osc[s->osclen++] = c;
    s->osc[s->osclen] = '\0';
}

static void ground_byte(screen *s, unsigned char c) {
    if (s->u8need) {
        if ((c & 0xC0) == 0x80) {
            s->u8cp = (s->u8cp << 6) | (c & 0x3F);
            if (--s->u8need == 0) put_cp(s, s->u8cp);
            return;
        }
        s->u8need = 0;
        put_cp(s, 0xFFFD); /* Truncated sequence; reprocess this byte. */
    }
    if (c == 0x1b) {
        s->state = ST_ESC;
        return;
    }
    if (c == '\r') {
        s->x = 0;
        s->wrap_pending = 0;
        return;
    }
    if (c == '\n') {
        line_feed(s);
        return;
    }
    if (c < 0x20 || c == 0x7f) {
        screen_error(s, "raw control byte 0x%02x", c);
        return;
    }
    if (c < 0x80) {
        put_cp(s, c);
        return;
    }
    if ((c & 0xE0) == 0xC0) {
        s->u8cp = c & 0x1F;
        s->u8need = 1;
        return;
    }
    if ((c & 0xF0) == 0xE0) {
        s->u8cp = c & 0x0F;
        s->u8need = 2;
        return;
    }
    if ((c & 0xF8) == 0xF0) {
        s->u8cp = c & 0x07;
        s->u8need = 3;
        return;
    }
    put_cp(s, 0xFFFD); /* Stray continuation or invalid lead byte. */
}

void screen_feed(screen *s, const char *data, int len) {
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)data[i];
        switch (s->state) {
        case ST_GROUND: ground_byte(s, c); break;
        case ST_ESC:
            if (c == '[') {
                s->state = ST_CSI;
                s->blen = 0;
            } else if (c == ']') {
                s->state = ST_OSC;
                s->osclen = 0;
                if (s->osc) s->osc[0] = 0;
            } else {
                screen_error(s, "unknown ESC %c", c);
                s->state = ST_GROUND;
            }
            break;
        case ST_CSI:
            if (c >= 0x40 && c <= 0x7E) {
                csi_dispatch(s, (char)c);
                s->state = ST_GROUND;
            } else if (s->blen < (int)sizeof(s->buf) - 1) {
                s->buf[s->blen++] = (char)c;
            } else {
                screen_error(s, "CSI too long");
                s->state = ST_GROUND;
            }
            break;
        case ST_OSC:
            if (c == 0x07) {
                osc_dispatch(s);
                s->state = ST_GROUND;
            } else if (c == 0x1b)
                s->state = ST_OSC_ESC;
            else
                osc_add(s, (char)c);
            break;
        case ST_OSC_ESC:
            if (c == '\\')
                osc_dispatch(s);
            else
                screen_error(s, "bad OSC terminator");
            s->state = ST_GROUND;
            break;
        }
    }
}

static int put_utf8(char *out, uint32_t cp) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

char *screen_span_text(const screen *s, int y, int x0, int x1, char *out, int outsize) {
    int o = 0;
    if (x0 < 0) x0 = 0;
    if (x1 > s->cols) x1 = s->cols;
    for (int x = x0; x < x1 && o + 5 < outsize; x++) {
        uint32_t cp = s->grid[y * s->cols + x].cp;
        if (cp == 0) continue; /* Right half of a wide character. */
        o += put_utf8(out + o, cp);
    }
    out[o] = '\0';
    return out;
}

char *screen_row_text(const screen *s, int y, char *out, int outsize) {
    screen_span_text(s, y, 0, s->cols, out, outsize);
    int n = (int)strlen(out);
    while (n > 0 && out[n - 1] == ' ') out[--n] = '\0';
    return out;
}

static void style_name(const cell *c, char *out, size_t size) {
    snprintf(out, size, "%s%s fg%d bg%d", (c->attr & ATTR_REVERSE) ? "rev " : "",
             (c->attr & ATTR_BOLD) ? "bold " : "", c->fg, c->bg);
}

char *screen_dump(const screen *s) {
    size_t cap = (size_t)s->rows * (s->cols * 8 + 512) + 256;
    char *out = malloc(cap);
    size_t o = 0;
    o += snprintf(out + o, cap - o, "cursor row %d col %d%s\n", s->y + 1, s->x + 1,
                  s->cursor_visible ? "" : " (hidden)");
    char *line = malloc(s->cols * 4 + 8);
    for (int y = 0; y < s->rows; y++) {
        screen_span_text(s, y, 0, s->cols, line, s->cols * 4 + 8);
        o += snprintf(out + o, cap - o, "%3d |%s|\n", y + 1, line);
    }
    free(line);
    /* Styled runs, one line each, skipping plain default cells. */
    for (int y = 0; y < s->rows; y++) {
        int x = 0;
        while (x < s->cols) {
            const cell *c = &s->grid[y * s->cols + x];
            int x1 = x + 1;
            while (x1 < s->cols) {
                const cell *d = &s->grid[y * s->cols + x1];
                if (d->fg != c->fg || d->bg != c->bg || d->attr != c->attr) break;
                x1++;
            }
            if ((c->fg || c->bg || c->attr) && cap - o > 128) {
                char st[64];
                style_name(c, st, sizeof(st));
                o +=
                    snprintf(out + o, cap - o, "    row %d cols %d-%d: %s\n", y + 1, x + 1, x1, st);
            }
            x = x1;
        }
    }
    return out;
}
