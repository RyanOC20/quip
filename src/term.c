/* The terminal: raw mode, reading keys, window size and resize signals. */
#include "quip.h"

/* Write all of buf: write() may write only part of it, or be interrupted
 * by a signal before writing anything. Returns 0, or -1 on error. Safe to
 * call from a signal handler. */
int writeAll(int fd, const void *buf, size_t len) {
    const char *p = buf;
    while (len > 0) {
        /* At most 1 GB a call: macOS refuses writes over INT_MAX bytes. */
        ssize_t n = write(fd, p, len < (1u << 30) ? len : (1u << 30));
        if (n == -1) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static struct termios orig_termios; /* In order to restore at exit.*/

static void disableRawMode(int fd) {
    /* Don't even check the return value as it's too late. */
    if (E.rawmode) {
        writeLit(STDOUT_FILENO, VT_MOUSE_OFF);
        writeLit(STDOUT_FILENO, VT_CURSOR_DEFAULT);
        writeLit(STDOUT_FILENO, VT_ALT_SCREEN_OFF); /* back to the shell's screen */
        tcsetattr(fd, TCSAFLUSH, &orig_termios);
        E.rawmode = 0;
    }
}

/* Leave the editor screen, then report a fatal error where the user can
 * still read it after exit. 'err' is an errno value, or 0 for none. */
void editorDie(const char *msg, int err) {
    disableRawMode(STDIN_FILENO);
    if (err)
        fprintf(stderr, "%s: %s\n", msg, strerror(err));
    else
        fprintf(stderr, "%s\n", msg);
    exit(1);
}

/* Restore the terminal when killed, then die by the same signal. Only
 * async-signal-safe calls (write, tcsetattr) are made on the way. */
static void handleSigTerm(int sig) {
    disableRawMode(STDIN_FILENO);
    signal(sig, SIG_DFL);
    raise(sig);
}

/* Called at exit to avoid remaining in raw mode. */
static void editorAtExit(void) { disableRawMode(STDIN_FILENO); }

/* Raw mode: 1960 magic shit. */
int enableRawMode(int fd) {
    struct termios raw;

    if (E.rawmode) return 0; /* Already enabled. */
    if (!isatty(STDIN_FILENO)) goto fatal;
    atexit(editorAtExit);
    if (tcgetattr(fd, &orig_termios) == -1) goto fatal;

    raw = orig_termios; /* modify the original mode */
    /* input modes: no break, no CR to NL, no parity check, no strip char,
     * no start/stop output control. */
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    /* output modes - disable post processing */
    raw.c_oflag &= ~(OPOST);
    /* control modes - set 8 bit chars */
    raw.c_cflag |= (CS8);
    /* local modes - choing off, canonical off, no extended functions,
     * no signal chars (^Z,^C) */
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    /* control chars - set return condition: min number of bytes and timer. */
    raw.c_cc[VMIN] = 0;  /* Return each byte, or zero for timeout. */
    raw.c_cc[VTIME] = 1; /* 100 ms timeout (unit is tens of second). */

    /* put terminal in raw mode after flushing */
    if (tcsetattr(fd, TCSAFLUSH, &raw) < 0) goto fatal;
    E.rawmode = 1;
    /* Draw on the alternate screen so quitting restores the shell's screen
     * instead of leaving the file's contents behind. */
    writeLit(STDOUT_FILENO, VT_ALT_SCREEN_ON);
    signal(SIGTERM, handleSigTerm);
    signal(SIGHUP, handleSigTerm);
    signal(SIGINT, handleSigTerm);
    writeLit(STDOUT_FILENO, VT_MOUSE_ON);
    writeLit(STDOUT_FILENO, VT_CURSOR_BAR);
    return 0;

fatal:
    errno = ENOTTY;
    return -1;
}

/* Set by the SIGWINCH handler; editorReadKey redraws for the new size. */
static volatile sig_atomic_t winch_pending = 0;
static void editorHandleResize(void);

/* Bytes read past a UTF-8 lead byte that turned out not to complete it,
 * handed out again, in order, before anything new is read. */
static char key_pushback[4];
static int key_npush = 0, key_pushpos = 0;

/* Read one byte. Returns 1, or 0 when the read times out (raw mode waits
 * 0.1s) or fails. A signal arriving meanwhile does not end the wait. */
static int readByte(int fd, char *c) {
    ssize_t n;
    do n = read(fd, c, 1);
    while (n == -1 && errno == EINTR);
    return n == 1;
}

/* The key for a mouse report: button code 'b' (the low two bits name the
 * button, 4/8/16 are Shift/Alt/Ctrl, 32 means motion, 64 the wheel) at the
 * 0-based cell (x,y). 'release' is set for a button release. */
static int editorMouseKey(int b, int x, int y, int release) {
    if (b & 64) {
        /* Wheel: bits 0-1 give up/down/left/right. Shift turns a vertical
         * wheel into a horizontal one, for mice without a tilt wheel. */
        int dir = b & 3;
        if ((b & 4) && dir < 2) dir += 2;
        if (dir == 0) return SCROLL_UP;
        if (dir == 1) return SCROLL_DOWN;
        if (dir == 2) return SCROLL_LEFT;
        return SCROLL_RIGHT;
    }
    E.mouse_cx = x;
    E.mouse_cy = y;
    if (release) return MOUSE_RELEASE;
    if (b == 0) return MOUSE_CLICK; /* left press */
    if (b == 32) return MOUSE_DRAG; /* motion, left held */
    return KEY_NULL;
}

/* Having read byte 'c', read the rest of the UTF-8 character it starts.
 * Returns KEY_CHAR plus the code point, or 'c' alone when it does not start a
 * valid sequence; the bytes read after it then come back as keys of their
 * own. Typing a character inserts it whole, so a half-typed one is never on
 * screen. */
static int editorReadUtf8(int fd, char c) {
    unsigned char b = (unsigned char)c;
    if (b < 0xc2 || b > 0xf4) return c;
    int need = b >= 0xf0 ? 4 : b >= 0xe0 ? 3 : 2;
    char u[4] = {c};
    int n = 1;
    while (n < need && readByte(fd, u + n)) {
        n++;
        if (((unsigned char)u[n - 1] & 0xc0) != 0x80) break;
    }
    uint32_t cp;
    if (n == need && utf8Decode(u, 0, n, &cp) == n) return KEY_CHAR + (int)cp;
    key_npush = n - 1;
    key_pushpos = 0;
    memcpy(key_pushback, u + 1, n - 1);
    return c;
}

/* Read a key from the terminal put in raw mode, trying to handle
 * escape sequences. While waiting for the first byte it redraws whenever
 * the terminal is resized. */
int editorReadKey(int fd) {
    char c, seq[5];
    for (;;) {
        if (key_pushpos < key_npush) {
            c = key_pushback[key_pushpos++];
            break;
        }
        ssize_t n = read(fd, &c, 1);
        if (n == 1) break;
        if (n == -1 && errno != EINTR && errno != EAGAIN)
            editorDie("Unable to read from the terminal", errno);
        if (winch_pending) editorHandleResize();
    }

    while (1) {
        switch (c) {
        case ESC: /* escape sequence */
            /* If this is just an ESC, we'll timeout here. */
            if (!readByte(fd, seq)) return ESC;

            /* ESC b / ESC f: Alt+Left / Alt+Right (2-byte sequences).
             * Must be checked before reading seq[1] or the second read
             * will time out — there are no more bytes in the sequence. */
            if (seq[0] == 'b') return ALT_ARROW_LEFT;
            if (seq[0] == 'f') return ALT_ARROW_RIGHT;

            /* ESC ] starts an OSC sequence (e.g. an OSC 52 clipboard response
             * that leaked into stdin).  Consume until BEL or ST so the base64
             * payload is never misinterpreted as keystrokes. */
            if (seq[0] == ']') {
                char osc = 0;
                while (readByte(fd, &osc) && (unsigned char)osc != 0x07 &&
                       (unsigned char)osc != 0x1b) {}
                if ((unsigned char)osc == 0x1b) readByte(fd, &osc); /* eat \ of ST */
                break; /* restart the while(1) to read the next real key */
            }

            if (!readByte(fd, seq + 1)) return ESC;

            /* ESC [ sequences. */
            if (seq[0] == '[') {
                if (seq[1] >= '0' && seq[1] <= '9') {
                    /* Extended escape, read additional byte. */
                    if (!readByte(fd, seq + 2)) return ESC;
                    if (seq[2] == '~') {
                        switch (seq[1]) {
                        case '3': return DEL_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        }
                    } else if (seq[1] == '1' && seq[2] == ';') {
                        /* ESC [ 1 ; <modifier> <dir>: modifier+arrow sequences.
                         * Modifier: 2=Shift, 3=Alt, 4=Shift+Alt, 5=Ctrl, 6=Shift+Ctrl. */
                        if (!readByte(fd, seq + 3)) return ESC;
                        if (!readByte(fd, seq + 4)) return ESC;
                        switch (seq[3]) {
                        case '2': /* Shift */
                            switch (seq[4]) {
                            case 'A': return SHIFT_ARROW_UP;
                            case 'B': return SHIFT_ARROW_DOWN;
                            case 'C': return SHIFT_ARROW_RIGHT;
                            case 'D': return SHIFT_ARROW_LEFT;
                            }
                            break;
                        case '3': /* Alt */
                            switch (seq[4]) {
                            case 'A': return ALT_ARROW_UP;
                            case 'B': return ALT_ARROW_DOWN;
                            case 'C': return ALT_ARROW_RIGHT;
                            case 'D': return ALT_ARROW_LEFT;
                            }
                            break;
                        case '4': /* Shift+Alt */
                            switch (seq[4]) {
                            case 'A': return SHIFT_ALT_ARROW_UP;
                            case 'B': return SHIFT_ALT_ARROW_DOWN;
                            case 'C': return SHIFT_ALT_ARROW_RIGHT;
                            case 'D': return SHIFT_ALT_ARROW_LEFT;
                            }
                            break;
                        case '5': /* Ctrl */
                            switch (seq[4]) {
                            case 'A': return CTRL_ARROW_UP;
                            case 'B': return CTRL_ARROW_DOWN;
                            case 'C': return CTRL_ARROW_RIGHT;
                            case 'D': return CTRL_ARROW_LEFT;
                            }
                            break;
                        case '6': /* Shift+Ctrl */
                            switch (seq[4]) {
                            case 'A': return SHIFT_CTRL_ARROW_UP;
                            case 'B': return SHIFT_CTRL_ARROW_DOWN;
                            case 'C': return SHIFT_CTRL_ARROW_RIGHT;
                            case 'D': return SHIFT_CTRL_ARROW_LEFT;
                            }
                            break;
                        }
                    }
                } else {
                    switch (seq[1]) {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                    case 'Z': return SHIFT_TAB;
                    case 'M': {
                        /* X10 mouse report: button, column and row, each
                         * a byte, plus 32, columns and rows from 1. */
                        if (!readByte(fd, seq + 2)) return ESC;
                        if (!readByte(fd, seq + 3)) return ESC;
                        if (!readByte(fd, seq + 4)) return ESC;
                        int b = (unsigned char)seq[2] - 32;
                        return editorMouseKey(b, (unsigned char)seq[3] - 33,
                                              (unsigned char)seq[4] - 33, b == 3);
                    }
                    case '<': {
                        /* SGR mouse report: ESC [ < button ; column ; row,
                         * then M for a press or motion, m for a release. */
                        int v[3] = {0, 0, 0}, k = 0;
                        char ch;
                        for (;;) {
                            if (!readByte(fd, &ch)) return ESC;
                            if (ch >= '0' && ch <= '9') {
                                if (v[k] < 100000) v[k] = v[k] * 10 + (ch - '0');
                            } else if (ch == ';' && k < 2) {
                                k++;
                            } else {
                                break;
                            }
                        }
                        if (k != 2 || (ch != 'M' && ch != 'm')) return KEY_NULL;
                        if (ch == 'm' && v[0] != 0) return KEY_NULL; /* not the left button */
                        return editorMouseKey(v[0], v[1] - 1, v[2] - 1, ch == 'm');
                    }
                    }
                }
            }

            /* ESC O sequences. */
            else if (seq[0] == 'O') {
                switch (seq[1]) {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
                }
            }

            break;
        default: return editorReadUtf8(fd, c);
        }
    }
}

/* Use the ESC [6n escape sequence to query the horizontal cursor position
 * and return it. On error -1 is returned, on success the position of the
 * cursor is stored at *rows and *cols and 0 is returned. */
static int getCursorPosition(int ifd, int ofd, int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;

    /* Report cursor location */
    if (writeLit(ofd, VT_QUERY_CURSOR) == -1) return -1;

    /* Read the response: ESC [ rows ; cols R */
    while (i < sizeof(buf) - 1) {
        if (!readByte(ifd, buf + i)) break;
        if (buf[i] == 'R') break;
        i++;
    }
    buf[i] = '\0';

    /* Parse it. */
    if (buf[0] != ESC || buf[1] != '[') return -1;
    if (sscanf(buf + 2, "%d;%d", rows, cols) != 2) return -1;
    return 0;
}

/* Try to get the number of columns in the current terminal. If the ioctl()
 * call fails the function will try to query the terminal itself.
 * Returns 0 on success, -1 on error. */
static int getWindowSize(int ifd, int ofd, int *rows, int *cols) {
    struct winsize ws;

    if (ioctl(1, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        /* ioctl() failed. Try to query the terminal itself. */
        int orig_row, orig_col, retval;

        /* Get the initial position so we can restore it later. */
        retval = getCursorPosition(ifd, ofd, &orig_row, &orig_col);
        if (retval == -1) goto failed;

        /* Go to right/bottom margin and get position. */
        if (writeLit(ofd, VT_CURSOR_FAR) == -1) goto failed;
        retval = getCursorPosition(ifd, ofd, rows, cols);
        if (retval == -1) goto failed;

        /* Restore position. */
        char seq[32];
        snprintf(seq, 32, "\x1b[%d;%dH", orig_row, orig_col);
        if (writeAll(ofd, seq, strlen(seq)) == -1) {
            /* Can't recover... */
        }
        return 0;
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }

failed:
    return -1;
}

/* Reads one byte from stdin with a timeout of `ms` milliseconds.
 * Returns 1 on success, 0 on timeout, -1 on error. */
int readByteTimeout(unsigned char *ch, int ms) {
    fd_set fds;
    struct timeval tv;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    int r;
    do r = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
    while (r == -1 && errno == EINTR);
    if (r <= 0) return r;
    return readByte(STDIN_FILENO, (char *)ch) ? 1 : -1;
}

void updateWindowSize(void) {
    /* A terminal that neither reports its size nor answers is assumed to be
     * 80x24 until a resize says otherwise. */
    if (getWindowSize(STDIN_FILENO, STDOUT_FILENO, &E.screenrows, &E.termcols) == -1 ||
        E.screenrows < 1 || E.termcols < 1) {
        E.screenrows = 24;
        E.termcols = 80;
    }
    E.screenrows -= 2;      /* Get room for status bar. */
    editorUpdateGeometry(); /* Reserve columns for the sidebar and gutter. */
}

/* Only note the resize: redrawing allocates memory, which a signal handler
 * must not do. editorReadKey calls editorHandleResize. */
static void handleSigWinCh(int unused __attribute__((unused))) { winch_pending = 1; }

/* Redraw whenever the terminal is resized. */
void editorWatchResize(void) {
    /* sigaction, not signal: on Linux, signal() under -std=c99 resets the
     * handler after one use. No SA_RESTART, so a resize wakes the pending
     * read at once instead of after its 0.1s timeout. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handleSigWinCh;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGWINCH, &sa, NULL);
}

static void editorHandleResize(void) {
    winch_pending = 0;
    updateWindowSize();
    if (E.ft_scroll > E.ft_vcount) E.ft_scroll = E.ft_vcount ? E.ft_vcount - 1 : 0;
    editorRefreshScreen();
}
