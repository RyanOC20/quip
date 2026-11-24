/* Random keystroke fuzzer. Runs random editing sessions (see keygen.c)
 * against quip, normally the sanitizer build, and reports any crash,
 * sanitizer error, hang or unexpected terminal output. Each failure is
 * minimized to the fewest keys that still fail the same way and printed as
 * an end-to-end test, ready to paste into tests/e2e/.
 *
 *   tests/fuzz [-n cases] [-s first_seed] [-k keys] [-j jobs] [-t seconds]
 *
 * The same seed always replays the same session. */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include "keygen.h"
#include "pty.h"
#include "test.h"

static char base[PATH_MAX];
static int run_counter;

static double now_s(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return strdup("");
    size_t cap = 4096, n = 0, r;
    char *buf = malloc(cap + 1);
    while ((r = fread(buf + n, 1, cap - n, f)) > 0) {
        n += r;
        if (n == cap) buf = realloc(buf, (cap *= 2) + 1);
    }
    fclose(f);
    buf[n] = '\0';
    return buf;
}

/* Start a run of 'c' in a child process. Its output goes to 'log'. */
static pid_t start_run(const kcase *c, char *log, size_t logsize) {
    char dir[PATH_MAX];
    int id = ++run_counter;
    snprintf(dir, sizeof(dir), "%s/r%d", base, id);
    snprintf(log, logsize, "%s/r%d.log", base, id);
    mkdir(dir, 0700);
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        int fd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd != -1) {
            dup2(fd, 1);
            dup2(fd, 2);
            close(fd);
        }
        setvbuf(stdout, NULL, _IOLBF, 0);
        test_dir = dir;
        alarm(120);
        quip *q = kcase_run(c, NULL);
        qquit(q);
        _exit(0);
    }
    return pid;
}

/* Wait for a run; returns its log text when it failed, else NULL. */
static char *finish_run(pid_t pid, const char *log) {
    int st;
    waitpid(pid, &st, 0);
    char *text = NULL;
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        text = read_file(log);
        if (WIFSIGNALED(st)) {
            char extra[64];
            snprintf(extra, sizeof(extra), "\nharness killed by signal %d\n",
                     WTERMSIG(st));
            text = realloc(text, strlen(text) + sizeof(extra));
            strcat(text, extra);
        }
    }
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%.*s", (int)(strlen(log) - 4), log);
    test_remove_tree(dir);
    unlink(log);
    return text;
}

static char *run_case(const kcase *c) {
    char log[PATH_MAX];
    pid_t pid = start_run(c, log, sizeof(log));
    return finish_run(pid, log);
}

/* What kind of failure a log shows, with numbers and addresses blanked so
 * that the same bug found with fewer keys still matches. */
static void signature(const char *log, char *out, size_t size) {
    const char *keys[] = {"ERROR: AddressSanitizer:", "runtime error:",
                          "unexpected terminal output:", "died from signal",
                          "timed out", "exited", "did not quit",
                          "harness killed", NULL};
    const char *line = NULL;
    for (int i = 0; keys[i] && !line; i++) line = strstr(log, keys[i]);
    if (!line) line = log;
    size_t o = 0;
    for (; *line && *line != '\n' && o + 1 < size; line++) {
        char ch = *line;
        if (isdigit((unsigned char)ch)) {
            if (o == 0 || out[o - 1] != '#') out[o++] = '#';
            continue;
        }
        out[o++] = ch;
    }
    out[o] = '\0';
    /* Keep sanitizer messages up to the error class only. */
    char *on = strstr(out, " on address");
    if (on) *on = '\0';
}

static int fails_like(const kcase *c, const char *want) {
    char *log = run_case(c);
    if (!log) return 0;
    char sig[256];
    signature(log, sig, sizeof(sig));
    free(log);
    return strcmp(sig, want) == 0;
}

/* Delta debugging (ddmin) over the keys of 'c'. */
static void minimize(kcase *c, const char *sig) {
    int n = 2;
    while (c->nkeys >= 2) {
        int chunk = (c->nkeys + n - 1) / n;
        int reduced = 0;
        for (int start = 0; start < c->nkeys; start += chunk) {
            char *keep = malloc(c->nkeys);
            for (int i = 0; i < c->nkeys; i++)
                keep[i] = !(i >= start && i < start + chunk);
            kcase t;
            kcase_subset(c, keep, &t);
            free(keep);
            if (fails_like(&t, sig)) {
                kcase_free(c);
                *c = t;
                n = n > 2 ? n - 1 : 2;
                reduced = 1;
                break;
            }
            kcase_free(&t);
        }
        if (!reduced) {
            if (n >= c->nkeys) break;
            n = n * 2 < c->nkeys ? n * 2 : c->nkeys;
        }
    }
    /* Then try dropping single keys, which ddmin's chunks can miss. */
    for (int i = c->nkeys - 1; i >= 0 && c->nkeys > 1; i--) {
        char *keep = malloc(c->nkeys);
        memset(keep, 1, c->nkeys);
        keep[i] = 0;
        kcase t;
        kcase_subset(c, keep, &t);
        free(keep);
        if (fails_like(&t, sig)) {
            kcase_free(c);
            *c = t;
        } else {
            kcase_free(&t);
        }
    }
}

