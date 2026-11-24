/* Random editing sessions. See keygen.h. */
#include "keygen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "keys.h"
#include "test.h"

static uint32_t next(uint32_t *s) {
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return *s;
}

static const struct { const char *file, *content; } starts[] = {
    {"a.c",
     "#include <stdio.h>\n/* block\n   comment */\nint main(void) {\n"
     "\tchar *s = \"str\\\"ing\";  \n\tif (x[1] > 0x1F) { return 42; }\n"
     "\t// line comment (\n}\n"},
    {"a.py",
     "def f(a, b):\n    \"\"\"Doc\n    string\"\"\"\n    return a + b  # sum\n"
     "x = '''t'''\n"},
    {"a.txt",
     "plain text with trailing spaces   \ncaf\xc3\xa9 \xe6\x97\xa5\xe6\x9c\xac\n"
     "\tindented\n"
     "a long line abcdefghijklmnopqrstuvwxyz abcdefghijklmnopqrstuvwxyz "
     "abcdefghijklmnopqrstuvwxyz abcdefghijklmnopqrstuvwxyz abcdefghij\n"},
    {"new.txt", NULL},
    {"a.sh", "#!/bin/sh\necho \"$HOME\" # c\nx=${#arr}; [ -f a ] && echo 'q'\n"},
    {"a.ml", "let x = (* a (* b *) *) 1\nlet s = {id|q|id} and c = 'a'\n"},
    {"a.md", "# Title\n\nSome *text* <!-- c -->\n"},
    {"lines.txt", NULL}, /* numbered lines, filled in below */
};

static const char *const keys[] = {
    K_UP, K_DOWN, K_LEFT, K_RIGHT, K_UP, K_DOWN, K_LEFT, K_RIGHT,
    K_HOME, K_END, K_PGUP, K_PGDN, K_DEL, K_BACKSPACE, K_BACKSPACE,
    K_ENTER, K_ENTER, K_TAB, K_SHIFT_TAB,
    K_SHIFT_UP, K_SHIFT_DOWN, K_SHIFT_LEFT, K_SHIFT_RIGHT,
    K_ALT_LEFT, K_ALT_RIGHT, K_ALT_UP, K_ALT_DOWN, K_META_B, K_META_F,
    K_SHIFT_ALT_LEFT, K_SHIFT_ALT_RIGHT,
    K_CTRL_UP, K_CTRL_DOWN, K_CTRL_LEFT, K_CTRL_RIGHT,
    K_SHIFT_CTRL_UP, K_SHIFT_CTRL_DOWN, K_SHIFT_CTRL_LEFT, K_SHIFT_CTRL_RIGHT,
    K_CTRL_C, K_CTRL_X, K_CTRL_V, K_CTRL_Z, K_CTRL_Y, K_CTRL_Z, K_CTRL_S,
    K_CTRL_F, K_CTRL_E, K_CTRL_SLASH,
    "\x07", "\x04", "\x15", /* unbound Ctrl keys */
    "\xc3\xa9", "\xe6\x97\xa5", /* UTF-8 characters, sent whole */
};

static char *dupkey(const char *s, int len) {
    char *k = malloc(len + 1);
    memcpy(k, s, len);
    k[len] = '\0';
    return k;
}

static char *mouse_key(uint32_t *s, int rows, int cols) {
    static const int buttons[] = {0, 0, 0, 3, 3, 32, 32, 64, 65, 66, 67};
    int b = buttons[next(s) % (sizeof(buttons) / sizeof(buttons[0]))];
    int row = 1 + (int)(next(s) % (rows + 1));
    int col = 1 + (int)(next(s) % (cols + 1));
    if (row > 223) row = 223;
    if (col > 223) col = 223;
    char seq[6] = {'\x1b', '[', 'M', (char)(32 + b), (char)(32 + col),
                   (char)(32 + row)};
    return dupkey(seq, 6);
}

