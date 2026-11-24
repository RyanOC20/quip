/* Test runner: runs each registered test in a child process with its own
 * temporary directory and a timeout, several at a time with -j.
 *
 *   tests/run [-j N] [-v] [-l] [-k] [substring...]
 *
 * -l lists the tests, -v prints the output of passing tests too, -k keeps
 * the temporary directories. Substrings select tests by name or file. */
#include "test.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef struct test {
    const char *name;
    const char *file;
    test_fn fn;
} test;

static test *tests;
static int ntests, captests;

void test_register(const char *name, test_fn fn, const char *file) {
    if (ntests == captests) {
        captests = captests ? captests * 2 : 64;
        tests = realloc(tests, sizeof(test) * captests);
    }
    const char *slash = strrchr(file, '/');
    tests[ntests].name = name;
    tests[ntests].file = slash ? slash + 1 : file;
    tests[ntests].fn = fn;
    ntests++;
}

/* --------------------------------- Runner --------------------------------- */

typedef struct running {
    pid_t pid;
    int idx;
    char dir[PATH_MAX];
    char log[PATH_MAX];
    struct timeval start;
} running;

static int keep_dirs, verbose;

static void print_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) fwrite(buf, 1, n, stdout);
    fclose(f);
}

static int start_test(running *r, int idx, const char *base) {
    r->idx = idx;
    snprintf(r->dir, sizeof(r->dir), "%s/%s", base, tests[idx].name);
    if (mkdir(r->dir, 0700) == -1) {
        perror(r->dir);
        return -1;
    }
    snprintf(r->log, sizeof(r->log), "%s.log", r->dir);
    gettimeofday(&r->start, NULL);
    fflush(stdout);
    r->pid = fork();
    if (r->pid == -1) {
        perror("fork");
        return -1;
    }
    if (r->pid == 0) {
        int fd = open(r->log, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd != -1) {
            dup2(fd, 1);
            dup2(fd, 2);
            close(fd);
        }
        setvbuf(stdout, NULL, _IOLBF, 0);
        test_dir = r->dir;
        const char *t = getenv("QUIP_TEST_TIMEOUT");
        alarm(t ? (unsigned)atoi(t) : 120);
        signal(SIGALRM, SIG_DFL);
        tests[idx].fn();
        fflush(stdout);
        _exit(0);
    }
    return 0;
}

static double elapsed(const struct timeval *t0) {
    struct timeval now;
    gettimeofday(&now, NULL);
    return (now.tv_sec - t0->tv_sec) + (now.tv_usec - t0->tv_usec) / 1e6;
}

static int finish_test(running *r, int status) {
    int ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    const test *t = &tests[r->idx];
    printf("%s %-40s %s %.2fs\n", ok ? "PASS" : "FAIL", t->name, t->file,
           elapsed(&r->start));
    if (!ok) {
        if (WIFSIGNALED(status))
            printf("  killed by signal %d%s\n", WTERMSIG(status),
                   WTERMSIG(status) == SIGALRM ? " (timeout)" : "");
    }
    if (!ok || verbose) {
        print_file(r->log);
        fputc('\n', stdout);
    }
    if (!keep_dirs) {
        test_remove_tree(r->dir);
        unlink(r->log);
    } else if (!ok) {
        printf("  kept %s\n", r->dir);
    }
    fflush(stdout);
    return ok;
}

static int cmp_tests(const void *a, const void *b) {
    const test *x = a, *y = b;
    int c = strcmp(x->file, y->file);
    return c ? c : strcmp(x->name, y->name);
}

int main(int argc, char **argv) {
    int jobs = 1, list = 0;
    char **filters = NULL;
    int nfilters = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-j") && i + 1 < argc) jobs = atoi(argv[++i]);
        else if (!strncmp(argv[i], "-j", 2) && argv[i][2]) jobs = atoi(argv[i] + 2);
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "-l")) list = 1;
        else if (!strcmp(argv[i], "-k")) keep_dirs = 1;
        else {
            filters = argv + i;
            nfilters = argc - i;
            break;
        }
    }
    if (jobs < 1) jobs = 1;

    qsort(tests, ntests, sizeof(test), cmp_tests);
    int *selected = malloc(sizeof(int) * (ntests + 1));
    int nsel = 0;
    for (int i = 0; i < ntests; i++) {
        int want = nfilters == 0;
        for (int f = 0; f < nfilters; f++)
            if (strstr(tests[i].name, filters[f]) || strstr(tests[i].file, filters[f]))
                want = 1;
        if (want) selected[nsel++] = i;
    }
    if (list) {
        for (int i = 0; i < nsel; i++)
            printf("%-40s %s\n", tests[selected[i]].name, tests[selected[i]].file);
        return 0;
    }
    if (nsel == 0) {
        fprintf(stderr, "no tests match\n");
        return 1;
    }

    const char *tmp = getenv("TMPDIR");
    char base[PATH_MAX];
    snprintf(base, sizeof(base), "%s/quip-tests-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    if (!mkdtemp(base)) {
        perror("mkdtemp");
        return 1;
    }

    running *run = calloc(jobs, sizeof(running));
    int next = 0, active = 0, failed = 0, passed = 0;
    struct timeval t0;
    gettimeofday(&t0, NULL);
    while (next < nsel || active > 0) {
        while (active < jobs && next < nsel) {
            int slot = 0;
            while (run[slot].pid) slot++;
            if (start_test(&run[slot], selected[next++], base) == -1) return 1;
            active++;
        }
        int status;
        pid_t pid = wait(&status);
        if (pid == -1) {
            if (errno == EINTR) continue;
            perror("wait");
            return 1;
        }
        for (int s = 0; s < jobs; s++) {
            if (run[s].pid == pid) {
                if (finish_test(&run[s], status)) passed++;
                else failed++;
                run[s].pid = 0;
                active--;
                break;
            }
        }
    }
    if (!keep_dirs) test_remove_tree(base);
    printf("\n%d passed, %d failed in %.1fs\n", passed, failed, elapsed(&t0));
    return failed ? 1 : 0;
}
