/* Differential test: replays the same random editing sessions against two
 * quip binaries and requires byte-identical results, both the terminal
 * output and the saved file. Used to prove that a refactoring changed no
 * behavior.
 *
 *   tests/differential OLD_BIN NEW_BIN [-n cases] [-s first_seed] [-k keys]
 *
 * Each session ends with Ctrl-S so the saved file can be compared too. */
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "keygen.h"
#include "keys.h"
#include "pty.h"
#include "test.h"

typedef struct result {
    int ok;
    char *out; /* Terminal output. */
    size_t outlen;
    char *file; /* Saved file. */
    int filelen;
    char *err;
} result;

/* Read all of 'fd' into a buffer. */
static char *slurp(int fd, size_t *len) {
    size_t cap = 65536, n = 0;
    char *buf = malloc(cap);
    ssize_t r;
    while ((r = read(fd, buf + n, cap - n)) > 0) {
        n += r;
        if (n == cap) buf = realloc(buf, cap *= 2);
    }
    *len = n;
    return buf;
}

static void write_all(int fd, const void *p, size_t len) {
    const char *c = p;
    while (len > 0) {
        ssize_t w = write(fd, c, len);
        if (w <= 0) {
            if (w == -1 && errno == EINTR) continue;
            _exit(3);
        }
        c += w;
        len -= w;
    }
}

/* Run a session in a child (so a failure can't take the tool down) and
 * collect its output and saved file through a pipe. Both binaries run in
 * the same directory path, so nothing path dependent can differ. */
static result run(const kcase *c, const char *bin, const char *dir) {
    result r = {0};
    int p[2];
    if (pipe(p) == -1) {
        perror("pipe");
        exit(2);
    }
    test_remove_tree(dir);
    mkdir(dir, 0700);
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        close(p[0]);
        test_dir = dir;
        alarm(120);
        qopts o = {.bin = bin, .lenient = 1};
        kcase cc = *c;
        quip *q = kcase_run(&cc, &o);
        qkey(q, K_ESC); /* leave any prompt so Ctrl-S saves */
        qkey(q, K_ESC);
        qkey(q, K_CTRL_S);
        int flen = 0;
        char *file = qread(q, c->file, &flen);
        if (!file) flen = -1;
        char *err = qstop(q, 1);
        uint64_t olen = q->rawlen;
        write_all(p[1], &olen, sizeof(olen));
        write_all(p[1], q->raw, q->rawlen);
        write_all(p[1], &flen, sizeof(flen));
        if (flen > 0) write_all(p[1], file, flen);
        int elen = err ? (int)strlen(err) : 0;
        write_all(p[1], &elen, sizeof(elen));
        if (elen) write_all(p[1], err, elen);
        _exit(0);
    }
    close(p[1]);
    size_t n;
    char *data = slurp(p[0], &n);
    close(p[0]);
    int st;
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0 || n < sizeof(uint64_t)) {
        r.err = strdup("session failed (see output above)");
        free(data);
        return r;
    }
    size_t o = 0;
    uint64_t olen;
    memcpy(&olen, data, sizeof(olen));
    o += sizeof(olen);
    r.out = malloc(olen + 1);
    memcpy(r.out, data + o, olen);
    r.outlen = olen;
    o += olen;
    memcpy(&r.filelen, data + o, sizeof(int));
    o += sizeof(int);
    if (r.filelen > 0) {
        r.file = malloc(r.filelen);
        memcpy(r.file, data + o, r.filelen);
        o += r.filelen;
    }
    int elen;
    memcpy(&elen, data + o, sizeof(int));
    o += sizeof(int);
    if (elen) {
        r.err = malloc(elen + 1);
        memcpy(r.err, data + o, elen);
        r.err[elen] = '\0';
    }
    r.ok = 1;
    free(data);
    return r;
}

static void result_free(result *r) {
    free(r->out);
    free(r->file);
    free(r->err);
}

/* Show where two outputs first differ. */
static void show_diff(const char *a, size_t alen, const char *b, size_t blen) {
    size_t i = 0;
    while (i < alen && i < blen && a[i] == b[i]) i++;
    size_t from = i > 60 ? i - 60 : 0;
    printf("  first difference at byte %zu of %zu / %zu\n", i, alen, blen);
    for (int k = 0; k < 2; k++) {
        const char *s = k ? b : a;
        size_t len = k ? blen : alen;
        printf("  %s: ", k ? "new" : "old");
        for (size_t j = from; j < len && j < i + 60; j++) {
            unsigned char ch = (unsigned char)s[j];
            if (j == i) fputs("[>]", stdout);
            if (ch == 0x1b)
                fputs("\\e", stdout);
            else if (ch < 0x20 || ch >= 0x7f)
                printf("\\x%02x", ch);
            else
                putchar(ch);
        }
        putchar('\n');
    }
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s OLD_BIN NEW_BIN [-n cases] [-s first_seed] [-k keys]\n",
                argv[0]);
        return 2;
    }
    char oldbin[PATH_MAX], newbin[PATH_MAX];
    if (!realpath(argv[1], oldbin) || !realpath(argv[2], newbin)) {
        perror("binary");
        return 2;
    }
    int cases = 100, nkeys = 120;
    uint32_t first = 1;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc)
            cases = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-s") && i + 1 < argc)
            first = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "-k") && i + 1 < argc)
            nkeys = atoi(argv[++i]);
    }
    const char *tmp = getenv("TMPDIR");
    char base[PATH_MAX];
    path_fmt(base, sizeof(base), "%s/quip-diff-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    if (!mkdtemp(base)) {
        perror("mkdtemp");
        return 2;
    }
    char dir[PATH_MAX];
    path_fmt(dir, sizeof(dir), "%s/session", base);

    printf("comparing %s\n     with %s\n", oldbin, newbin);
    int differ = 0, failed = 0;
    for (uint32_t seed = first; seed < first + cases; seed++) {
        kcase c;
        kcase_make(&c, seed, nkeys);
        result a = run(&c, oldbin, dir);
        result b = run(&c, newbin, dir);
        if (!a.ok || !b.ok) {
            printf("seed %u: could not run (%s / %s)\n", seed, a.err ? a.err : "ok",
                   b.err ? b.err : "ok");
            failed++;
        } else if (a.outlen != b.outlen || memcmp(a.out, b.out, a.outlen) != 0) {
            printf("seed %u: terminal output differs\n", seed);
            show_diff(a.out, a.outlen, b.out, b.outlen);
            differ++;
        } else if (a.filelen != b.filelen ||
                   (a.filelen > 0 && memcmp(a.file, b.file, a.filelen) != 0)) {
            printf("seed %u: saved file differs\n", seed);
            differ++;
        }
        result_free(&a);
        result_free(&b);
        kcase_free(&c);
        if ((seed - first + 1) % 25 == 0) {
            printf("  %u/%d sessions\n", seed - first + 1, cases);
            fflush(stdout);
        }
    }
    test_remove_tree(base);
    printf("%d sessions: %d differ, %d could not run\n", cases, differ, failed);
    return differ || failed ? 1 : 0;
}
