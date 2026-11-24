/* Property test for the incremental highlighter: after random edits, the
 * colors on screen must equal those of a fresh open of the saved file.
 * Each row's lexer state carries into the next, so an edit that fails to
 * re-highlight the rows after it shows up here. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../keys.h"
#include "../pty.h"
#include "../test.h"

static uint32_t rng_state;
static uint32_t rnd(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

/* Tokens that move the lexer between states, plus plain text and motion. */
static const char *const tokens[] = {
    "/*", "*/", "(*", "*)", "\"", "'", "\"\"\"", "'''", "\\", "#", "//",
    "{a|", "|a}", "1", "0x1F", "3.14", "if", "int", "return", "def", "let",
    " ", " ", "x", "y_", "(", ")", "$", "`",
    K_ENTER, K_ENTER, K_BACKSPACE, K_BACKSPACE, K_DEL,
    K_LEFT, K_RIGHT, K_UP, K_DOWN, K_HOME, K_END,
};

static void edit_randomly(quip *q, int steps) {
    int n = (int)(sizeof(tokens) / sizeof(tokens[0]));
    for (int i = 0; i < steps; i++) {
        const char *t = tokens[rnd() % n];
        if (t[0] == '\x1b' || t[0] == '\r' || t[0] == '\x7f') qkey(q, t);
        else qtype(q, t);
    }
}

static void compare_with_fresh(quip *q, const char *file, uint32_t seed) {
    /* Show the top of the file, with the cursor on line 1 in both. */
    qkey(q, K_CTRL_S);
    qkey(q, K_CTRL_UP);
    char *text = qread(q, file, NULL);
    quip *f = qstart(file, text);
    for (int row = 1; row <= q->scr.rows - 2; row++) {
        for (int col = 5; col <= q->scr.cols; col++) {
            const cell *a = qcell(q, row, col), *b = qcell(f, row, col);
            if (a->cp == ' ' && b->cp == ' ') continue;
            if (a->cp != b->cp || a->fg != b->fg) {
                printf("seed %u: file after the edits:\n%s\n", seed, text);
                printf("row %d col %d: incremental '%c' fg%d, fresh '%c' fg%d\n",
                       row, col, (int)a->cp, a->fg, (int)b->cp, b->fg);
                test_fail(__FILE__, __LINE__, "highlight differs from a fresh open");
            }
        }
    }
    free(text);
    qquit(f);
}

static void run(const char *file, const char *start, uint32_t first_seed,
                int seeds) {
    for (uint32_t seed = first_seed; seed < first_seed + seeds; seed++) {
        rng_state = seed * 2654435761u + 1;
        quip *q = qstart(file, start);
        edit_randomly(q, 80);
        compare_with_fresh(q, file, seed);
        qquit(q);
    }
}

TEST(highlight_incremental_c) {
    run("a.c", "int a = 1;\n/* c */\nchar *s = \"x\";\nreturn 0;\n", 1, 6);
}

TEST(highlight_incremental_python) {
    run("a.py", "def f():\n    \"\"\"doc\"\"\"\n    return 1 # c\n", 100, 6);
}

TEST(highlight_incremental_ocaml) {
    run("a.ml", "let x = (* a (* b *) *) 1\nlet s = {a|q|a}\n", 200, 6);
}

TEST(highlight_incremental_shell) {
    run("a.sh", "echo \"$x\" # c\nif [ 1 ]; then x; fi\n", 300, 6);
}
