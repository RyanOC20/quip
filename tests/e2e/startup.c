/* Command line handling and terminal setup and teardown. */
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/wait.h>

#include "../keys.h"
#include "../pty.h"
#include "../test.h"

TEST(no_arguments_prints_usage) {
    const char *const none[] = {NULL};
    qopts o = {.argv = none, .no_wait = 1};
    quip *q = qstart_opts(NULL, NULL, &o);
    int st = qwait_exit(q);
    CHECK(WIFEXITED(st));
    CHECK_INT(WEXITSTATUS(st), 1);
    CHECK_STR(qrow(q, 1), "Usage: quip <filename>");
    CHECK(!q->scr.alt);
    qfree(q);
}

TEST(quit_restores_terminal) {
    quip *q = qstart("a.txt", "x\n");
    CHECK(q->scr.alt);
    CHECK(q->scr.modes & MODE_MOUSE_BUTTON);
    /* qquit checks the alternate screen and mouse mode are off again. */
    qquit(q);
}

TEST(cursor_shape_restored_on_quit) {
    quip *q = qstart("a.txt", "x\n");
    CHECK_INT(q->scr.cursor_shape, 5);
    qsend(q, K_CTRL_Q, 1, 0);
    qwait_exit(q);
    CHECK_INT(q->scr.cursor_shape, 0);
    qfree(q);
}

TEST(sigterm_restores_terminal) {
    quip *q = qstart("a.txt", "x\n");
    kill(q->pid, SIGTERM);
    int st = qwait_exit(q);
    CHECK(WIFSIGNALED(st));
    CHECK_INT(WTERMSIG(st), SIGTERM);
    CHECK(!q->scr.alt);
    CHECK_INT(q->scr.modes, 0);
    qfree(q);
}

TEST(language_files_found_next_to_binary) {
    /* No language files installed for the user: quip still finds langs/
     * beside its own binary and highlights Python. */
    quip *q = qstart("a.py", "def f(): pass\n");
    CHECK_INT(qcell(q, 1, 5)->fg, 33);
    CHECK_STR(qmessage(q), "");
    qquit(q);
}

TEST(user_language_file_overrides_shipped) {
    /* A user python.lang where "pass" is the only keyword. */
    const char *const files[] = {
        ".config/quip/langs/python.lang",
        "quip-lang 1\nextensions .py\nkeywords pass\n", NULL};
    qopts o = {.files = files};
    quip *q = qstart_opts("a.py", "def pass\n", &o);
    CHECK_INT(qcell(q, 1, 5)->fg, 0);
    CHECK_INT(qcell(q, 1, 9)->fg, 33);
    qquit(q);
}

TEST(bad_language_file_is_reported) {
    const char *const files[] = {
        ".config/quip/langs/bad.lang", "quip-lang 1\nextensions .zz\nbogus x\n",
        NULL};
    qopts o = {.files = files};
    quip *q = qstart_opts("a.txt", "x\n", &o);
    CHECK(strstr(qmessage(q), "bad.lang") != NULL);
    qquit(q);
}