void kcase_make(kcase *c, uint32_t seed, int nkeys) {
    uint32_t s = seed * 2654435761u + 0x9E3779B9u;
    if (!s) s = 1;
    memset(c, 0, sizeof(*c));
    c->seed = seed;
    int pick = (int)(next(&s) % (sizeof(starts) / sizeof(starts[0])));
    c->file = starts[pick].file;
    if (!strcmp(c->file, "lines.txt")) {
        char *buf = malloc(80 * 12 + 1);
        int o = 0;
        for (int i = 1; i <= 80; i++) o += sprintf(buf + o, "line %d\n", i);
        c->content = buf;
    } else {
        c->content = starts[pick].content ? strdup(starts[pick].content) : NULL;
    }
    int size = (int)(next(&s) % 10);
    if (size < 6) { c->rows = 24; c->cols = 80; }
    else if (size < 9) { c->rows = 3 + (int)(next(&s) % 10); c->cols = 5 + (int)(next(&s) % 50); }
    else { c->rows = 40 + (int)(next(&s) % 20); c->cols = 100 + (int)(next(&s) % 100); }

    c->keys = malloc(sizeof(char *) * (nkeys > 0 ? nkeys : 1));
    int nk = (int)(sizeof(keys) / sizeof(keys[0]));
    for (int i = 0; i < nkeys; i++) {
        int r = (int)(next(&s) % 100);
        if (r < 40) {
            /* Printable text, weighted towards characters the lexers care
             * about. */
            static const char text[] =
                "abcxyz_ 019(){}[]\"'/*#\\|$`.,;:=+-<>!?&^";
            char ch = text[next(&s) % (sizeof(text) - 1)];
            c->keys[i] = dupkey(&ch, 1);
        } else if (r < 85) {
            const char *k = keys[next(&s) % nk];
            c->keys[i] = dupkey(k, (int)strlen(k));
        } else if (r < 98) {
            c->keys[i] = mouse_key(&s, c->rows, c->cols);
        } else {
            c->keys[i] = dupkey(K_ESC, 1);
        }
    }
    c->nkeys = nkeys;
}

void kcase_make_no_tree(kcase *c, uint32_t seed, int nkeys) {
    kcase_make(c, seed, nkeys);
    for (int i = 0; i < c->nkeys; i++)
        if (!strcmp(c->keys[i], K_CTRL_E)) c->keys[i][0] = 'e';
}

void kcase_free(kcase *c) {
    for (int i = 0; i < c->nkeys; i++) free(c->keys[i]);
    free(c->keys);
    free((char *)c->content);
    memset(c, 0, sizeof(*c));
}

void kcase_subset(const kcase *c, const char *keep, kcase *out) {
    *out = *c;
    out->content = c->content ? strdup(c->content) : NULL;
    out->keys = malloc(sizeof(char *) * (c->nkeys > 0 ? c->nkeys : 1));
    out->nkeys = 0;
    for (int i = 0; i < c->nkeys; i++)
        if (keep[i]) out->keys[out->nkeys++] = strdup(c->keys[i]);
}

/* Frames quip draws for a key: one per key it reads, and it reads an
 * escape sequence as one key but anything else byte by byte. */
static int key_frames(const char *k) {
    return k[0] == '\x1b' ? 1 : (int)strlen(k);
}

quip *kcase_run(const kcase *c, const qopts *base) {
    qopts o = base ? *base : (qopts){0};
    o.rows = c->rows;
    o.cols = c->cols;
    quip *q = qstart_opts(c->file, c->content, &o);
    for (int i = 0; i < c->nkeys; i++)
        qsend(q, c->keys[i], (int)strlen(c->keys[i]), key_frames(c->keys[i]));
    return q;
}

static void print_c_string(const char *s) {
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '\n') fputs("\\n", stdout);
        else if (*p == '\t') fputs("\\t", stdout);
        else if (*p == '"' || *p == '\\') printf("\\%c", *p);
        else if (*p < 0x20 || *p >= 0x7f) printf("\\%03o", *p);
        else putchar(*p);
    }
    putchar('"');
}

void kcase_print_test(const kcase *c, const char *name) {
    printf("TEST(%s) {\n", name);
    printf("    qopts o = {.rows = %d, .cols = %d};\n", c->rows, c->cols);
    printf("    quip *q = qstart_opts(\"%s\", ", c->file);
    if (c->content) print_c_string(c->content);
    else fputs("NULL", stdout);
    printf(", &o);\n");
    for (int i = 0; i < c->nkeys; i++) {
        const char *k = c->keys[i];
        fputs(k[0] != '\x1b' && strlen(k) > 1 ? "    qtype(q, " : "    qkey(q, ", stdout);
        print_c_string(k);
        fputs(");\n", stdout);
    }
    printf("    qkey(q, \"\\033\");\n    qkey(q, \"\\033\");\n");
    printf("    qquit(q);\n}\n");
}
