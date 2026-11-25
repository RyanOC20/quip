/* Copy and paste: the internal clipboard and the terminal's, through OSC 52. */
#include "quip.h"

/* The most text a copy sends to the terminal's clipboard. A bigger copy only
 * goes to quip's own. */
#define QUIP_OSC52_MAX (16 << 20)

/* Base64, for OSC 52 clipboard reads and writes. */

static const char b64chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64val(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Returns malloc'd NUL-terminated base64 string. Caller frees. */
static char *b64Encode(const unsigned char *src, size_t len) {
    size_t outlen = 4 * ((len + 2) / 3);
    char *out = malloc(outlen + 1);
    size_t i = 0, j = 0;
    while (i < len) {
        unsigned int a = (unsigned char)src[i++];
        unsigned int b = (i < len) ? (unsigned char)src[i++] : 0;
        unsigned int c = (i < len) ? (unsigned char)src[i++] : 0;
        unsigned int triple = (a << 16) | (b << 8) | c;
        out[j++] = b64chars[(triple >> 18) & 0x3f];
        out[j++] = b64chars[(triple >> 12) & 0x3f];
        out[j++] = b64chars[(triple >> 6) & 0x3f];
        out[j++] = b64chars[triple & 0x3f];
    }
    if (len % 3 == 1) {
        out[j - 2] = '=';
        out[j - 1] = '=';
    } else if (len % 3 == 2) {
        out[j - 1] = '=';
    }
    out[j] = '\0';
    return out;
}

/* Returns malloc'd decoded buffer. Sets *outlen. Caller frees. */
static char *b64Decode(const char *src, size_t *outlen) {
    size_t srclen = strlen(src);
    size_t maxout = (srclen / 4) * 3 + 3;
    char *out = malloc(maxout + 1);
    size_t i = 0, j = 0;
    while (i < srclen) {
        /* Skip whitespace that some terminals insert as line-wrap in responses. */
        while (i < srclen && (src[i] == '\r' || src[i] == '\n' || src[i] == ' ')) i++;
        if (i >= srclen) break;
        int v0 = b64val((unsigned char)src[i++]);
        int v1 = (i < srclen) ? b64val((unsigned char)src[i++]) : -1;
        int v2 = (i < srclen) ? b64val((unsigned char)src[i++]) : -1;
        int v3 = (i < srclen) ? b64val((unsigned char)src[i++]) : -1;
        if (v0 < 0 || v1 < 0) break;
        out[j++] = (char)((v0 << 2) | (v1 >> 4));
        if (v2 >= 0) out[j++] = (char)(((v1 & 0xf) << 4) | (v2 >> 2));
        if (v2 >= 0 && v3 >= 0) out[j++] = (char)(((v2 & 0x3) << 6) | v3);
    }
    out[j] = '\0';
    *outlen = j;
    return out;
}

/* Returns malloc'd string of selected text, sets *outlen. Caller frees.
 * Returns NULL if no selection or zero-length selection. */
static char *editorGetSelection(size_t *outlen) {
    int sr, sc, er, ec;
    if (!editorSelBounds(&sr, &sc, &er, &ec)) return NULL;

    if (sr == er) {
        int count = ec - sc;
        char *buf = malloc(count + 1);
        memcpy(buf, E.row[sr].chars + sc, count);
        buf[count] = '\0';
        *outlen = count;
        return buf;
    }

    /* Multi-row: calculate total size then fill buffer. */
    size_t size = (size_t)(E.row[sr].size - sc) + 1;             /* first row chars + newline */
    for (int i = sr + 1; i < er; i++) size += E.row[i].size + 1; /* middle rows + newlines */
    size += ec;                                                  /* last row partial */

    char *buf = malloc(size + 1);
    size_t pos = 0;
    memcpy(buf + pos, E.row[sr].chars + sc, E.row[sr].size - sc);
    pos += E.row[sr].size - sc;
    buf[pos++] = '\n';
    for (int i = sr + 1; i < er; i++) {
        memcpy(buf + pos, E.row[i].chars, E.row[i].size);
        pos += E.row[i].size;
        buf[pos++] = '\n';
    }
    memcpy(buf + pos, E.row[er].chars, ec);
    pos += ec;
    buf[pos] = '\0';
    *outlen = pos;
    return buf;
}

/* Copies selection to internal clipboard and sends it to terminal via OSC 52. */
void editorCopySelection(void) {
    size_t len;
    char *text = editorGetSelection(&len);
    if (!text) return;

    free(E.clipboard);
    E.clipboard = text;
    E.clipboard_len = len;

    /* Terminals keep a few megabytes at most, and writing more would hold
     * quip up for nothing. */
    if (len > QUIP_OSC52_MAX) {
        editorSetStatusMessage("Copied %zu bytes, too many for the terminal's clipboard", len);
        return;
    }
    char *b64 = b64Encode((unsigned char *)text, len);
    /* ESC ] 52 ; c ; <base64> BEL */
    size_t b64len = strlen(b64), seqlen = 8 + b64len;
    char *seq = malloc(seqlen + 1);
    memcpy(seq, "\x1b]52;c;", 7);
    memcpy(seq + 7, b64, b64len);
    seq[seqlen - 1] = '\a';
    seq[seqlen] = '\0';
    writeAll(STDOUT_FILENO, seq, seqlen);
    free(seq);
    free(b64);
}

/* Queries terminal clipboard via OSC 52.
 * Returns malloc'd decoded text (caller frees), or NULL on failure/timeout.
 *
 * Searches stdin for the literal prefix "\x1b]52;c;" so that everything
 * before it — interleaved mouse events, tmux DCS passthrough wrappers, etc.
 * — is consumed and never leaks into editorReadKey. */
static char *editorReadClipboard(size_t *outlen) {
    unsigned char ch;

    /* Drain anything already buffered in stdin. */
    while (readByteTimeout(&ch, 0) == 1) {}

    writeLit(STDOUT_FILENO, VT_CLIPBOARD_QUERY);

    /* Byte-by-byte KMP-lite search for "\x1b]52;c;" within a 500 ms window.
     * This tolerates mouse events and tmux DCS wrappers arriving before the
     * real response, consuming them completely so they never reach editorReadKey. */
    const char target[] = "\x1b]52;c;";
    const int tlen = (int)(sizeof(target) - 1);
    int match = 0;
    struct timeval t0, tnow;
    gettimeofday(&t0, NULL);

    while (match < tlen) {
        gettimeofday(&tnow, NULL);
        int elapsed = (int)((tnow.tv_sec - t0.tv_sec) * 1000 + (tnow.tv_usec - t0.tv_usec) / 1000);
        if (elapsed >= 500) return NULL;

        if (readByteTimeout(&ch, 100) != 1) continue;

        if (ch == (unsigned char)target[match]) {
            match++;
        } else {
            /* Mismatch: restart, but check whether this byte opens a new match. */
            match = (ch == (unsigned char)target[0]) ? 1 : 0;
        }
    }

    /* Found the prefix.  Read the base64 payload until BEL (0x07) or ST (ESC \). */
    char buf[65536];
    int i = 0;
    while (i < (int)(sizeof(buf) - 1)) {
        if (readByteTimeout(&ch, 200) != 1) break;
        if (ch == 0x07) {
            /* BEL terminator.  Drain any trailing DCS-closing bytes quickly. */
            unsigned char extra;
            while (readByteTimeout(&extra, 50) == 1) {}
            break;
        }
        if (ch == 0x1b) {
            /* ST (ESC \) or an escaped inner byte in a DCS wrapper.
             * Either way the payload is finished; drain whatever follows. */
            unsigned char extra;
            while (readByteTimeout(&extra, 50) == 1) {}
            break;
        }
        buf[i++] = (char)ch;
    }
    buf[i] = '\0';

    if (i == 0) return NULL;
    return b64Decode(buf, outlen);
}

/* Inserts text at cursor, splitting on newlines. */
static void editorPasteText(const char *text, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (text[i] == '\n')
            editorInsertNewline();
        else
            editorInsertChar((unsigned char)text[i]);
    }
}

/* Pastes from system clipboard (OSC 52), falling back to internal clipboard. */
void editorPaste(void) {
    size_t len = 0;
    char *text = editorReadClipboard(&len);
    if (text) {
        free(E.clipboard);
        E.clipboard = text;
        E.clipboard_len = len;
        editorPasteText(text, len);
    } else if (E.clipboard) {
        editorPasteText(E.clipboard, E.clipboard_len);
    }
}
