/* Files past 2 GB, whose size does not fit in an int. The test writes a
 * 2.2 GB file and quip needs about 12 GB of memory for it, so it only runs
 * when QUIP_BIG_TESTS is set, with a long frame timeout:
 *
 *   QUIP_BIG_TESTS=1 QUIP_FRAME_TIMEOUT_MS=600000 make test T=past_2gb */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../keys.h"
#include "../pty.h"
#include "../test.h"

#define BIG_LINES 22000000L /* of 100 bytes each, newline included */

static void make_big(quip *q) {
    FILE *f = fopen(qpath(q, "big.txt"), "w");
    CHECK(f != NULL);
    static char chunk[100 * 10000];
    for (long i = 0; i < BIG_LINES; i += 10000) {
        for (int j = 0; j < 10000; j++) {
            snprintf(chunk + 100 * j, 101, "%099ld", i + j);
            chunk[100 * j + 99] = '\n';
        }
        CHECK(fwrite(chunk, 1, sizeof(chunk), f) == sizeof(chunk));
    }
    CHECK(fclose(f) == 0);
}

TEST(file_past_2gb_opens_edits_and_saves) {
    if (!getenv("QUIP_BIG_TESTS")) return;
    qopts o = {.setup = make_big};
    quip *q = qstart_opts("big.txt", NULL, &o);
    CHECK(strstr(qstatus(q), "22000000 lines") != NULL);
    qtype(q, "x");
    qkey(q, K_CTRL_S);
    CHECK(strstr(qmessage(q), "2200000001 bytes written on disk") != NULL);
    struct stat st;
    CHECK(stat(qpath(q, "big.txt"), &st) == 0);
    CHECK(st.st_size == 2200000001LL);
    FILE *f = fopen(qpath(q, "big.txt"), "r");
    char head[8] = {0};
    CHECK(f && fread(head, 1, 4, f) == 4);
    if (f) fclose(f);
    CHECK_STR(head, "x000");
    qquit(q);
}
