/* A terminal screen model for tests: it interprets the bytes quip writes and
 * keeps the resulting grid of cells, the way a terminal emulator would.
 *
 * It understands exactly the control sequences quip produces. Anything else
 * is recorded as an error, so a new escape sequence in quip fails the tests
 * until the model (and so the tests) learn about it. */
#ifndef QUIP_TEST_SCREEN_H
#define QUIP_TEST_SCREEN_H

#include <stdint.h>

#define ATTR_BOLD    1
#define ATTR_REVERSE 2

/* Colors are the SGR codes that set them (31 = red fg, 41 = red bg,
 * 100 = bright black bg), or 0 for the terminal's default. */
typedef struct cell {
    uint32_t cp; /* Code point; ' ' when blank, 0 for the right half of
                    a wide character. */
    uint8_t fg, bg;
    uint8_t attr; /* ATTR_* bits. */
} cell;

#define MODE_MOUSE_BUTTON  1 /* ?1002 */
#define MODE_MOUSE_SGR     2 /* ?1006 */
#define MODE_BRACKET_PASTE 4 /* ?2004 */

typedef struct screen {
    int rows, cols;
    cell *grid; /* The screen in use (main or alternate). */
    cell *main_grid, *alt_grid;
    int alt;              /* Alternate screen active? */
    int x, y;             /* Cursor, zero based. */
    int wrap_pending;     /* Printed into the last column (VT autowrap). */
    int saved_x, saved_y; /* Main screen cursor while on the alternate one. */
    int cursor_visible;
    int cursor_shape;     /* Parameter of the last DECSCUSR (ESC [ n SP q). */
    int modes;            /* MODE_* bits. */
    uint8_t fg, bg, attr; /* Current SGR state. */

    /* Parser. */
    int state;
    char buf[64]; /* CSI parameters and intermediates. */
    int blen;
    char *osc; /* OSC payload. */
    int osclen, osccap;
    uint32_t u8cp; /* UTF-8 decoding. */
    int u8need;

    long frames;     /* Completed frames: one per cursor show. */
    char *clipboard; /* Last OSC 52 write, decoded. */
    int clipboard_len;

    /* Called when quip asks the terminal something (cursor position,
     * clipboard), so the harness can answer like a terminal would. */
    void (*reply)(void *ctx, const char *s, int len);
    void *reply_ctx;
    int answer_cursor;    /* Answer ESC [ 6 n? */
    int answer_clipboard; /* Answer OSC 52 queries with 'clipboard'? */

    int nerrors;
    char error[512]; /* The first few unexpected sequences. */
} screen;

void screen_init(screen *s, int rows, int cols);
void screen_free(screen *s);
/* Change the size, blanking both screens, as a terminal window resize. */
void screen_resize(screen *s, int rows, int cols);
void screen_feed(screen *s, const char *data, int len);

/* The text of row 'y' as UTF-8, trailing blanks removed. Wide characters
 * appear once. Returns 'out'. */
char *screen_row_text(const screen *s, int y, char *out, int outsize);
/* Text of cells [x0, x1) of row 'y', blanks kept. */
char *screen_span_text(const screen *s, int y, int x0, int x1, char *out, int outsize);
const cell *screen_cell(const screen *s, int y, int x);
/* A human readable picture of the whole screen with the cursor and
 * per-cell styles, for failure messages. Caller frees. */
char *screen_dump(const screen *s);
/* Display width of a code point: 0, 1 or 2 cells. */
int screen_cpwidth(uint32_t cp);

#endif
