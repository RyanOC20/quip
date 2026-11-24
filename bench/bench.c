/* Performance benchmark: drives quip through a pseudo terminal on large
 * generated files and measures what a user waits for.
 *
 *   bench/bench BIN            measure one binary
 *   bench/bench OLD NEW [-r runs] [-p percent]
 *                              compare two binaries, interleaving their runs
 *                              on the same machine; exits 1 if NEW is slower
 *                              or bigger than allowed
 *
 * Each figure is the median of several runs. */
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../tests/keys.h"
#include "../tests/pty.h"
#include "../tests/test.h"

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static char *big_c(int lines) {
    size_t cap = (size_t)lines * 64 + 64, o = 0;
    char *buf = malloc(cap);
    for (int i = 0; i < lines; i++) {
        o += snprintf(buf + o, cap - o,
                      "int f%d(int x) { return x * %d; } // note %d\n", i, i % 97, i);
    }
    return buf;
}

typedef struct metric {
    const char *name, *unit;
    int lower_is_better;
    double floor;   /* Differences below this are noise. */
} metric;

enum { M_LOAD, M_SCROLL, M_TYPE, M_CASCADE, M_BRACKET, M_RSS, M_SIZE, NMETRICS };
static const metric metrics[NMETRICS] = {
    {"load 200k lines", "ms", 1, 2.0},
    {"page down, 12MB file", "ms/frame", 1, 0.2},
    {"type a key, 200k lines", "ms/key", 1, 0.2},
    {"open a block comment, 200k lines", "ms", 1, 2.0},
    {"unmatched bracket scan, 12MB", "ms/frame", 1, 0.5},
    {"peak memory, 12MB file", "MB", 1, 1.0},
    {"stripped binary", "bytes", 1, 0},
};

/* The input files, written once before any run so writing them is never
 * part of a timing. */
static char path200k[PATH_MAX], path12mb[PATH_MAX], pathbracket[PATH_MAX];

/* Start 'bin' on the file at 'path'. */
static quip *open_file(const char *bin, const char *path) {
    const char *argv[] = {path, NULL};
    qopts o = {.bin = bin, .rows = 40, .cols = 120, .argv = argv};
    return qstart_opts(NULL, NULL, &o);
}

static void write_file(char *path, const char *dir, const char *name, const char *s) {
    snprintf(path, PATH_MAX, "%s/%s", dir, name);
    FILE *f = fopen(path, "w");
    if (!f || fputs(s, f) == EOF || fclose(f) == EOF) {
        perror(path);
        exit(2);
    }
}

/* One run of every measurement for 'bin'; results go to v[]. Runs in a
 * child process so the harness's failure exits stay contained. */
static void measure_child(const char *bin, double *v) {
    quip *q;
    double t;

    t = now_ms();
    q = open_file(bin, path200k);
    v[M_LOAD] = now_ms() - t;

    /* Typing in the middle of a line: re-highlights one row. */
    for (int i = 0; i < 10; i++) qkey(q, K_DOWN);
    t = now_ms();
    for (int i = 0; i < 50; i++) qkey(q, "x");
    v[M_TYPE] = (now_ms() - t) / 50;

    /* "/" then "*" at the top: every row after it becomes a comment. */
    qkey(q, K_CTRL_UP);
    qkey(q, "/");
    t = now_ms();
    qkey(q, "*");
    v[M_CASCADE] = now_ms() - t;
    qkill(q);

    q = open_file(bin, path12mb);
    t = now_ms();
    for (int i = 0; i < 40; i++) qkey(q, K_PGDN);
    v[M_SCROLL] = (now_ms() - t) / 40;
    char *err = qstop(q, 1);
    free(err);
    v[M_RSS] = q->maxrss_kb / 1024.0;
    qfree(q);

    /* A '{' that is never closed: each frame scans the whole file for its
     * partner. */
    q = open_file(bin, pathbracket);
    t = now_ms();
    for (int i = 0; i < 10; i++) qkey(q, i % 2 ? K_LEFT : K_RIGHT);
    v[M_BRACKET] = (now_ms() - t) / 10;
    qkill(q);
}

static double binary_size(const char *bin, const char *dir) {
    char out[PATH_MAX];
    snprintf(out, sizeof(out), "%s/stripped", dir);
    pid_t pid = fork();
    if (pid == 0) {
        execlp("strip", "strip", "-o", out, bin, (char *)NULL);
        _exit(127);
    }
    int st;
    waitpid(pid, &st, 0);
    struct stat sb;
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0 || stat(out, &sb) == -1) {
        /* strip without -o (old toolchains): measure as given. */
        if (stat(bin, &sb) == -1) return 0;
    }
    unlink(out);
    return (double)sb.st_size;
}

