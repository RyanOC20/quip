/* PTY driver for end-to-end tests. See pty.h. */
#include "pty.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "test.h"

static int session_seq;

const char *qbin(void) {
    static char path[PATH_MAX];
    if (path[0]) return path;
    const char *b = getenv("QUIP_BIN");
    if (!b || !*b) b = "./quip";
    /* quip finds its language files next to its own path, so pass it as an
     * absolute path, the way a shell would after a PATH lookup. */
    if (!realpath(b, path)) {
        fprintf(stderr, "quip binary %s: %s\n", b, strerror(errno));
        exit(2);
    }
    return path;
}

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void reply(void *ctx, const char *s, int len) {
    quip *q = ctx;
    while (len > 0) {
        ssize_t n = write(q->fd, s, len);
        if (n <= 0) {
            if (n == -1 && (errno == EINTR || errno == EAGAIN)) continue;
            return;
        }
        s += n;
        len -= n;
    }
}

static void dump_on_fail(void *ctx) {
    quip *q = ctx;
    char *d = screen_dump(&q->scr);
    printf("quip screen (%ld frames):\n%s", q->scr.frames, d);
    free(d);
    if (q->scr.nerrors) printf("screen errors: %s\n", q->scr.error);
    if (q->pid > 0 && !q->exited) {
        kill(q->pid, SIGKILL);
        waitpid(q->pid, NULL, 0);
        q->exited = 1;
    }
}

static void mkdirs(const char *path) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0700);
            *p = '/';
        }
    }
    mkdir(tmp, 0700);
}

void qwrite(quip *q, const char *name, const char *content) {
    char path[PATH_MAX];
    path_fmt(path, sizeof(path), "%s/%s", q->dir, name);
    char *slash = strrchr(path, '/');
    *slash = '\0';
    mkdirs(path);
    *slash = '/';
    FILE *f = fopen(path, "wb");
    if (!f) test_fail(__FILE__, __LINE__, "create %s: %s", path, strerror(errno));
    fwrite(content, 1, strlen(content), f);
    fclose(f);
}

const char *qpath(quip *q, const char *name) {
    static char path[PATH_MAX];
    path_fmt(path, sizeof(path), "%s/%s", q->dir, name);
    return path;
}

char *qread(quip *q, const char *name, int *len) {
    FILE *f = fopen(qpath(q, name), "rb");
    if (!f) return NULL;
    size_t cap = 4096, n = 0, r;
    char *buf = malloc(cap + 1);
    while ((r = fread(buf + n, 1, cap - n, f)) > 0) {
        n += r;
        if (n == cap) buf = realloc(buf, (cap *= 2) + 1);
    }
    fclose(f);
    buf[n] = '\0';
    if (len) *len = (int)n;
    return buf;
}

static void set_env(const char *dir) {
    char buf[PATH_MAX];
    setenv("HOME", dir, 1);
    path_fmt(buf, sizeof(buf), "%s/.config", dir);
    setenv("XDG_CONFIG_HOME", buf, 1);
    path_fmt(buf, sizeof(buf), "%s/.local/share", dir);
    setenv("XDG_DATA_HOME", buf, 1);
    path_fmt(buf, sizeof(buf), "%s/.no-data-dirs", dir);
    setenv("XDG_DATA_DIRS", buf, 1);
    unsetenv("QUIP_LANGDIR");
    setenv("TERM", "xterm-256color", 1);
    /* With AddressSanitizer on Intel macOS, the system allocator otherwise
     * prints "nano zone abandoned" to the terminal at startup. */
    setenv("MallocNanoZone", "0", 1);
    /* Sanitizer reports go to files, not the terminal, and fail the test. */
    snprintf(buf, sizeof(buf), "log_path=%s/sanitizer:detect_leaks=0:abort_on_error=0", dir);
    setenv("ASAN_OPTIONS", buf, 1);
    snprintf(buf, sizeof(buf), "log_path=%s/sanitizer:print_stacktrace=1:halt_on_error=1", dir);
    setenv("UBSAN_OPTIONS", buf, 1);
}

quip *qstart(const char *file, const char *content) { return qstart_opts(file, content, NULL); }

