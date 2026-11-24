/* Drives a real quip binary through a pseudo terminal, the way a user's
 * terminal would, and keeps a screen model of what it draws.
 *
 * quip redraws the whole screen once per key it reads, and every frame ends
 * by showing the cursor. The driver counts those frames, so after sending a
 * key it waits for exactly one new frame instead of sleeping. */
#ifndef QUIP_TEST_PTY_H
#define QUIP_TEST_PTY_H

#include <sys/types.h>

#include "screen.h"

typedef struct qopts {
    int rows, cols;           /* Terminal size; 24x80 when 0. */
    int no_winsize;           /* Report a 0x0 window, as a broken terminal does. */
    const char *const *argv;  /* Arguments after the program name; NULL means
                                 just the file name given to qstart. */
    const char *bin;          /* Binary to run; NULL means $QUIP_BIN. */
    int no_wait;              /* Don't wait for the first frame. */
    const char *const *files; /* Extra files to create first: name, content,
                                 name, content, ..., NULL. */
    int lenient;              /* Don't fail on output the screen model doesn't
                                 know (differential runs compare raw bytes). */
} qopts;

typedef struct quip {
    pid_t pid;
    int fd; /* PTY master. */
    screen scr;
    char dir[1024]; /* The session's working directory. */
    char *raw;      /* Everything quip wrote, for differential tests. */
    size_t rawlen, rawcap;
    int exited, status;
    int timeout_ms;
    int lenient;
    long maxrss_kb; /* Peak memory of quip, once it has exited. */
} quip;

/* Start quip in a new directory under test_dir, editing 'file' there. When
 * 'content' is not NULL the file is created with it first. */
quip *qstart(const char *file, const char *content);
quip *qstart_opts(const char *file, const char *content, const qopts *o);

/* Send one key (a byte or an escape sequence) and wait for its frame. */
void qkey(quip *q, const char *seq);
/* Send bytes and wait for 'frames' new frames. */
void qsend(quip *q, const char *bytes, int len, int frames);
/* Type text, one frame per byte; '\n' is sent as Enter. */
void qtype(quip *q, const char *text);
/* Mouse, in 1-based screen coordinates. A click is a press and a release. */
void qpress(quip *q, int row, int col);
void qrelease(quip *q, int row, int col);
void qdrag(quip *q, int row, int col);
void qclick(quip *q, int row, int col);
void qwheel(quip *q, int up, int row, int col);
/* Resize the terminal (quip gets SIGWINCH) and wait for its redraw. */
void qresize(quip *q, int rows, int cols);
/* Wait until 'frames' frames have been drawn in total. */
void qwait_frames(quip *q, long frames);
/* Read output for 'ms' milliseconds without expecting anything. */
void qpump(quip *q, int ms);

/* Screen text of 1-based row 'row', trailing blanks removed. The string is
 * valid until the next call. */
const char *qrow(quip *q, int row);
/* Text of the document line shown on screen row 'row', without the line
 * number gutter. */
const char *qline(quip *q, int row);
const cell *qcell(quip *q, int row, int col);
/* The status bar rows. */
const char *qstatus(quip *q);
const char *qmessage(quip *q);

/* Path of 'name' inside the session directory (static buffer). */
const char *qpath(quip *q, const char *name);
/* Contents of a file in the session directory, or NULL. Caller frees. */
char *qread(quip *q, const char *name, int *len);
void qwrite(quip *q, const char *name, const char *content);

/* Wait for quip to exit by itself; returns its wait status. */
int qwait_exit(quip *q);

/* Quit with Ctrl-Q (confirming as needed) and check quip exited cleanly with
 * no sanitizer report. Frees the session. */
void qquit(quip *q);
/* Kill quip, check no sanitizer report, free the session. */
void qkill(quip *q);
/* Stop without checks (for the fuzzer, which inspects the result). Returns
 * a description of any problem (crash, sanitizer report, unknown escape
 * sequence) or NULL. Caller frees. */
char *qstop(quip *q, int quit);
void qfree(quip *q);

/* The binary under test: $QUIP_BIN, else ./quip. */
const char *qbin(void);

#endif