static int measure(const char *bin, const char *dir, double *v) {
    int p[2];
    if (pipe(p) == -1) return -1;
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        close(p[0]);
        test_dir = dir;
        measure_child(bin, v);
        if (write(p[1], v, sizeof(double) * NMETRICS) < 0) _exit(1);
        _exit(0);
    }
    close(p[1]);
    ssize_t n = read(p[0], v, sizeof(double) * NMETRICS);
    close(p[0]);
    int st;
    waitpid(pid, &st, 0);
    if (n != (ssize_t)(sizeof(double) * NMETRICS) || !WIFEXITED(st) ||
        WEXITSTATUS(st) != 0) {
        fprintf(stderr, "benchmark run of %s failed\n", bin);
        return -1;
    }
    v[M_SIZE] = binary_size(bin, dir);
    return 0;
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static double median(double *xs, int n) {
    qsort(xs, n, sizeof(double), cmp_double);
    return n % 2 ? xs[n / 2] : (xs[n / 2 - 1] + xs[n / 2]) / 2;
}

int main(int argc, char **argv) {
    const char *bins[2] = {NULL, NULL};
    int nbins = 0, runs = 7;
    double pct = 10, size_pct = 15;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-r") && i + 1 < argc) runs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) pct = atof(argv[++i]);
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) size_pct = atof(argv[++i]);
        else if (nbins < 2) bins[nbins++] = argv[i];
    }
    if (nbins == 0 || runs < 1) {
        fprintf(stderr, "usage: %s BIN | OLD NEW [-r runs] [-p percent] [-s size_percent]\n",
                argv[0]);
        return 2;
    }
    char paths[2][PATH_MAX];
    for (int b = 0; b < nbins; b++) {
        if (!realpath(bins[b], paths[b])) {
            perror(bins[b]);
            return 2;
        }
        bins[b] = paths[b];
    }

    const char *tmp = getenv("TMPDIR");
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/quip-tests-bench-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 2;
    }
    char *file200k = big_c(200000), *file12mb = big_c(240000);
    size_t blen = strlen(file12mb);
    char *bracket12mb = malloc(blen + 3);
    bracket12mb[0] = '{';
    bracket12mb[1] = '\n';
    memcpy(bracket12mb + 2, file12mb, blen + 1);
    write_file(path200k, dir, "big.c", file200k);
    write_file(path12mb, dir, "big12.c", file12mb);
    write_file(pathbracket, dir, "br.c", bracket12mb);
    free(file200k);
    free(file12mb);
    free(bracket12mb);

    double *vals = calloc((size_t)nbins * runs * NMETRICS, sizeof(double));
    for (int r = 0; r < runs; r++) {
        for (int b = 0; b < nbins; b++) {
            /* Alternate the order so drift affects both alike. */
            int bb = (r % 2) ? nbins - 1 - b : b;
            double *v = vals + ((size_t)bb * runs + r) * NMETRICS;
            if (measure(bins[bb], dir, v) == -1) {
                test_remove_tree(dir);
                return 2;
            }
        }
        fprintf(stderr, "run %d/%d done\n", r + 1, runs);
    }
    test_remove_tree(dir);

    double med[2][NMETRICS];
    double *col = malloc(sizeof(double) * runs);
    for (int b = 0; b < nbins; b++) {
        for (int m = 0; m < NMETRICS; m++) {
            for (int r = 0; r < runs; r++)
                col[r] = vals[((size_t)b * runs + r) * NMETRICS + m];
            med[b][m] = median(col, runs);
        }
    }

    int worse = 0;
    if (nbins == 1) {
        printf("%-36s %12s\n", bins[0], "");
        for (int m = 0; m < NMETRICS; m++)
            printf("%-36s %12.2f %s\n", metrics[m].name, med[0][m], metrics[m].unit);
    } else {
        printf("%-36s %12s %12s %8s\n", "", "old", "new", "change");
        for (int m = 0; m < NMETRICS; m++) {
            double a = med[0][m], b = med[1][m];
            double change = a > 0 ? (b - a) / a * 100 : 0;
            double limit = m == M_SIZE ? size_pct : pct;
            int bad = change > limit && (b - a) > metrics[m].floor;
            worse |= bad;
            printf("%-36s %12.2f %12.2f %+7.1f%% %s%s\n", metrics[m].name, a, b,
                   change, metrics[m].unit, bad ? "  <-- regression" : "");
        }
    }
    return worse ? 1 : 0;
}