quip *qstart_opts(const char *file, const char *content, const qopts *o) {
    qopts def = {0};
    if (!o) o = &def;
    quip *q = calloc(1, sizeof(quip));
    const char *base = test_dir ? test_dir : "/tmp";
    path_fmt(q->dir, sizeof(q->dir), "%s/s%d", base, ++session_seq);
    mkdirs(q->dir);
    if (content) qwrite(q, file, content);
    for (int i = 0; o->files && o->files[i]; i += 2) qwrite(q, o->files[i], o->files[i + 1]);
    const char *t = getenv("QUIP_FRAME_TIMEOUT_MS");
    q->timeout_ms = t ? atoi(t) : 10000;
    q->lenient = o->lenient;

    int rows = o->rows ? o->rows : 24, cols = o->cols ? o->cols : 80;
    screen_init(&q->scr, rows, cols);
    q->scr.reply = reply;
    q->scr.reply_ctx = q;
    q->scr.answer_cursor = 1;
    q->scr.answer_clipboard = 1;

    q->fd = posix_openpt(O_RDWR | O_NOCTTY);
    if (q->fd == -1 || grantpt(q->fd) == -1 || unlockpt(q->fd) == -1)
        test_fail(__FILE__, __LINE__, "posix_openpt: %s", strerror(errno));
    char slave[PATH_MAX];
    snprintf(slave, sizeof(slave), "%s", ptsname(q->fd));

    const char *bin = o->bin ? o->bin : qbin();
    const char *argv[32];
    int argc = 0;
    argv[argc++] = bin;
    if (o->argv) {
        for (int i = 0; o->argv[i] && argc < 31; i++) argv[argc++] = o->argv[i];
    } else if (file) {
        argv[argc++] = file;
    }
    argv[argc] = NULL;

    fflush(stdout);
    q->pid = fork();
    if (q->pid == -1) test_fail(__FILE__, __LINE__, "fork: %s", strerror(errno));
    if (q->pid == 0) {
        setsid();
        int s = open(slave, O_RDWR);
        if (s == -1) _exit(126);
#ifdef TIOCSCTTY
        ioctl(s, TIOCSCTTY, 0);
#endif
        /* Size the terminal before quip starts, or it asks the terminal for
         * its size and waits for an answer. */
        struct winsize ws = {0};
        if (!o->no_winsize) {
            ws.ws_row = rows;
            ws.ws_col = cols;
        }
        ioctl(s, TIOCSWINSZ, &ws);
        dup2(s, 0);
        dup2(s, 1);
        dup2(s, 2);
        if (s > 2) close(s);
        close(q->fd);
        if (chdir(q->dir) == -1) _exit(126);
        set_env(q->dir);
        execv(bin, (char *const *)argv);
        _exit(127);
    }
    fcntl(q->fd, F_SETFL, fcntl(q->fd, F_GETFL) | O_NONBLOCK);
    test_on_fail(dump_on_fail, q);
    if (!o->no_wait) qwait_frames(q, 1);
    return q;
}

static void check_alive(quip *q);

/* Read whatever quip has written within 'ms'. Returns 0 once quip is gone
 * and its output fully read. */
static int pump_once(quip *q, int ms) {
    struct pollfd p = {q->fd, POLLIN, 0};
    int r = poll(&p, 1, ms);
    if (r <= 0) return 1;
    char buf[65536];
    ssize_t n = read(q->fd, buf, sizeof(buf));
    if (n > 0) {
        if (q->rawlen + n > q->rawcap) {
            while (q->rawlen + n > q->rawcap) q->rawcap = q->rawcap ? q->rawcap * 2 : 65536;
            q->raw = realloc(q->raw, q->rawcap);
        }
        memcpy(q->raw + q->rawlen, buf, n);
        q->rawlen += n;
        screen_feed(&q->scr, buf, (int)n);
        return 1;
    }
    if (n == -1 && (errno == EAGAIN || errno == EINTR)) return 1;
    return 0; /* EOF or EIO: the slave side is closed. */
}

void qpump(quip *q, int ms) {
    long end = now_ms() + ms;
    while (now_ms() < end) {
        if (!pump_once(q, (int)(end - now_ms()))) break;
    }
}

static void reap(quip *q, int block) {
    if (q->exited || q->pid <= 0) return;
    int st;
    struct rusage ru;
    pid_t r = wait4(q->pid, &st, block ? 0 : WNOHANG, &ru);
    if (r == q->pid) {
        q->exited = 1;
        q->status = st;
#ifdef __APPLE__
        q->maxrss_kb = ru.ru_maxrss / 1024; /* bytes on macOS */
#else
        q->maxrss_kb = ru.ru_maxrss; /* kilobytes on Linux */
#endif
    }
}

