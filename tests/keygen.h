/* Random editing sessions for the fuzzer and the differential tester. A
 * session is a starting file, a terminal size and a list of keys, all
 * derived from one seed, so any seed can be replayed exactly. */
#ifndef QUIP_TEST_KEYGEN_H
#define QUIP_TEST_KEYGEN_H

#include <stdint.h>

#include "pty.h"

typedef struct kcase {
    uint32_t seed;
    const char *file;     /* File name; its extension picks the language. */
    const char *content;  /* Starting content. */
    int rows, cols;
    char **keys;          /* Each a key's bytes (NUL terminated). */
    int nkeys;
} kcase;

/* Build the session for 'seed' with 'nkeys' keys. */
void kcase_make(kcase *c, uint32_t seed, int nkeys);
/* The same, leaving out the file tree (Ctrl-E), which can open another
 * file and start a new undo history. */
void kcase_make_no_tree(kcase *c, uint32_t seed, int nkeys);
void kcase_free(kcase *c);
/* Copy of 'c' keeping only the keys whose 'keep' entry is non-zero. */
void kcase_subset(const kcase *c, const char *keep, kcase *out);

/* Start quip for the session and send every key. Calls test_fail on any
 * problem, like the end-to-end tests. Returns the running session. */
quip *kcase_run(const kcase *c, const qopts *base);

/* Print the session as an end-to-end test that replays it. */
void kcase_print_test(const kcase *c, const char *name);

#endif
