/* Saving: the file on disk is either the old text or the new text, never a
 * mix, and saving keeps what the file is: its mode, its links. */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../keys.h"
#include "../pty.h"
#include "../test.h"

static int modified(quip *q) { return strstr(qstatus(q), "(modified)") != NULL; }

/* Names in the session directory other than '.' and '..', joined by spaces
 * in sorted order (static buffer). */
static const char *dir_listing(quip *q) {
    static char out[1024];
    char *names[64];
    int n = 0;
    DIR *d = opendir(q->dir);
    struct dirent *e;
    while (d && (e = readdir(d)) && n < 64)
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) names[n++] = strdup(e->d_name);
    if (d) closedir(d);
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && strcmp(names[j - 1], names[j]) > 0; j--) {
            char *t = names[j];
            names[j] = names[j - 1];
            names[j - 1] = t;
        }
    out[0] = '\0';
    for (int i = 0; i < n; i++) {
        if (i) strcat(out, " ");
        strncat(out, names[i], sizeof(out) - strlen(out) - 2);
        free(names[i]);
    }
    return out;
}

static char *numbered_lines(int n) {
    char *text = malloc((size_t)n * 32 + 1);
    int o = 0;
    for (int i = 1; i <= n; i++) o += sprintf(text + o, "line %05d of the file\n", i);
    return text;
}

TEST(save_cut_short_keeps_the_old_file) {
    /* A write that cannot finish, as when the disk fills up or quip is
     * killed: past the size limit the kernel refuses to write. The file
     * must still hold its old text. */
    char *text = numbered_lines(2000); /* 44 KB */
    qopts o = {.fsize_limit = 16384};
    quip *q = qstart_opts("a.txt", text, &o);
    qkey(q, K_END);
    qkey(q, K_BACKSPACE); /* a shorter file, so that it fits the old one */
    qkey(q, K_CTRL_S);
    CHECK(strstr(qmessage(q), "Can't save") != NULL);
    CHECK(modified(q));
    char *disk = qread(q, "a.txt", NULL);
    CHECK_STR(disk, text);
    CHECK_STR(dir_listing(q), "a.txt");
    free(disk);
    free(text);
    qquit(q);
}

TEST(save_leaves_no_other_files) {
    quip *q = qstart("a.txt", "x\n");
    qtype(q, "y");
    qkey(q, K_CTRL_S);
    CHECK(strstr(qmessage(q), "3 bytes written") != NULL);
    CHECK_STR(dir_listing(q), "a.txt");
    char *disk = qread(q, "a.txt", NULL);
    CHECK_STR(disk, "yx\n");
    free(disk);
    qquit(q);
}

static void make_private(quip *q) { CHECK(chmod(qpath(q, "a.txt"), 0600) == 0); }

TEST(save_keeps_the_file_mode) {
    qopts o = {.setup = make_private};
    quip *q = qstart_opts("a.txt", "x\n", &o);
    qtype(q, "y");
    qkey(q, K_CTRL_S);
    struct stat st;
    CHECK(stat(qpath(q, "a.txt"), &st) == 0);
    CHECK_INT(st.st_mode & 07777, 0600);
    qquit(q);
}

static void make_symlink(quip *q) { CHECK(symlink("target.txt", qpath(q, "link.txt")) == 0); }

TEST(save_through_a_symlink_writes_the_target) {
    const char *const files[] = {"target.txt", "x\n", NULL};
    qopts o = {.files = files, .setup = make_symlink};
    quip *q = qstart_opts("link.txt", NULL, &o);
    CHECK_STR(qline(q, 1), "x");
    qtype(q, "y");
    qkey(q, K_CTRL_S);
    struct stat st;
    CHECK(lstat(qpath(q, "link.txt"), &st) == 0);
    CHECK(S_ISLNK(st.st_mode));
    char *disk = qread(q, "target.txt", NULL);
    CHECK_STR(disk, "yx\n");
    free(disk);
    CHECK_STR(dir_listing(q), "link.txt target.txt");
    qquit(q);
}

static void make_hard_link(quip *q) {
    char a[1024];
    snprintf(a, sizeof(a), "%s", qpath(q, "a.txt"));
    CHECK(link(a, qpath(q, "b.txt")) == 0);
}

TEST(save_keeps_hard_links) {
    qopts o = {.setup = make_hard_link};
    quip *q = qstart_opts("a.txt", "x\n", &o);
    qtype(q, "y");
    qkey(q, K_CTRL_S);
    char *disk = qread(q, "b.txt", NULL);
    CHECK_STR(disk, "yx\n");
    free(disk);
    qquit(q);
}

static void make_dir_read_only(quip *q) { CHECK(chmod(q->dir, 0555) == 0); }

TEST(save_in_a_read_only_directory_writes_in_place) {
    /* No new file can go in the directory, so the file is rewritten where
     * it is, which the status bar says. Root may write anywhere, so the
     * test means nothing there. */
    if (geteuid() == 0) return;
    qopts o = {.setup = make_dir_read_only};
    quip *q = qstart_opts("a.txt", "x\n", &o);
    qtype(q, "y");
    qkey(q, K_CTRL_S);
    CHECK(strstr(qmessage(q), "3 bytes written in place") != NULL);
    CHECK(!modified(q));
    char *disk = qread(q, "a.txt", NULL);
    CHECK_STR(disk, "yx\n");
    free(disk);
    chmod(q->dir, 0755); /* so the session can be cleaned up */
    qquit(q);
}