/* The sanitizer report written by quip, if any. Caller frees. */
static char *sanitizer_report(quip *q) {
    DIR *d = opendir(q->dir);
    if (!d) return NULL;
    struct dirent *e;
    char *report = NULL;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "sanitizer", 9) == 0) {
            report = qread(q, e->d_name, NULL);
            break;
        }
    }
    closedir(d);
    return report;
}

static void check_alive(quip *q) {
    reap(q, 0);
    if (!q->exited) return;
    qpump(q, 50);
    char *rep = sanitizer_report(q);
    if (rep) printf("%s\n", rep);
    if (WIFSIGNALED(q->status))
        test_fail(__FILE__, __LINE__, "quip died from signal %d", WTERMSIG(q->status));
    test_fail(__FILE__, __LINE__, "quip exited unexpectedly with status %d",
              WEXITSTATUS(q->status));
}

void qwait_frames(quip *q, long frames) {
    long deadline = now_ms() + q->timeout_ms;
    while (q->scr.frames < frames) {
        if (now_ms() > deadline)
            test_fail(__FILE__, __LINE__, "timed out waiting for frame %ld (have %ld)", frames,
                      q->scr.frames);
        if (!pump_once(q, 20)) check_alive(q);
        check_alive(q);
    }
    if (q->scr.nerrors && !q->lenient)
        test_fail(__FILE__, __LINE__, "unexpected terminal output: %s", q->scr.error);
}

void qsend(quip *q, const char *bytes, int len, int frames) {
    long target = q->scr.frames + frames;
    reply(q, bytes, len);
    qwait_frames(q, target);
}

void qkey(quip *q, const char *seq) { qsend(q, seq, (int)strlen(seq), 1); }

/* Length of the well-formed UTF-8 character at s (n bytes left), or 0. */
static int utf8_len(const unsigned char *s, int n) {
    int len = s[0] < 0x80                    ? 1
              : s[0] >= 0xc2 && s[0] <= 0xdf ? 2
              : s[0] >= 0xe0 && s[0] <= 0xef ? 3
              : s[0] >= 0xf0 && s[0] <= 0xf4 ? 4
                                             : 0;
    if (len == 0 || len > n) return 0;
    for (int i = 1; i < len; i++)
        if ((s[i] & 0xc0) != 0x80) return 0;
    if (len == 3 && s[0] == 0xe0 && s[1] < 0xa0) return 0;  /* overlong */
    if (len == 3 && s[0] == 0xed && s[1] >= 0xa0) return 0; /* surrogate */
    if (len == 4 && s[0] == 0xf0 && s[1] < 0x90) return 0;  /* overlong */
    if (len == 4 && s[0] == 0xf4 && s[1] >= 0x90) return 0; /* past U+10FFFF */
    return len;
}

int qtext_frames(const char *text, int n) {
    const unsigned char *s = (const unsigned char *)text;
    int frames = 0;
    for (int i = 0; i < n; frames++) {
        int len = utf8_len(s + i, n - i);
        i += len ? len : 1;
    }
    return frames;
}

void qtype(quip *q, const char *text) {
    int n = (int)strlen(text);
    char *buf = malloc(n);
    for (int i = 0; i < n; i++) buf[i] = text[i] == '\n' ? '\r' : text[i];
    qsend(q, buf, n, qtext_frames(buf, n));
    free(buf);
}

static void mouse(quip *q, int button, int row, int col) {
    char seq[6] = {'\x1b', '[', 'M', (char)(32 + button), (char)(32 + col), (char)(32 + row)};
    qsend(q, seq, 6, 1);
}

void qpress(quip *q, int row, int col) { mouse(q, 0, row, col); }
void qrelease(quip *q, int row, int col) { mouse(q, 3, row, col); }
void qdrag(quip *q, int row, int col) { mouse(q, 32, row, col); }
void qwheel(quip *q, int up, int row, int col) { mouse(q, up ? 64 : 65, row, col); }
void qclick(quip *q, int row, int col) {
    qpress(q, row, col);
    qrelease(q, row, col);
}

void qresize(quip *q, int rows, int cols) {
    long target = q->scr.frames + 1;
    screen_resize(&q->scr, rows, cols);
    /* Setting the size on the master signals the slave's foreground
     * process group with SIGWINCH. */
    struct winsize ws = {0};
    ws.ws_row = rows;
    ws.ws_col = cols;
    if (ioctl(q->fd, TIOCSWINSZ, &ws) == -1)
        test_fail(__FILE__, __LINE__, "TIOCSWINSZ: %s", strerror(errno));
    qwait_frames(q, target);
}

