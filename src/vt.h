/* Terminal output: VT100 and xterm escape sequences, and the append buffer a
 * frame is composed in. */
#ifndef QUIP_VT_H
#define QUIP_VT_H

#include <stddef.h>

/* Terminal escape sequences (VT100 and xterm). Write them with abAppendLit
 * or writeLit, which take the length from the literal so it is never counted
 * by hand. The "" on both sides only compiles for a string literal: a char *
 * would silently give sizeof(char *). */
#define LIT_LEN(s)         ((int)sizeof("" s "") - 1)
#define abAppendLit(ab, s) abAppend(ab, "" s "", LIT_LEN(s))
#define writeLit(fd, s)    writeAll(fd, "" s "", LIT_LEN(s))

/* Write all of buf, retrying short and interrupted writes. Returns 0, or -1
 * on error. Safe to call from a signal handler. */
int writeAll(int fd, const void *buf, size_t len);

#define VT_HOME            "\x1b[H"
#define VT_CLEAR_EOL       "\x1b[0K" /* Erase to the end of the line. */
#define VT_CURSOR_HIDE     "\x1b[?25l"
#define VT_CURSOR_SHOW     "\x1b[?25h"
#define VT_CURSOR_BAR      "\x1b[5 q"           /* Blinking bar cursor. */
#define VT_CURSOR_DEFAULT  "\x1b[0 q"           /* The terminal's own cursor shape. */
#define VT_CURSOR_FAR      "\x1b[999C\x1b[999B" /* To the bottom right. */
#define VT_QUERY_CURSOR    "\x1b[6n"            /* Reply: ESC [ row ; col R */
#define VT_ALT_SCREEN_ON   "\x1b[?1049h"
#define VT_ALT_SCREEN_OFF  "\x1b[?1049l"
#define VT_MOUSE_ON        "\x1b[?1002h" /* Button-event mouse reporting. */
#define VT_MOUSE_OFF       "\x1b[?1002l"
#define VT_CLIPBOARD_QUERY "\x1b]52;c;?\x07" /* OSC 52 clipboard read. */

/* Text attributes (SGR). */
#define VT_RESET       "\x1b[0m" /* All attributes off. */
#define VT_BOLD        "\x1b[1m"
#define VT_REVERSE     "\x1b[7m"
#define VT_REVERSE_OFF "\x1b[27m"
#define VT_FG_DEFAULT  "\x1b[39m"
#define VT_BG_DEFAULT  "\x1b[49m"
#define VT_FG_GRAY     "\x1b[90m"
#define VT_BOLD_BLUE   "\x1b[1;34m"

/* What each background means, as SGR numbers for "ESC [ n m". */
#define BG_DEFAULT           49
#define BG_TRAILING_SPACE    41  /* red */
#define BG_BRACKET_MATCH     100 /* gray */
#define BG_BRACKET_UNMATCHED 41  /* red */

/* We define a very simple "append buffer" structure, that is an heap
 * allocated string where we can append to. This is useful in order to
 * write all the escape sequences in a buffer and flush them to the standard
 * output in a single call, to avoid flickering effects. */
struct abuf {
    char *b;
    int len;
    int cap;
};

#define ABUF_INIT {NULL, 0, 0}

void abAppend(struct abuf *ab, const char *s, int len);

#endif