typedef struct slot {
    pid_t pid;
    uint32_t seed;
    char log[PATH_MAX];
} slot;

typedef struct failure {
    uint32_t seed;
    char sig[256];
    char *log;
} failure;

int main(int argc, char **argv) {
    int cases = 200, nkeys = 150, jobs = 4;
    uint32_t first = 1;
    double limit = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) cases = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) first = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "-k") && i + 1 < argc) nkeys = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-j") && i + 1 < argc) jobs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) limit = atof(argv[++i]);
        else {
            fprintf(stderr, "usage: %s [-n cases] [-s first_seed] [-k keys] "
                            "[-j jobs] [-t seconds]\n", argv[0]);
            return 2;
        }
    }
    if (jobs < 1) jobs = 1;
    printf("fuzzing %s: seeds %u..%u, %d keys each\n", qbin(), first,
           first + cases - 1, nkeys);
    fflush(stdout);

    const char *tmp = getenv("TMPDIR");
    snprintf(base, sizeof(base), "%s/quip-fuzz-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    if (!mkdtemp(base)) {
        perror("mkdtemp");
        return 2;
    }

    /* Phase 1: run the sessions, several at a time. */
    double t0 = now_s();
    slot *slots = calloc(jobs, sizeof(slot));
    failure *fails = NULL;
    int nfails = 0, done = 0, active = 0;
    uint32_t next_seed = first;
    for (;;) {
        int more = next_seed < first + cases && (limit <= 0 || now_s() - t0 < limit);
        while (more && active < jobs) {
            int s = 0;
            while (slots[s].pid) s++;
            kcase c;
            kcase_make(&c, next_seed, nkeys);
            slots[s].seed = next_seed++;
            slots[s].pid = start_run(&c, slots[s].log, sizeof(slots[s].log));
            kcase_free(&c);
            active++;
            more = next_seed < first + cases && (limit <= 0 || now_s() - t0 < limit);
        }
        if (active == 0) break;
        int st;
        pid_t pid = wait(&st);
        if (pid == -1) {
            if (errno == EINTR) continue;
            break;
        }
        int s = 0;
        while (s < jobs && slots[s].pid != pid) s++;
        if (s == jobs) continue;
        active--;
        slots[s].pid = 0;
        done++;
        char *log = NULL;
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
            log = read_file(slots[s].log);
            if (WIFSIGNALED(st)) {
                char extra[64];
                snprintf(extra, sizeof(extra), "\nharness killed by signal %d\n",
                         WTERMSIG(st));
                log = realloc(log, strlen(log) + sizeof(extra));
                strcat(log, extra);
            }
        }
        char dir[PATH_MAX];
        snprintf(dir, sizeof(dir), "%.*s", (int)(strlen(slots[s].log) - 4), slots[s].log);
        test_remove_tree(dir);
        unlink(slots[s].log);
        if (!log) continue;
        fails = realloc(fails, sizeof(failure) * (nfails + 1));
        failure *f = &fails[nfails++];
        f->seed = slots[s].seed;
        f->log = log;
        signature(log, f->sig, sizeof(f->sig));
        printf("seed %u fails: %s\n", f->seed, f->sig);
        fflush(stdout);
    }
    printf("%d sessions, %d failing in %.1fs\n", done, nfails, now_s() - t0);

    /* Phase 2: minimize the lowest seed of each distinct failure. */
    int distinct = 0;
    for (int i = 0; i < nfails; i++) {
        int dup = 0;
        for (int j = 0; j < nfails; j++)
            if (!strcmp(fails[j].sig, fails[i].sig) &&
                (fails[j].seed < fails[i].seed || (fails[j].seed == fails[i].seed && j < i)))
                dup = 1;
        if (dup) continue;
        distinct++;
        printf("\n=== %s (seed %u)\n%s\n", fails[i].sig, fails[i].seed, fails[i].log);
        kcase c;
        kcase_make(&c, fails[i].seed, nkeys);
        printf("minimizing %d keys...\n", c.nkeys);
        fflush(stdout);
        if (!fails_like(&c, fails[i].sig)) {
            printf("does not fail again when run alone (flaky?)\n");
        } else {
            minimize(&c, fails[i].sig);
            printf("minimal reproduction (%d keys):\n\n", c.nkeys);
            char name[64];
            snprintf(name, sizeof(name), "fuzz_seed_%u", fails[i].seed);
            kcase_print_test(&c, name);
            printf("\n");
        }
        kcase_free(&c);
        fflush(stdout);
    }
    test_remove_tree(base);
    if (nfails) printf("%d distinct failures\n", distinct);
    return nfails ? 1 : 0;
}
