/* The test framework's failure reporting, shared by the test runner, the
 * fuzzer, the differential tester and the benchmark. See test.h. */
#include "test.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

const char *test_dir;

/* ------------------------------ Failure hooks ----------------------------- */

typedef struct hook {
    void (*fn)(void *);
    void *ctx;
} hook;
static hook hooks[16];
static int nhooks;

void test_on_fail(void (*fn)(void *), void *ctx) {
    if (nhooks < 16) hooks[nhooks++] = (hook){fn, ctx};
}

void test_on_fail_remove(void *ctx) {
    for (int i = 0; i < nhooks; i++) {
        if (hooks[i].ctx == ctx) {
            memmove(hooks + i, hooks + i + 1, sizeof(hook) * (nhooks - i - 1));
            nhooks--;
            return;
        }
    }
}

void test_fail(const char *file, int line, const char *fmt, ...) {
    va_list ap;
    fprintf(stdout, "%s:%d: ", file, line);
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
    /* Newest first; a hook may fail again, so take each one off first. */
    while (nhooks > 0) {
        hook h = hooks[--nhooks];
        h.fn(h.ctx);
    }
    fflush(stdout);
    _exit(1);
}

static void print_quoted(const char *s, int len) {
    fputc('"', stdout);
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\n')
            fputs("\\n", stdout);
        else if (c == '\r')
            fputs("\\r", stdout);
        else if (c == '\t')
            fputs("\\t", stdout);
        else if (c == '"' || c == '\\')
            printf("\\%c", c);
        else if (c < 0x20 || c == 0x7f)
            printf("\\x%02x", c);
        else
            fputc(c, stdout);
    }
    fputc('"', stdout);
}

void test_check_mem(const char *file, int line, const char *expr, const char *got, int gotlen,
                    const char *want, int wantlen) {
    if (got && gotlen == wantlen && memcmp(got, want, wantlen) == 0) return;
    printf("%s:%d: %s\n  got:  ", file, line, expr);
    if (got)
        print_quoted(got, gotlen);
    else
        fputs("NULL", stdout);
    fputs("\n  want: ", stdout);
    print_quoted(want, wantlen);
    fputc('\n', stdout);
    test_fail(file, line, "%s differs", expr);
}

void test_check_str(const char *file, int line, const char *expr, const char *got,
                    const char *want) {
    test_check_mem(file, line, expr, got, got ? (int)strlen(got) : 0, want, (int)strlen(want));
}

void test_remove_tree(const char *path) {
    static const char *const roots[] = {"/quip-tests-", "/quip-fuzz-", "/quip-diff-"};
    int ok = 0;
    for (int i = 0; i < 3; i++) {
        const char *p = strstr(path, roots[i]);
        /* The root's random suffix must follow, so the root itself or a
         * parent of it never matches. */
        if (p && strlen(p) > strlen(roots[i]) + 5) ok = 1;
    }
    if (!ok || strstr(path, "..")) {
        fprintf(stderr, "refusing to remove %s\n", path);
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        execlp("rm", "rm", "-rf", "--", path, (char *)NULL);
        _exit(127);
    }
    if (pid > 0) waitpid(pid, NULL, 0);
}