const char *qrow(quip *q, int row) {
    static char buf[4096];
    return screen_row_text(&q->scr, row - 1, buf, sizeof(buf));
}

const cell *qcell(quip *q, int row, int col) { return screen_cell(&q->scr, row - 1, col - 1); }

const char *qline(quip *q, int row) {
    static char buf[4096];
    /* The gutter is drawn gray, or bold on the cursor's line; text never
     * is, so the gutter ends at the first cell without that style. */
    int x = 0;
    while (x < q->scr.cols) {
        const cell *c = screen_cell(&q->scr, row - 1, x);
        if (c->fg != 90 && !(c->attr & ATTR_BOLD)) break;
        x++;
    }
    if (x == 0) {
        /* Past the end of the file: "~" and padding, no style. */
        screen_row_text(&q->scr, row - 1, buf, sizeof(buf));
        return buf;
    }
    screen_span_text(&q->scr, row - 1, x, q->scr.cols, buf, sizeof(buf));
    int n = (int)strlen(buf);
    while (n > 0 && buf[n - 1] == ' ') buf[--n] = '\0';
    return buf;
}

const char *qstatus(quip *q) { return qrow(q, q->scr.rows - 1); }
const char *qmessage(quip *q) { return qrow(q, q->scr.rows); }

int qwait_exit(quip *q) {
    long deadline = now_ms() + q->timeout_ms;
    while (!q->exited) {
        if (now_ms() > deadline)
            test_fail(__FILE__, __LINE__, "timed out waiting for quip to exit");
        pump_once(q, 20);
        reap(q, 0);
    }
    qpump(q, 20);
    return q->status;
}

char *qstop(quip *q, int quit) {
    char msg[2048] = "";
    if (quit) {
        /* A modified file needs Ctrl-Q several times. */
        for (int i = 0; i < 4 && !q->exited; i++) {
            reply(q, "\x11", 1);
            long end = now_ms() + 2000;
            long frames = q->scr.frames;
            while (!q->exited && q->scr.frames == frames && now_ms() < end) {
                pump_once(q, 20);
                reap(q, 0);
            }
        }
        long end = now_ms() + q->timeout_ms;
        while (!q->exited && now_ms() < end) {
            pump_once(q, 20);
            reap(q, 0);
        }
        if (!q->exited) snprintf(msg, sizeof(msg), "quip did not quit");
    }
    if (!q->exited) {
        kill(q->pid, SIGKILL);
        reap(q, 1);
    } else if (WIFSIGNALED(q->status)) {
        snprintf(msg, sizeof(msg), "quip died from signal %d", WTERMSIG(q->status));
    } else if (WEXITSTATUS(q->status) != 0) {
        snprintf(msg, sizeof(msg), "quip exited with status %d", WEXITSTATUS(q->status));
    }
    qpump(q, 0);
    if (q->scr.nerrors && !q->lenient && !msg[0])
        snprintf(msg, sizeof(msg), "unexpected terminal output: %s", q->scr.error);
    char *rep = sanitizer_report(q);
    if (rep) {
        size_t l = strlen(msg);
        snprintf(msg + l, sizeof(msg) - l, "%ssanitizer report:\n%.1500s", l ? "\n" : "", rep);
        free(rep);
    }
    return msg[0] ? strdup(msg) : NULL;
}

void qfree(quip *q) {
    test_on_fail_remove(q);
    if (!q->exited && q->pid > 0) {
        kill(q->pid, SIGKILL);
        reap(q, 1);
    }
    close(q->fd);
    screen_free(&q->scr);
    free(q->raw);
    free(q);
}

static void stop_checked(quip *q, int quit) {
    char *err = qstop(q, quit);
    if (err) test_fail(__FILE__, __LINE__, "%s", err);
    if (quit) {
        /* Quitting must leave the alternate screen and the terminal modes. */
        if (q->scr.alt) test_fail(__FILE__, __LINE__, "still on the alternate screen");
        if (q->scr.modes) test_fail(__FILE__, __LINE__, "terminal modes left on: %d", q->scr.modes);
    }
    qfree(q);
}

void qquit(quip *q) { stop_checked(q, 1); }
void qkill(quip *q) { stop_checked(q, 0); }
