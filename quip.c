/* Copyright (C) 2016 Salvatore Sanfilippo <antirez at gmail dot com>
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *  *  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *
 *  *  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define QUIP_VERSION "0.0.1"

#ifdef __linux__
#define _POSIX_C_SOURCE 200809L
#endif

#include <termios.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdarg.h>
#include <fcntl.h>
#include <signal.h>

/* Syntax highlight types */
#define HL_NORMAL 0
#define HL_NONPRINT 1
#define HL_COMMENT 2   /* Single line comment. */
#define HL_MLCOMMENT 3 /* Multi-line comment. */
#define HL_KEYWORD1 4
#define HL_KEYWORD2 5
#define HL_STRING 6
#define HL_NUMBER 7
#define HL_MATCH 8      /* Search match. */

#define HL_HIGHLIGHT_STRINGS (1<<0)
#define HL_HIGHLIGHT_NUMBERS (1<<1)

struct editorSyntax {
    char **filematch;
    char **keywords;
    char singleline_comment_start[2];
    char multiline_comment_start[3];
    char multiline_comment_end[3];
    int flags;
};

/* This structure represents a single line of the file we are editing. */
typedef struct erow {
    int idx;            /* Row index in the file, zero-based. */
    int size;           /* Size of the row, excluding the null term. */
    int rsize;          /* Size of the rendered row. */
    char *chars;        /* Row content. */
    char *render;       /* Row content "rendered" for screen (for TABs). */
    unsigned char *hl;  /* Syntax highlight type for each character in render.*/
    int hl_oc;          /* Row had open comment at end in last syntax highlight
                           check. */
} erow;

typedef struct hlcolor {
    int r,g,b;
} hlcolor;

struct editorConfig {
    int cx,cy;  /* Cursor x and y position in characters */
    int rowoff;     /* Offset of row displayed. */
    int coloff;     /* Offset of column displayed. */
    int screenrows; /* Number of rows that we can show */
    int screencols; /* Number of cols that we can show */
    int numrows;    /* Number of rows */
    int rawmode;    /* Is terminal raw mode enabled? */
    erow *row;      /* Rows */
    int dirty;      /* File modified but not saved. */
    char *filename; /* Currently open filename */
    char statusmsg[80];
    time_t statusmsg_time;
    struct editorSyntax *syntax;    /* Current syntax highlight, or NULL. */
    int sel_active;       /* 1 if a selection is live. */
    int sel_anchor_row;   /* File row where the selection was anchored. */
    int sel_anchor_col;   /* File col where the selection was anchored. */
    int mouse_cx, mouse_cy; /* Screen position of last mouse click. */
    char *clipboard;      /* Internal clipboard buffer (may contain '\n'). */
    int   clipboard_len;  /* Byte count of clipboard contents. */
};

static struct editorConfig E;

/* ================================ Undo/Redo ================================ */

typedef struct editorSnapshot {
    char *text;
    int   len;
    int   cx, cy, rowoff, coloff;
} editorSnapshot;

#define UNDO_HISTORY_MAX 200

static editorSnapshot undo_hist[UNDO_HISTORY_MAX];
static int undo_cur   = -1;
static int undo_count =  0;
static int undo_saved = -1;
static int undo_last_insert = 0;

enum KEY_ACTION{
        KEY_NULL = 0,       /* NULL */
        CTRL_C = 3,         /* Ctrl-c */
        CTRL_D = 4,         /* Ctrl-d */
        CTRL_F = 6,         /* Ctrl-f */
        CTRL_H = 8,         /* Ctrl-h */
        TAB = 9,            /* Tab */
        CTRL_L = 12,        /* Ctrl+l */
        ENTER = 13,         /* Enter */
        CTRL_Q = 17,        /* Ctrl-q */
        CTRL_S = 19,        /* Ctrl-s */
        CTRL_U = 21,        /* Ctrl-u */
        CTRL_V = 22,        /* Ctrl-v */
        CTRL_X = 24,        /* Ctrl-x */
        CTRL_Y = 25,        /* Ctrl-y */
        CTRL_Z = 26,        /* Ctrl-z */
        ESC = 27,           /* Escape */
        CTRL_SLASH = 31,    /* Ctrl-/ */
        BACKSPACE =  127,   /* Backspace */
        /* The following are just soft codes, not really reported by the
         * terminal directly. */
        ARROW_LEFT = 1000,
        ARROW_RIGHT,
        ARROW_UP,
        ARROW_DOWN,
        DEL_KEY,
        HOME_KEY,
        END_KEY,
        PAGE_UP,
        PAGE_DOWN,
        SHIFT_ARROW_LEFT,
        SHIFT_ARROW_RIGHT,
        SHIFT_ARROW_UP,
        SHIFT_ARROW_DOWN,
        CTRL_ARROW_LEFT,
        CTRL_ARROW_RIGHT,
        CTRL_ARROW_UP,
        CTRL_ARROW_DOWN,
        ALT_ARROW_LEFT,
        ALT_ARROW_RIGHT,
        ALT_ARROW_UP,
        ALT_ARROW_DOWN,
        SHIFT_ALT_ARROW_LEFT,
        SHIFT_ALT_ARROW_RIGHT,
        SHIFT_ALT_ARROW_UP,
        SHIFT_ALT_ARROW_DOWN,
        SHIFT_CTRL_ARROW_LEFT,
        SHIFT_CTRL_ARROW_RIGHT,
        SHIFT_CTRL_ARROW_UP,
        SHIFT_CTRL_ARROW_DOWN,
        SCROLL_UP,
        SCROLL_DOWN,
        MOUSE_CLICK,
        MOUSE_DRAG,
        MOUSE_RELEASE
};

void editorSetStatusMessage(const char *fmt, ...);

/* =========================== Syntax highlights DB =========================
 *
 * In order to add a new syntax, define two arrays with a list of file name
 * matches and keywords. The file name matches are used in order to match
 * a given syntax with a given file name: if a match pattern starts with a
 * dot, it is matched as the last past of the filename, for example ".c".
 * Otherwise the pattern is just searched inside the filenme, like "Makefile").
 *
 * The list of keywords to highlight is just a list of words, however if they
 * a trailing '|' character is added at the end, they are highlighted in
 * a different color, so that you can have two different sets of keywords.
 *
 * Finally add a stanza in the HLDB global variable with two two arrays
 * of strings, and a set of flags in order to enable highlighting of
 * comments and numbers.
 *
 * The characters for single and multi line comments must be exactly two
 * and must be provided as well (see the C language example).
 *
 * There is no support to highlight patterns currently. */

/* C / C++ */
char *C_HL_extensions[] = {".c",".h",".cpp",".hpp",".cc",NULL};
char *C_HL_keywords[] = {
	/* C Keywords */
	"auto","break","case","continue","default","do","else","enum",
	"extern","for","goto","if","register","return","sizeof","static",
	"struct","switch","typedef","union","volatile","while","NULL",

	/* C++ Keywords */
	"alignas","alignof","and","and_eq","asm","bitand","bitor","class",
	"compl","constexpr","const_cast","deltype","delete","dynamic_cast",
	"explicit","export","false","friend","inline","mutable","namespace",
	"new","noexcept","not","not_eq","nullptr","operator","or","or_eq",
	"private","protected","public","reinterpret_cast","static_assert",
	"static_cast","template","this","thread_local","throw","true","try",
	"typeid","typename","virtual","xor","xor_eq",

	/* C types */
        "int|","long|","double|","float|","char|","unsigned|","signed|",
        "void|","short|","auto|","const|","bool|",NULL
};

/* Here we define an array of syntax highlights by extensions, keywords,
 * comments delimiters and flags. */
struct editorSyntax HLDB[] = {
    {
        /* C / C++ */
        C_HL_extensions,
        C_HL_keywords,
        "//","/*","*/",
        HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS
    }
};

#define HLDB_ENTRIES (sizeof(HLDB)/sizeof(HLDB[0]))

/* ======================= Low level terminal handling ====================== */

static struct termios orig_termios; /* In order to restore at exit.*/

void disableRawMode(int fd) {
    /* Don't even check the return value as it's too late. */
    if (E.rawmode) {
        write(STDOUT_FILENO, "\x1b[?1002l", 8); /* disable button-event mouse reporting */
        tcsetattr(fd,TCSAFLUSH,&orig_termios);
        E.rawmode = 0;
    }
}

/* Called at exit to avoid remaining in raw mode. */
void editorAtExit(void) {
    disableRawMode(STDIN_FILENO);
}

/* Raw mode: 1960 magic shit. */
int enableRawMode(int fd) {
    struct termios raw;

    if (E.rawmode) return 0; /* Already enabled. */
    if (!isatty(STDIN_FILENO)) goto fatal;
    atexit(editorAtExit);
    if (tcgetattr(fd,&orig_termios) == -1) goto fatal;

    raw = orig_termios;  /* modify the original mode */
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
    raw.c_cc[VMIN] = 0; /* Return each byte, or zero for timeout. */
    raw.c_cc[VTIME] = 1; /* 100 ms timeout (unit is tens of second). */

    /* put terminal in raw mode after flushing */
    if (tcsetattr(fd,TCSAFLUSH,&raw) < 0) goto fatal;
    E.rawmode = 1;
    write(STDOUT_FILENO, "\x1b[?1002h", 8); /* enable button-event mouse reporting */
    return 0;

fatal:
    errno = ENOTTY;
    return -1;
}

/* ========================= Base64 =========================================
 * Used for OSC 52 clipboard read/write. */

static const char b64chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64val(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Returns malloc'd NUL-terminated base64 string. Caller frees. */
static char *base64_encode(const unsigned char *src, int len) {
    int outlen = 4 * ((len + 2) / 3);
    char *out = malloc(outlen + 1);
    int i = 0, j = 0;
    while (i < len) {
        unsigned int a = (unsigned char)src[i++];
        unsigned int b = (i < len) ? (unsigned char)src[i++] : 0;
        unsigned int c = (i < len) ? (unsigned char)src[i++] : 0;
        unsigned int triple = (a << 16) | (b << 8) | c;
        out[j++] = b64chars[(triple >> 18) & 0x3f];
        out[j++] = b64chars[(triple >> 12) & 0x3f];
        out[j++] = b64chars[(triple >>  6) & 0x3f];
        out[j++] = b64chars[ triple        & 0x3f];
    }
    if (len % 3 == 1) { out[j-2] = '='; out[j-1] = '='; }
    else if (len % 3 == 2) { out[j-1] = '='; }
    out[j] = '\0';
    return out;
}

/* Returns malloc'd decoded buffer. Sets *outlen. Caller frees. */
static char *base64_decode(const char *src, int *outlen) {
    int srclen = 0;
    while (src[srclen]) srclen++;
    int maxout = (srclen / 4) * 3 + 3;
    char *out = malloc(maxout + 1);
    int i = 0, j = 0;
    while (i < srclen) {
        /* Skip whitespace that some terminals insert as line-wrap in responses. */
        while (i < srclen && (src[i] == '\r' || src[i] == '\n' || src[i] == ' '))
            i++;
        if (i >= srclen) break;
        int v0 = b64val((unsigned char)src[i++]);
        int v1 = (i < srclen) ? b64val((unsigned char)src[i++]) : -1;
        int v2 = (i < srclen) ? b64val((unsigned char)src[i++]) : -1;
        int v3 = (i < srclen) ? b64val((unsigned char)src[i++]) : -1;
        if (v0 < 0 || v1 < 0) break;
        out[j++] = (char)((v0 << 2) | (v1 >> 4));
        if (v2 >= 0) out[j++] = (char)(((v1 & 0xf) << 4) | (v2 >> 2));
        if (v2 >= 0 && v3 >= 0) out[j++] = (char)(((v2 & 0x3) << 6) | v3);
    }
    out[j] = '\0';
    *outlen = j;
    return out;
}

/* ========================================================================= */

/* Read a key from the terminal put in raw mode, trying to handle
 * escape sequences. */
int editorReadKey(int fd) {
    int nread;
    char c, seq[5];
    while ((nread = read(fd,&c,1)) == 0);
    if (nread == -1) exit(1);

    while(1) {
        switch(c) {
        case ESC:    /* escape sequence */
            /* If this is just an ESC, we'll timeout here. */
            if (read(fd,seq,1) == 0) return ESC;

            /* ESC b / ESC f: Alt+Left / Alt+Right (2-byte sequences).
             * Must be checked before reading seq[1] or the second read
             * will time out — there are no more bytes in the sequence. */
            if (seq[0] == 'b') return ALT_ARROW_LEFT;
            if (seq[0] == 'f') return ALT_ARROW_RIGHT;

            /* ESC ] starts an OSC sequence (e.g. an OSC 52 clipboard response
             * that leaked into stdin).  Consume until BEL or ST so the base64
             * payload is never misinterpreted as keystrokes. */
            if (seq[0] == ']') {
                char osc;
                while (read(fd, &osc, 1) == 1 &&
                       (unsigned char)osc != 0x07 &&
                       (unsigned char)osc != 0x1b) {}
                if ((unsigned char)osc == 0x1b) read(fd, &osc, 1); /* eat \ of ST */
                break; /* restart the while(1) to read the next real key */
            }

            if (read(fd,seq+1,1) == 0) return ESC;

            /* ESC [ sequences. */
            if (seq[0] == '[') {
                if (seq[1] >= '0' && seq[1] <= '9') {
                    /* Extended escape, read additional byte. */
                    if (read(fd,seq+2,1) == 0) return ESC;
                    if (seq[2] == '~') {
                        switch(seq[1]) {
                        case '3': return DEL_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        }
                    } else if (seq[1] == '1' && seq[2] == ';') {
                        /* ESC [ 1 ; <modifier> <dir>: modifier+arrow sequences.
                         * Modifier: 2=Shift, 3=Alt, 4=Shift+Alt, 5=Ctrl, 6=Shift+Ctrl. */
                        if (read(fd,seq+3,1) == 0) return ESC;
                        if (read(fd,seq+4,1) == 0) return ESC;
                        switch(seq[3]) {
                        case '2': /* Shift */
                            switch(seq[4]) {
                            case 'A': return SHIFT_ARROW_UP;
                            case 'B': return SHIFT_ARROW_DOWN;
                            case 'C': return SHIFT_ARROW_RIGHT;
                            case 'D': return SHIFT_ARROW_LEFT;
                            }
                            break;
                        case '3': /* Alt */
                            switch(seq[4]) {
                            case 'A': return ALT_ARROW_UP;
                            case 'B': return ALT_ARROW_DOWN;
                            case 'C': return ALT_ARROW_RIGHT;
                            case 'D': return ALT_ARROW_LEFT;
                            }
                            break;
                        case '4': /* Shift+Alt */
                            switch(seq[4]) {
                            case 'A': return SHIFT_ALT_ARROW_UP;
                            case 'B': return SHIFT_ALT_ARROW_DOWN;
                            case 'C': return SHIFT_ALT_ARROW_RIGHT;
                            case 'D': return SHIFT_ALT_ARROW_LEFT;
                            }
                            break;
                        case '5': /* Ctrl */
                            switch(seq[4]) {
                            case 'A': return CTRL_ARROW_UP;
                            case 'B': return CTRL_ARROW_DOWN;
                            case 'C': return CTRL_ARROW_RIGHT;
                            case 'D': return CTRL_ARROW_LEFT;
                            }
                            break;
                        case '6': /* Shift+Ctrl */
                            switch(seq[4]) {
                            case 'A': return SHIFT_CTRL_ARROW_UP;
                            case 'B': return SHIFT_CTRL_ARROW_DOWN;
                            case 'C': return SHIFT_CTRL_ARROW_RIGHT;
                            case 'D': return SHIFT_CTRL_ARROW_LEFT;
                            }
                            break;
                        }
                    }
                } else {
                    switch(seq[1]) {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                    case 'M': {
                        if (read(fd,seq+2,1) == 0) return ESC;
                        if (read(fd,seq+3,1) == 0) return ESC;
                        if (read(fd,seq+4,1) == 0) return ESC;
                        unsigned char mbtn = (unsigned char)seq[2];
                        if (mbtn == 96) return SCROLL_UP;
                        if (mbtn == 97) return SCROLL_DOWN;
                        E.mouse_cx = (unsigned char)seq[3] - 33;
                        E.mouse_cy = (unsigned char)seq[4] - 33;
                        if (mbtn == 32) return MOUSE_CLICK;   /* left press */
                        if (mbtn == 35) return MOUSE_RELEASE; /* any release */
                        if (mbtn == 64) return MOUSE_DRAG;    /* motion, left held */
                        return KEY_NULL;
                    }
                    }
                }
            }

            /* ESC O sequences. */
            else if (seq[0] == 'O') {
                switch(seq[1]) {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
                }
            }

            break;
        default:
            return c;
        }
    }
}

/* Use the ESC [6n escape sequence to query the horizontal cursor position
 * and return it. On error -1 is returned, on success the position of the
 * cursor is stored at *rows and *cols and 0 is returned. */
int getCursorPosition(int ifd, int ofd, int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;

    /* Report cursor location */
    if (write(ofd, "\x1b[6n", 4) != 4) return -1;

    /* Read the response: ESC [ rows ; cols R */
    while (i < sizeof(buf)-1) {
        if (read(ifd,buf+i,1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }
    buf[i] = '\0';

    /* Parse it. */
    if (buf[0] != ESC || buf[1] != '[') return -1;
    if (sscanf(buf+2,"%d;%d",rows,cols) != 2) return -1;
    return 0;
}

/* Try to get the number of columns in the current terminal. If the ioctl()
 * call fails the function will try to query the terminal itself.
 * Returns 0 on success, -1 on error. */
int getWindowSize(int ifd, int ofd, int *rows, int *cols) {
    struct winsize ws;

    if (ioctl(1, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        /* ioctl() failed. Try to query the terminal itself. */
        int orig_row, orig_col, retval;

        /* Get the initial position so we can restore it later. */
        retval = getCursorPosition(ifd,ofd,&orig_row,&orig_col);
        if (retval == -1) goto failed;

        /* Go to right/bottom margin and get position. */
        if (write(ofd,"\x1b[999C\x1b[999B",12) != 12) goto failed;
        retval = getCursorPosition(ifd,ofd,rows,cols);
        if (retval == -1) goto failed;

        /* Restore position. */
        char seq[32];
        snprintf(seq,32,"\x1b[%d;%dH",orig_row,orig_col);
        if (write(ofd,seq,strlen(seq)) == -1) {
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

/* ====================== Syntax highlight color scheme  ==================== */

int is_separator(int c) {
    return c == '\0' || isspace(c) || strchr(",.()+-/*=~%[];",c) != NULL;
}

/* Return true if the specified row last char is part of a multi line comment
 * that starts at this row or at one before, and does not end at the end
 * of the row but spawns to the next row. */
int editorRowHasOpenComment(erow *row) {
    if (row->hl && row->rsize && row->hl[row->rsize-1] == HL_MLCOMMENT &&
        (row->rsize < 2 || (row->render[row->rsize-2] != '*' ||
                            row->render[row->rsize-1] != '/'))) return 1;
    return 0;
}

/* Set every byte of row->hl (that corresponds to every character in the line)
 * to the right syntax highlight type (HL_* defines). */
void editorUpdateSyntax(erow *row) {
    row->hl = realloc(row->hl,row->rsize);
    memset(row->hl,HL_NORMAL,row->rsize);

    if (E.syntax == NULL) return; /* No syntax, everything is HL_NORMAL. */

    int i, prev_sep, in_string, in_comment;
    char *p;
    char **keywords = E.syntax->keywords;
    char *scs = E.syntax->singleline_comment_start;
    char *mcs = E.syntax->multiline_comment_start;
    char *mce = E.syntax->multiline_comment_end;

    /* Point to the first non-space char. */
    p = row->render;
    i = 0; /* Current char offset */
    while(*p && isspace(*p)) {
        p++;
        i++;
    }
    prev_sep = 1; /* Tell the parser if 'i' points to start of word. */
    in_string = 0; /* Are we inside "" or '' ? */
    in_comment = 0; /* Are we inside multi-line comment? */

    /* If the previous line has an open comment, this line starts
     * with an open comment state. */
    if (row->idx > 0 && editorRowHasOpenComment(&E.row[row->idx-1]))
        in_comment = 1;

    while(*p) {
        /* Handle // comments. */
        if (prev_sep && *p == scs[0] && *(p+1) == scs[1]) {
            /* From here to end is a comment */
            memset(row->hl+i,HL_COMMENT,row->size-i);
            return;
        }

        /* Handle multi line comments. */
        if (in_comment) {
            row->hl[i] = HL_MLCOMMENT;
            if (*p == mce[0] && *(p+1) == mce[1]) {
                row->hl[i+1] = HL_MLCOMMENT;
                p += 2; i += 2;
                in_comment = 0;
                prev_sep = 1;
                continue;
            } else {
                prev_sep = 0;
                p++; i++;
                continue;
            }
        } else if (*p == mcs[0] && *(p+1) == mcs[1]) {
            row->hl[i] = HL_MLCOMMENT;
            row->hl[i+1] = HL_MLCOMMENT;
            p += 2; i += 2;
            in_comment = 1;
            prev_sep = 0;
            continue;
        }

        /* Handle "" and '' */
        if (in_string) {
            row->hl[i] = HL_STRING;
            if (*p == '\\') {
                row->hl[i+1] = HL_STRING;
                p += 2; i += 2;
                prev_sep = 0;
                continue;
            }
            if (*p == in_string) in_string = 0;
            p++; i++;
            continue;
        } else {
            if (*p == '"' || *p == '\'') {
                in_string = *p;
                row->hl[i] = HL_STRING;
                p++; i++;
                prev_sep = 0;
                continue;
            }
        }

        /* Handle non printable chars. */
        if (!isprint(*p)) {
            row->hl[i] = HL_NONPRINT;
            p++; i++;
            prev_sep = 0;
            continue;
        }

        /* Handle numbers */
        if ((isdigit(*p) && (prev_sep || row->hl[i-1] == HL_NUMBER)) ||
            (*p == '.' && i >0 && row->hl[i-1] == HL_NUMBER)) {
            row->hl[i] = HL_NUMBER;
            p++; i++;
            prev_sep = 0;
            continue;
        }

        /* Handle keywords and lib calls */
        if (prev_sep) {
            int j;
            for (j = 0; keywords[j]; j++) {
                int klen = strlen(keywords[j]);
                int kw2 = keywords[j][klen-1] == '|';
                if (kw2) klen--;

                if (!memcmp(p,keywords[j],klen) &&
                    is_separator(*(p+klen)))
                {
                    /* Keyword */
                    memset(row->hl+i,kw2 ? HL_KEYWORD2 : HL_KEYWORD1,klen);
                    p += klen;
                    i += klen;
                    break;
                }
            }
            if (keywords[j] != NULL) {
                prev_sep = 0;
                continue; /* We had a keyword match */
            }
        }

        /* Not special chars */
        prev_sep = is_separator(*p);
        p++; i++;
    }

    /* Propagate syntax change to the next row if the open commen
     * state changed. This may recursively affect all the following rows
     * in the file. */
    int oc = editorRowHasOpenComment(row);
    if (row->hl_oc != oc && row->idx+1 < E.numrows)
        editorUpdateSyntax(&E.row[row->idx+1]);
    row->hl_oc = oc;
}

/* Maps syntax highlight token types to terminal colors. */
int editorSyntaxToColor(int hl) {
    switch(hl) {
    case HL_COMMENT:
    case HL_MLCOMMENT: return 36;     /* cyan */
    case HL_KEYWORD1: return 33;    /* yellow */
    case HL_KEYWORD2: return 32;    /* green */
    case HL_STRING: return 35;      /* magenta */
    case HL_NUMBER: return 31;      /* red */
    case HL_MATCH: return 34;      /* blu */
    default: return 37;             /* white */
    }
}

/* Select the syntax highlight scheme depending on the filename,
 * setting it in the global state E.syntax. */
void editorSelectSyntaxHighlight(char *filename) {
    for (unsigned int j = 0; j < HLDB_ENTRIES; j++) {
        struct editorSyntax *s = HLDB+j;
        unsigned int i = 0;
        while(s->filematch[i]) {
            char *p;
            int patlen = strlen(s->filematch[i]);
            if ((p = strstr(filename,s->filematch[i])) != NULL) {
                if (s->filematch[i][0] != '.' || p[patlen] == '\0') {
                    E.syntax = s;
                    return;
                }
            }
            i++;
        }
    }
}

/* ======================= Editor rows implementation ======================= */

/* Update the rendered version and the syntax highlight of a row. */
void editorUpdateRow(erow *row) {
    unsigned int tabs = 0, nonprint = 0;
    int j, idx;

   /* Create a version of the row we can directly print on the screen,
     * respecting tabs, substituting non printable characters with '?'. */
    free(row->render);
    for (j = 0; j < row->size; j++)
        if (row->chars[j] == TAB) tabs++;

    unsigned long long allocsize =
        (unsigned long long) row->size + tabs*8 + nonprint*9 + 1;
    if (allocsize > UINT32_MAX) {
        printf("Some line of the edited file is too long for quip\n");
        exit(1);
    }

    row->render = malloc(row->size + tabs*8 + nonprint*9 + 1);
    idx = 0;
    for (j = 0; j < row->size; j++) {
        if (row->chars[j] == TAB) {
            row->render[idx++] = ' ';
            while((idx+1) % 8 != 0) row->render[idx++] = ' ';
        } else {
            row->render[idx++] = row->chars[j];
        }
    }
    row->rsize = idx;
    row->render[idx] = '\0';

    /* Update the syntax highlighting attributes of the row. */
    editorUpdateSyntax(row);
}

/* Insert a row at the specified position, shifting the other rows on the bottom
 * if required. */
void editorInsertRow(int at, char *s, size_t len) {
    if (at > E.numrows) return;
    E.row = realloc(E.row,sizeof(erow)*(E.numrows+1));
    if (at != E.numrows) {
        memmove(E.row+at+1,E.row+at,sizeof(E.row[0])*(E.numrows-at));
        for (int j = at+1; j <= E.numrows; j++) E.row[j].idx++;
    }
    E.row[at].size = len;
    E.row[at].chars = malloc(len+1);
    memcpy(E.row[at].chars,s,len+1);
    E.row[at].hl = NULL;
    E.row[at].hl_oc = 0;
    E.row[at].render = NULL;
    E.row[at].rsize = 0;
    E.row[at].idx = at;
    editorUpdateRow(E.row+at);
    E.numrows++;
    E.dirty++;
}

/* Free row's heap allocated stuff. */
void editorFreeRow(erow *row) {
    free(row->render);
    free(row->chars);
    free(row->hl);
}

/* Remove the row at the specified position, shifting the remainign on the
 * top. */
void editorDelRow(int at) {
    erow *row;

    if (at >= E.numrows) return;
    row = E.row+at;
    editorFreeRow(row);
    memmove(E.row+at,E.row+at+1,sizeof(E.row[0])*(E.numrows-at-1));
    for (int j = at; j < E.numrows-1; j++) E.row[j].idx++;
    E.numrows--;
    E.dirty++;
}

/* Turn the editor rows into a single heap-allocated string.
 * Returns the pointer to the heap-allocated string and populate the
 * integer pointed by 'buflen' with the size of the string, escluding
 * the final nulterm. */
char *editorRowsToString(int *buflen) {
    char *buf = NULL, *p;
    int totlen = 0;
    int j;

    /* Compute count of bytes */
    for (j = 0; j < E.numrows; j++)
        totlen += E.row[j].size+1; /* +1 is for "\n" at end of every row */
    *buflen = totlen;
    totlen++; /* Also make space for nulterm */

    p = buf = malloc(totlen);
    for (j = 0; j < E.numrows; j++) {
        memcpy(p,E.row[j].chars,E.row[j].size);
        p += E.row[j].size;
        *p = '\n';
        p++;
    }
    *p = '\0';
    return buf;
}

/* Insert a character at the specified position in a row, moving the remaining
 * chars on the right if needed. */
void editorRowInsertChar(erow *row, int at, int c) {
    if (at > row->size) {
        /* Pad the string with spaces if the insert location is outside the
         * current length by more than a single character. */
        int padlen = at-row->size;
        /* In the next line +2 means: new char and null term. */
        row->chars = realloc(row->chars,row->size+padlen+2);
        memset(row->chars+row->size,' ',padlen);
        row->chars[row->size+padlen+1] = '\0';
        row->size += padlen+1;
    } else {
        /* If we are in the middle of the string just make space for 1 new
         * char plus the (already existing) null term. */
        row->chars = realloc(row->chars,row->size+2);
        memmove(row->chars+at+1,row->chars+at,row->size-at+1);
        row->size++;
    }
    row->chars[at] = c;
    editorUpdateRow(row);
    E.dirty++;
}

/* Append the string 's' at the end of a row */
void editorRowAppendString(erow *row, char *s, size_t len) {
    row->chars = realloc(row->chars,row->size+len+1);
    memcpy(row->chars+row->size,s,len);
    row->size += len;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    E.dirty++;
}

/* Delete the character at offset 'at' from the specified row. */
void editorRowDelChar(erow *row, int at) {
    if (row->size <= at) return;
    memmove(row->chars+at,row->chars+at+1,row->size-at);
    editorUpdateRow(row);
    row->size--;
    E.dirty++;
}

/* Insert the specified char at the current prompt position. */
void editorInsertChar(int c) {
    int filerow = E.rowoff+E.cy;
    int filecol = E.coloff+E.cx;
    erow *row = (filerow >= E.numrows) ? NULL : &E.row[filerow];

    /* If the row where the cursor is currently located does not exist in our
     * logical representaion of the file, add enough empty rows as needed. */
    if (!row) {
        while(E.numrows <= filerow)
            editorInsertRow(E.numrows,"",0);
    }
    row = &E.row[filerow];
    editorRowInsertChar(row,filecol,c);
    if (E.cx == E.screencols-1)
        E.coloff++;
    else
        E.cx++;
    E.dirty++;
}

/* Inserting a newline is slightly complex as we have to handle inserting a
 * newline in the middle of a line, splitting the line as needed. */
void editorInsertNewline(void) {
    int filerow = E.rowoff+E.cy;
    int filecol = E.coloff+E.cx;
    erow *row = (filerow >= E.numrows) ? NULL : &E.row[filerow];

    if (!row) {
        if (filerow == E.numrows) {
            editorInsertRow(filerow,"",0);
            goto fixcursor;
        }
        return;
    }
    /* If the cursor is over the current line size, we want to conceptually
     * think it's just over the last character. */
    if (filecol >= row->size) filecol = row->size;
    if (filecol == 0) {
        editorInsertRow(filerow,"",0);
    } else {
        /* We are in the middle of a line. Split it between two rows. */
        editorInsertRow(filerow+1,row->chars+filecol,row->size-filecol);
        row = &E.row[filerow];
        row->chars[filecol] = '\0';
        row->size = filecol;
        editorUpdateRow(row);
    }
fixcursor:
    if (E.cy == E.screenrows-1) {
        E.rowoff++;
    } else {
        E.cy++;
    }
    E.cx = 0;
    E.coloff = 0;
}

/* Delete the char at the current prompt position. */
void editorDelChar(void) {
    int filerow = E.rowoff+E.cy;
    int filecol = E.coloff+E.cx;
    erow *row = (filerow >= E.numrows) ? NULL : &E.row[filerow];

    if (!row || (filecol == 0 && filerow == 0)) return;
    if (filecol == 0) {
        /* Handle the case of column 0, we need to move the current line
         * on the right of the previous one. */
        filecol = E.row[filerow-1].size;
        editorRowAppendString(&E.row[filerow-1],row->chars,row->size);
        editorDelRow(filerow);
        row = NULL;
        if (E.cy == 0)
            E.rowoff--;
        else
            E.cy--;
        E.cx = filecol;
        if (E.cx >= E.screencols) {
            int shift = (E.screencols-E.cx)+1;
            E.cx -= shift;
            E.coloff += shift;
        }
    } else {
        editorRowDelChar(row,filecol-1);
        if (E.cx == 0 && E.coloff)
            E.coloff--;
        else
            E.cx--;
    }
    if (row) editorUpdateRow(row);
    E.dirty++;
}

/* Load the specified program in the editor memory and returns 0 on success
 * or 1 on error. */
int editorOpen(char *filename) {
    FILE *fp;

    E.dirty = 0;
    free(E.filename);
    size_t fnlen = strlen(filename)+1;
    E.filename = malloc(fnlen);
    memcpy(E.filename,filename,fnlen);

    fp = fopen(filename,"r");
    if (!fp) {
        if (errno != ENOENT) {
            perror("Opening file");
            exit(1);
        }
        return 1;
    }

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    while((linelen = getline(&line,&linecap,fp)) != -1) {
        if (linelen && (line[linelen-1] == '\n' || line[linelen-1] == '\r'))
            line[--linelen] = '\0';
        editorInsertRow(E.numrows,line,linelen);
    }
    free(line);
    fclose(fp);
    E.dirty = 0;
    return 0;
}

/* Save the current file on disk. Return 0 on success, 1 on error. */
int editorSave(void) {
    int len;
    char *buf = editorRowsToString(&len);
    int fd = open(E.filename,O_RDWR|O_CREAT,0644);
    if (fd == -1) goto writeerr;

    /* Use truncate + a single write(2) call in order to make saving
     * a bit safer, under the limits of what we can do in a small editor. */
    if (ftruncate(fd,len) == -1) goto writeerr;
    if (write(fd,buf,len) != len) goto writeerr;

    close(fd);
    free(buf);
    E.dirty = 0;
    editorSetStatusMessage("%d bytes written on disk", len);
    return 0;

writeerr:
    free(buf);
    if (fd != -1) close(fd);
    editorSetStatusMessage("Can't save! I/O error: %s",strerror(errno));
    return 1;
}

/* ============================= Terminal update ============================ */

/* We define a very simple "append buffer" structure, that is an heap
 * allocated string where we can append to. This is useful in order to
 * write all the escape sequences in a buffer and flush them to the standard
 * output in a single call, to avoid flickering effects. */
struct abuf {
    char *b;
    int len;
};

#define ABUF_INIT {NULL,0}

void abAppend(struct abuf *ab, const char *s, int len) {
    char *new = realloc(ab->b,ab->len+len);

    if (new == NULL) return;
    memcpy(new+ab->len,s,len);
    ab->b = new;
    ab->len += len;
}

void abFree(struct abuf *ab) {
    free(ab->b);
}

int editorCharToRenderCol(erow *row, int char_col) {
    int rcol = 0;
    for (int i = 0; i < char_col && i < row->size; i++) {
        if (row->chars[i] == TAB) rcol += 8 - (rcol % 8);
        else rcol++;
    }
    return rcol;
}

void editorSelClear(void) { E.sel_active = 0; }

static void editorSelSetAnchor(void) {
    if (!E.sel_active) {
        E.sel_active     = 1;
        E.sel_anchor_row = E.rowoff + E.cy;
        E.sel_anchor_col = E.coloff + E.cx;
    }
}

/* This function writes the whole screen using VT100 escape characters
 * starting from the logical state of the editor in the global state 'E'. */
void editorRefreshScreen(void) {
    int y;
    erow *r;
    char buf[32];
    struct abuf ab = ABUF_INIT;

    abAppend(&ab,"\x1b[?25l",6); /* Hide cursor. */
    abAppend(&ab,"\x1b[H",3); /* Go home. */
    for (y = 0; y < E.screenrows; y++) {
        int filerow = E.rowoff+y;

        if (filerow >= E.numrows) {
            {
                abAppend(&ab,"~\x1b[0K\r\n",7);
            }
            continue;
        }

        r = &E.row[filerow];

        /* Compute selection render-column range for this row. */
        int sel_rcol_start = -1, sel_rcol_end = -1;
        if (E.sel_active) {
            int anc_row = E.sel_anchor_row, anc_col = E.sel_anchor_col;
            int cur_row = E.rowoff + E.cy,  cur_col = E.coloff + E.cx;
            int sr, sc, er, ec;
            if (anc_row < cur_row || (anc_row == cur_row && anc_col <= cur_col)) {
                sr = anc_row; sc = anc_col; er = cur_row; ec = cur_col;
            } else {
                sr = cur_row; sc = cur_col; er = anc_row; ec = anc_col;
            }
            if (filerow >= sr && filerow <= er) {
                int char_start = (filerow == sr) ? sc : 0;
                int char_end   = (filerow == er) ? ec : r->size;
                sel_rcol_start = editorCharToRenderCol(r, char_start);
                sel_rcol_end   = (filerow < er) ? r->rsize
                                                : editorCharToRenderCol(r, char_end);
            }
        }

        int len = r->rsize - E.coloff;
        int current_color = -1;
        int in_selection = 0;
        if (len > 0) {
            if (len > E.screencols) len = E.screencols;
            char *c = r->render+E.coloff;
            unsigned char *hl = r->hl+E.coloff;
            int j;
            for (j = 0; j < len; j++) {
                int rcol = E.coloff + j;

                /* Handle selection highlight transitions. */
                if (sel_rcol_start >= 0) {
                    if (!in_selection && rcol >= sel_rcol_start && rcol < sel_rcol_end) {
                        abAppend(&ab,"\x1b[7m",4);
                        in_selection = 1;
                        current_color = -1;
                    } else if (in_selection && rcol >= sel_rcol_end) {
                        abAppend(&ab,"\x1b[27m",5);
                        in_selection = 0;
                        current_color = -1;
                    }
                }

                if (hl[j] == HL_NONPRINT) {
                    char sym;
                    if (!in_selection) abAppend(&ab,"\x1b[7m",4);
                    if (c[j] <= 26)
                        sym = '@'+c[j];
                    else
                        sym = '?';
                    abAppend(&ab,&sym,1);
                    abAppend(&ab,"\x1b[0m",4);
                    current_color = -1;
                    if (in_selection) abAppend(&ab,"\x1b[7m",4);
                } else if (hl[j] == HL_NORMAL) {
                    if (current_color != -1) {
                        abAppend(&ab,"\x1b[39m",5);
                        current_color = -1;
                    }
                    abAppend(&ab,c+j,1);
                } else {
                    int color = editorSyntaxToColor(hl[j]);
                    if (color != current_color) {
                        char buf[16];
                        int clen = snprintf(buf,sizeof(buf),"\x1b[%dm",color);
                        current_color = color;
                        abAppend(&ab,buf,clen);
                    }
                    abAppend(&ab,c+j,1);
                }
            }
        }
        abAppend(&ab,"\x1b[39m",5);
        if (in_selection) {
            abAppend(&ab,"\x1b[0K",4);  /* erase to EOL with reverse-video bg */
            abAppend(&ab,"\x1b[27m",5); /* reverse off */
        } else {
            abAppend(&ab,"\x1b[0K",4);
        }
        abAppend(&ab,"\r\n",2);
    }

    /* Create a two rows status. First row: */
    abAppend(&ab,"\x1b[0K",4);
    abAppend(&ab,"\x1b[7m",4);
    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
        E.filename, E.numrows, E.dirty ? "(modified)" : "");
    int rlen = snprintf(rstatus, sizeof(rstatus),
        "%d/%d",E.rowoff+E.cy+1,E.numrows);
    if (len > E.screencols) len = E.screencols;
    abAppend(&ab,status,len);
    while(len < E.screencols) {
        if (E.screencols - len == rlen) {
            abAppend(&ab,rstatus,rlen);
            break;
        } else {
            abAppend(&ab," ",1);
            len++;
        }
    }
    abAppend(&ab,"\x1b[0m\r\n",6);

    /* Second row depends on E.statusmsg and the status message update time. */
    abAppend(&ab,"\x1b[0K",4);
    int msglen = strlen(E.statusmsg);
    if (msglen && time(NULL)-E.statusmsg_time < 5)
        abAppend(&ab,E.statusmsg,msglen <= E.screencols ? msglen : E.screencols);

    /* Put cursor at its current position. Note that the horizontal position
     * at which the cursor is displayed may be different compared to 'E.cx'
     * because of TABs. */
    int j;
    int cx = 1;
    int filerow = E.rowoff+E.cy;
    erow *row = (filerow >= E.numrows) ? NULL : &E.row[filerow];
    if (row) {
        for (j = E.coloff; j < (E.cx+E.coloff); j++) {
            if (j < row->size && row->chars[j] == TAB) cx += 7-((cx)%8);
            cx++;
        }
    }
    snprintf(buf,sizeof(buf),"\x1b[%d;%dH",E.cy+1,cx);
    abAppend(&ab,buf,strlen(buf));
    abAppend(&ab,"\x1b[?25h",6); /* Show cursor. */
    write(STDOUT_FILENO,ab.b,ab.len);
    abFree(&ab);
}

/* Set an editor status message for the second line of the status, at the
 * end of the screen. */
void editorSetStatusMessage(const char *fmt, ...) {
    va_list ap;
    va_start(ap,fmt);
    vsnprintf(E.statusmsg,sizeof(E.statusmsg),fmt,ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}

/* =============================== Find mode ================================ */

#define QUIP_QUERY_LEN 256

void editorFind(int fd) {
    char query[QUIP_QUERY_LEN+1] = {0};
    int qlen = 0;
    int last_match = -1; /* Last line where a match was found. -1 for none. */
    int find_next = 0; /* if 1 search next, if -1 search prev. */
    int saved_hl_line = -1;  /* No saved HL */
    char *saved_hl = NULL;

#define FIND_RESTORE_HL do { \
    if (saved_hl) { \
        memcpy(E.row[saved_hl_line].hl,saved_hl, E.row[saved_hl_line].rsize); \
        free(saved_hl); \
        saved_hl = NULL; \
    } \
} while (0)

    /* Save the cursor position in order to restore it later. */
    int saved_cx = E.cx, saved_cy = E.cy;
    int saved_coloff = E.coloff, saved_rowoff = E.rowoff;

    while(1) {
        editorSetStatusMessage(
            "Search: %s (Use ESC/Arrows/Enter)", query);
        editorRefreshScreen();

        int c = editorReadKey(fd);
        if (c == DEL_KEY || c == CTRL_H || c == BACKSPACE) {
            if (qlen != 0) query[--qlen] = '\0';
            last_match = -1;
        } else if (c == ESC || c == ENTER) {
            if (c == ESC) {
                E.cx = saved_cx; E.cy = saved_cy;
                E.coloff = saved_coloff; E.rowoff = saved_rowoff;
            }
            FIND_RESTORE_HL;
            editorSetStatusMessage("");
            return;
        } else if (c == ARROW_RIGHT || c == ARROW_DOWN) {
            find_next = 1;
        } else if (c == ARROW_LEFT || c == ARROW_UP) {
            find_next = -1;
        } else if (isprint(c)) {
            if (qlen < QUIP_QUERY_LEN) {
                query[qlen++] = c;
                query[qlen] = '\0';
                last_match = -1;
            }
        }

        /* Search occurrence. */
        if (last_match == -1) find_next = 1;
        if (find_next) {
            char *match = NULL;
            int match_offset = 0;
            int i, current = last_match;

            for (i = 0; i < E.numrows; i++) {
                current += find_next;
                if (current == -1) current = E.numrows-1;
                else if (current == E.numrows) current = 0;
                match = strstr(E.row[current].render,query);
                if (match) {
                    match_offset = match-E.row[current].render;
                    break;
                }
            }
            find_next = 0;

            /* Highlight */
            FIND_RESTORE_HL;

            if (match) {
                erow *row = &E.row[current];
                last_match = current;
                if (row->hl) {
                    saved_hl_line = current;
                    saved_hl = malloc(row->rsize);
                    memcpy(saved_hl,row->hl,row->rsize);
                    memset(row->hl+match_offset,HL_MATCH,qlen);
                }
                E.cy = 0;
                E.cx = match_offset;
                E.rowoff = current;
                E.coloff = 0;
                /* Scroll horizontally as needed. */
                if (E.cx > E.screencols) {
                    int diff = E.cx - E.screencols;
                    E.cx -= diff;
                    E.coloff += diff;
                }
            }
        }
    }
}

/* ========================= Editor events handling  ======================== */

/* Handle cursor position change because arrow keys were pressed. */
void editorMoveCursor(int key) {
    int filerow = E.rowoff+E.cy;
    int filecol = E.coloff+E.cx;
    int rowlen;
    erow *row = (filerow >= E.numrows) ? NULL : &E.row[filerow];

    switch(key) {
    case ARROW_LEFT:
        if (E.cx == 0) {
            if (E.coloff) {
                E.coloff--;
            } else {
                if (filerow > 0) {
                    E.cy--;
                    E.cx = E.row[filerow-1].size;
                    if (E.cx > E.screencols-1) {
                        E.coloff = E.cx-E.screencols+1;
                        E.cx = E.screencols-1;
                    }
                }
            }
        } else {
            E.cx -= 1;
        }
        break;
    case ARROW_RIGHT:
        if (row && filecol < row->size) {
            if (E.cx == E.screencols-1) {
                E.coloff++;
            } else {
                E.cx += 1;
            }
        } else if (row && filecol == row->size) {
            E.cx = 0;
            E.coloff = 0;
            if (E.cy == E.screenrows-1) {
                E.rowoff++;
            } else {
                E.cy += 1;
            }
        }
        break;
    case ARROW_UP:
        if (E.cy == 0) {
            if (E.rowoff) E.rowoff--;
        } else {
            E.cy -= 1;
        }
        break;
    case ARROW_DOWN:
        if (filerow < E.numrows) {
            if (E.cy == E.screenrows-1) {
                E.rowoff++;
            } else {
                E.cy += 1;
            }
        }
        break;
    }
    /* Fix cx if the current line has not enough chars. */
    filerow = E.rowoff+E.cy;
    filecol = E.coloff+E.cx;
    row = (filerow >= E.numrows) ? NULL : &E.row[filerow];
    rowlen = row ? row->size : 0;
    if (filecol > rowlen) {
        E.cx -= filecol-rowlen;
        if (E.cx < 0) {
            E.coloff += E.cx;
            E.cx = 0;
        }
    }
}

static void editorSetCol(int new_col) {
    if (new_col < E.screencols) {
        E.cx = new_col;
        E.coloff = 0;
    } else {
        E.coloff = new_col - E.screencols + 1;
        E.cx = E.screencols - 1;
    }
}

void editorMoveWordRight(void) {
    int filerow = E.rowoff + E.cy;
    if (filerow >= E.numrows) return;
    erow *row = &E.row[filerow];
    int pos = E.coloff + E.cx;

    if (pos >= row->size) {
        if (filerow + 1 < E.numrows) {
            editorMoveCursor(ARROW_DOWN);
            E.cx = 0; E.coloff = 0;
        }
        return;
    }
    while (pos < row->size &&  is_separator(row->chars[pos])) pos++;
    while (pos < row->size && !is_separator(row->chars[pos])) pos++;
    editorSetCol(pos);
}

void editorMoveWordLeft(void) {
    int filerow = E.rowoff + E.cy;
    if (filerow >= E.numrows) return;
    erow *row = &E.row[filerow];
    int pos = E.coloff + E.cx;

    if (pos == 0) {
        if (filerow > 0) {
            editorMoveCursor(ARROW_UP);
            editorSetCol(E.row[filerow - 1].size);
        }
        return;
    }
    pos--;
    while (pos > 0 &&  is_separator(row->chars[pos])) pos--;
    while (pos > 0 && !is_separator(row->chars[pos - 1])) pos--;
    editorSetCol(pos);
}

void editorMoveLineStart(void) {
    int filerow = E.rowoff + E.cy;
    if (filerow >= E.numrows) { E.cx = 0; E.coloff = 0; return; }
    erow *row = &E.row[filerow];

    int indent = 0;
    while (indent < row->size && isspace(row->chars[indent])) indent++;

    int filecol = E.coloff + E.cx;
    if (filecol != indent)
        editorSetCol(indent);
    else { E.cx = 0; E.coloff = 0; }
}

void editorMoveLineEnd(void) {
    int filerow = E.rowoff + E.cy;
    if (filerow >= E.numrows) return;
    editorSetCol(E.row[filerow].size);
}

void editorMoveFileStart(void) {
    E.cx = 0; E.cy = 0; E.rowoff = 0; E.coloff = 0;
}

void editorMoveFileEnd(void) {
    if (E.numrows == 0) return;
    int last = E.numrows - 1;
    if (last < E.screenrows) {
        E.cy = last; E.rowoff = 0;
    } else {
        E.rowoff = last - E.screenrows + 1;
        E.cy = E.screenrows - 1;
    }
    editorSetCol(E.row[last].size);
}

void editorToggleLineComment(int filerow) {
    if (filerow >= E.numrows || E.syntax == NULL ||
        E.syntax->singleline_comment_start[0] == '\0') return;

    erow *row = &E.row[filerow];
    char *prefix = E.syntax->singleline_comment_start;

    int indent = 0;
    while (indent < row->size && isspace(row->chars[indent])) indent++;

    if (indent + 1 < row->size &&
        row->chars[indent]   == prefix[0] &&
        row->chars[indent+1] == prefix[1]) {
        editorRowDelChar(row, indent);
        editorRowDelChar(row, indent);
    } else {
        editorRowInsertChar(row, indent,   prefix[0]);
        editorRowInsertChar(row, indent+1, prefix[1]);
    }
}

void editorDeleteSelection(void) {
    if (!E.sel_active) return;

    int anc_row = E.sel_anchor_row, anc_col = E.sel_anchor_col;
    int cur_row = E.rowoff + E.cy,  cur_col = E.coloff + E.cx;
    int sr, sc, er, ec;
    if (anc_row < cur_row || (anc_row == cur_row && anc_col <= cur_col)) {
        sr = anc_row; sc = anc_col; er = cur_row; ec = cur_col;
    } else {
        sr = cur_row; sc = cur_col; er = anc_row; ec = anc_col;
    }

    if (sr == er && sc == ec) { editorSelClear(); return; }

    if (sr == er) {
        erow *row = &E.row[sr];
        int count = ec - sc;
        memmove(row->chars + sc, row->chars + ec, row->size - ec + 1);
        row->size -= count;
        editorUpdateRow(row);
        E.dirty++;
    } else {
        E.row[sr].chars[sc] = '\0';
        E.row[sr].size = sc;
        editorRowAppendString(&E.row[sr], E.row[er].chars + ec, E.row[er].size - ec);
        for (int i = er; i > sr; i--)
            editorDelRow(i);
    }

    if (sr < E.rowoff) {
        E.rowoff = sr; E.cy = 0;
    } else if (sr >= E.rowoff + E.screenrows) {
        E.rowoff = sr - E.screenrows + 1;
        E.cy = E.screenrows - 1;
    } else {
        E.cy = sr - E.rowoff;
    }
    editorSetCol(sc);
    E.sel_active = 0;
}

static void editorClampCursorCol(void) {
    int file_row = E.rowoff + E.cy;
    if (file_row >= E.numrows) return;
    erow *row = &E.row[file_row];
    int cur_col = E.coloff + E.cx;
    if (cur_col > row->size) editorSetCol(row->size);
}

/* Moves cursor to E.mouse_cx/cy, converting render column → file column. */
static void editorMouseSetCursor(void) {
    if (E.mouse_cy >= 0 && E.mouse_cy < E.screenrows)
        E.cy = E.mouse_cy;
    if (E.mouse_cx >= 0 && E.mouse_cx < E.screencols) {
        int file_row = E.rowoff + E.cy;
        if (file_row < E.numrows) {
            erow *row = &E.row[file_row];
            int rcol = E.coloff + E.mouse_cx;
            int fc = 0, rc = 0;
            while (fc < row->size && rc < rcol) {
                if (row->chars[fc] == TAB) rc += 8 - (rc % 8);
                else rc++;
                fc++;
            }
            editorSetCol(fc);
        } else {
            editorSetCol(0);
        }
    }
}

/* ========================= Clipboard ======================================*/

/* Returns malloc'd string of selected text, sets *outlen. Caller frees.
 * Returns NULL if no selection or zero-length selection. */
static char *editorGetSelection(int *outlen) {
    if (!E.sel_active) return NULL;

    int anc_row = E.sel_anchor_row, anc_col = E.sel_anchor_col;
    int cur_row = E.rowoff + E.cy,  cur_col = E.coloff + E.cx;
    int sr, sc, er, ec;
    if (anc_row < cur_row || (anc_row == cur_row && anc_col <= cur_col)) {
        sr = anc_row; sc = anc_col; er = cur_row; ec = cur_col;
    } else {
        sr = cur_row; sc = cur_col; er = anc_row; ec = anc_col;
    }
    if (sr == er && sc == ec) return NULL;

    if (sr == er) {
        int count = ec - sc;
        char *buf = malloc(count + 1);
        memcpy(buf, E.row[sr].chars + sc, count);
        buf[count] = '\0';
        *outlen = count;
        return buf;
    }

    /* Multi-row: calculate total size then fill buffer. */
    int size = (E.row[sr].size - sc) + 1; /* first row chars + newline */
    for (int i = sr + 1; i < er; i++)
        size += E.row[i].size + 1;        /* middle rows + newlines */
    size += ec;                            /* last row partial */

    char *buf = malloc(size + 1);
    int pos = 0;
    memcpy(buf + pos, E.row[sr].chars + sc, E.row[sr].size - sc);
    pos += E.row[sr].size - sc;
    buf[pos++] = '\n';
    for (int i = sr + 1; i < er; i++) {
        memcpy(buf + pos, E.row[i].chars, E.row[i].size);
        pos += E.row[i].size;
        buf[pos++] = '\n';
    }
    memcpy(buf + pos, E.row[er].chars, ec);
    pos += ec;
    buf[pos] = '\0';
    *outlen = pos;
    return buf;
}

/* Copies selection to internal clipboard and sends it to terminal via OSC 52. */
static void editorCopySelection(void) {
    int len;
    char *text = editorGetSelection(&len);
    if (!text) return;

    free(E.clipboard);
    E.clipboard     = text;
    E.clipboard_len = len;

    char *b64 = base64_encode((unsigned char *)text, len);
    int seqlen = 8 + (int)strlen(b64); /* ESC ] 52 ; c ; <b64> BEL */
    char *seq  = malloc(seqlen + 1);
    snprintf(seq, seqlen + 1, "\x1b]52;c;%s\x07", b64);
    write(STDOUT_FILENO, seq, seqlen);
    free(seq);
    free(b64);
}

/* Reads one byte from stdin with a timeout of `ms` milliseconds.
 * Returns 1 on success, 0 on timeout, -1 on error. */
static int readByteTimeout(unsigned char *ch, int ms) {
    fd_set fds;
    struct timeval tv;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    tv.tv_sec  = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    int r = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
    if (r <= 0) return r;
    return (read(STDIN_FILENO, ch, 1) == 1) ? 1 : -1;
}

/* Queries terminal clipboard via OSC 52.
 * Returns malloc'd decoded text (caller frees), or NULL on failure/timeout.
 *
 * Searches stdin for the literal prefix "\x1b]52;c;" so that everything
 * before it — interleaved mouse events, tmux DCS passthrough wrappers, etc.
 * — is consumed and never leaks into editorReadKey. */
static char *editorReadClipboard(int *outlen) {
    unsigned char ch;

    /* Drain anything already buffered in stdin. */
    while (readByteTimeout(&ch, 0) == 1) {}

    write(STDOUT_FILENO, "\x1b]52;c;?\x07", 9);

    /* Byte-by-byte KMP-lite search for "\x1b]52;c;" within a 500 ms window.
     * This tolerates mouse events and tmux DCS wrappers arriving before the
     * real response, consuming them completely so they never reach editorReadKey. */
    const char     target[] = "\x1b]52;c;";
    const int      tlen     = (int)(sizeof(target) - 1);
    int            match    = 0;
    struct timeval t0, tnow;
    gettimeofday(&t0, NULL);

    while (match < tlen) {
        gettimeofday(&tnow, NULL);
        int elapsed = (int)((tnow.tv_sec  - t0.tv_sec)  * 1000 +
                            (tnow.tv_usec - t0.tv_usec) / 1000);
        if (elapsed >= 500) return NULL;

        if (readByteTimeout(&ch, 100) != 1) continue;

        if (ch == (unsigned char)target[match]) {
            match++;
        } else {
            /* Mismatch: restart, but check whether this byte opens a new match. */
            match = (ch == (unsigned char)target[0]) ? 1 : 0;
        }
    }

    /* Found the prefix.  Read the base64 payload until BEL (0x07) or ST (ESC \). */
    char buf[65536];
    int  i = 0;
    while (i < (int)(sizeof(buf) - 1)) {
        if (readByteTimeout(&ch, 200) != 1) break;
        if (ch == 0x07) {
            /* BEL terminator.  Drain any trailing DCS-closing bytes quickly. */
            unsigned char extra;
            while (readByteTimeout(&extra, 50) == 1) {}
            break;
        }
        if (ch == 0x1b) {
            /* ST (ESC \) or an escaped inner byte in a DCS wrapper.
             * Either way the payload is finished; drain whatever follows. */
            unsigned char extra;
            while (readByteTimeout(&extra, 50) == 1) {}
            break;
        }
        buf[i++] = (char)ch;
    }
    buf[i] = '\0';

    if (i == 0) return NULL;
    return base64_decode(buf, outlen);
}

/* Inserts text at cursor, splitting on newlines. */
static void editorPasteText(const char *text, int len) {
    for (int i = 0; i < len; i++) {
        if (text[i] == '\n')
            editorInsertNewline();
        else
            editorInsertChar((unsigned char)text[i]);
    }
}

/* Pastes from system clipboard (OSC 52), falling back to internal clipboard. */
static void editorPaste(void) {
    int len = 0;
    char *text = editorReadClipboard(&len);
    if (text) {
        free(E.clipboard);
        E.clipboard     = text;
        E.clipboard_len = len;
        editorPasteText(text, len);
    } else if (E.clipboard) {
        editorPasteText(E.clipboard, E.clipboard_len);
    }
}

/* ========================= Undo / Redo helpers ============================ */

void editorPushSnapshot(void) {
    /* Discard any redo entries above undo_cur. */
    for (int i = undo_cur + 1; i < undo_count; i++)
        free(undo_hist[i].text);
    undo_count = undo_cur + 1;

    /* If the history is full, drop the oldest entry. */
    if (undo_count == UNDO_HISTORY_MAX) {
        free(undo_hist[0].text);
        memmove(&undo_hist[0], &undo_hist[1],
                (UNDO_HISTORY_MAX - 1) * sizeof(editorSnapshot));
        undo_count--;
        if (undo_cur > 0) undo_cur--;
        if (undo_saved > 0) undo_saved--;
        else if (undo_saved == 0) undo_saved = -1;
    }

    int len;
    char *text = editorRowsToString(&len);
    undo_hist[undo_count].text   = text;
    undo_hist[undo_count].len    = len;
    undo_hist[undo_count].cx     = E.cx;
    undo_hist[undo_count].cy     = E.cy;
    undo_hist[undo_count].rowoff = E.rowoff;
    undo_hist[undo_count].coloff = E.coloff;
    undo_cur = undo_count;
    undo_count++;
}

void editorRestoreSnapshot(editorSnapshot *snap) {
    for (int i = 0; i < E.numrows; i++) {
        free(E.row[i].render);
        free(E.row[i].chars);
        free(E.row[i].hl);
    }
    free(E.row);
    E.row    = NULL;
    E.numrows = 0;

    /* Rebuild rows from serialized text (rows separated by '\n'). */
    char *p   = snap->text;
    int   rem = snap->len;
    while (rem > 0) {
        char *nl      = memchr(p, '\n', rem);
        int   linelen = nl ? (int)(nl - p) : rem;
        editorInsertRow(E.numrows, p, linelen);
        if (!nl) break;
        rem -= linelen + 1;
        p   += linelen + 1;
    }

    E.cx     = snap->cx;
    E.cy     = snap->cy;
    E.rowoff = snap->rowoff;
    E.coloff = snap->coloff;
    E.dirty  = (undo_cur != undo_saved);
    E.sel_active = 0;
}

void editorUndo(void) {
    if (undo_cur <= 0) {
        editorSetStatusMessage("Nothing to undo.");
        return;
    }
    undo_cur--;
    editorRestoreSnapshot(&undo_hist[undo_cur]);
    undo_last_insert = 0;
}

void editorRedo(void) {
    if (undo_cur >= undo_count - 1) {
        editorSetStatusMessage("Nothing to redo.");
        return;
    }
    undo_cur++;
    editorRestoreSnapshot(&undo_hist[undo_cur]);
    undo_last_insert = 0;
}

/* ========================================================================= */

/* Process events arriving from the standard input, which is, the user
 * is typing stuff on the terminal. */
#define QUIP_QUIT_TIMES 3
void editorProcessKeypress(int fd) {
    /* When the file is modified, requires Ctrl-q to be pressed N times
     * before actually quitting. */
    static int quit_times = QUIP_QUIT_TIMES;

    int c = editorReadKey(fd);
    switch(c) {
    case CTRL_Z:        /* Ctrl-z — undo */
        editorUndo();
        break;
    case CTRL_Y:        /* Ctrl-y — redo */
        editorRedo();
        break;
    case ENTER:         /* Enter */
        editorPushSnapshot();
        undo_last_insert = 0;
        if (E.sel_active) editorDeleteSelection();
        editorInsertNewline();
        break;
    case CTRL_C:        /* Ctrl-c — copy selection */
        if (E.sel_active) editorCopySelection();
        break;
    case CTRL_X:        /* Ctrl-x — cut selection */
        if (E.sel_active) {
            editorPushSnapshot();
            undo_last_insert = 0;
            editorCopySelection();
            editorDeleteSelection();
        }
        break;
    case CTRL_V:        /* Ctrl-v — paste */
        editorPushSnapshot();
        undo_last_insert = 0;
        editorSelClear();
        editorPaste();
        break;
    case CTRL_Q:        /* Ctrl-q */
        /* Quit if the file was already saved. */
        if (E.dirty && quit_times) {
            editorSetStatusMessage("WARNING!!! File has unsaved changes. "
                "Press Ctrl-Q %d more times to quit.", quit_times);
            quit_times--;
            return;
        }
        exit(0);
        break;
    case CTRL_S:        /* Ctrl-s */
        editorSave();
        undo_saved = undo_cur;
        break;
    case CTRL_F:
        editorFind(fd);
        break;
    case CTRL_SLASH:
        editorPushSnapshot();
        undo_last_insert = 0;
        editorSelClear();
        editorToggleLineComment(E.rowoff + E.cy);
        break;
    case BACKSPACE:     /* Backspace */
    case CTRL_H:        /* Ctrl-h */
    case DEL_KEY:
        editorPushSnapshot();
        undo_last_insert = 0;
        if (E.sel_active) editorDeleteSelection();
        else              editorDelChar();
        break;
    case MOUSE_CLICK:
        undo_last_insert = 0;
        editorMouseSetCursor();
        E.sel_active     = 1;
        E.sel_anchor_row = E.rowoff + E.cy;
        E.sel_anchor_col = E.coloff + E.cx;
        break;
    case MOUSE_DRAG:
        if (E.sel_active) editorMouseSetCursor();
        break;
    case MOUSE_RELEASE:
        editorMouseSetCursor();
        /* If anchor == cursor the user just clicked; clear the selection. */
        if (E.sel_anchor_row == E.rowoff + E.cy &&
            E.sel_anchor_col == E.coloff + E.cx)
            E.sel_active = 0;
        break;
    case SCROLL_UP: {
        undo_last_insert = 0;
        int file_row = E.rowoff + E.cy;
        int new_rowoff = E.rowoff - 3;
        if (new_rowoff < 0) new_rowoff = 0;
        E.rowoff = new_rowoff;
        E.cy = file_row - E.rowoff;
        if (E.cy >= E.screenrows) E.cy = E.screenrows - 1;
        if (E.cy < 0) E.cy = 0;
        editorClampCursorCol();
        break;
    }
    case SCROLL_DOWN: {
        undo_last_insert = 0;
        int file_row = E.rowoff + E.cy;
        int max_rowoff = (E.numrows > E.screenrows) ? E.numrows - E.screenrows : 0;
        int new_rowoff = E.rowoff + 3;
        if (new_rowoff > max_rowoff) new_rowoff = max_rowoff;
        E.rowoff = new_rowoff;
        E.cy = file_row - E.rowoff;
        if (E.cy >= E.screenrows) E.cy = E.screenrows - 1;
        if (E.cy < 0) E.cy = 0;
        editorClampCursorCol();
        break;
    }
    case PAGE_UP:
    case PAGE_DOWN:
        undo_last_insert = 0;
        editorSelClear();
        if (c == PAGE_UP && E.cy != 0)
            E.cy = 0;
        else if (c == PAGE_DOWN && E.cy != E.screenrows-1)
            E.cy = E.screenrows-1;
        {
        int times = E.screenrows;
        while(times--)
            editorMoveCursor(c == PAGE_UP ? ARROW_UP:
                                            ARROW_DOWN);
        }
        break;

    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
        undo_last_insert = 0;
        editorSelClear();
        editorMoveCursor(c);
        break;
    case SHIFT_ARROW_UP:
        undo_last_insert = 0;
        editorSelSetAnchor(); editorMoveCursor(ARROW_UP);    break;
    case SHIFT_ARROW_DOWN:
        undo_last_insert = 0;
        editorSelSetAnchor(); editorMoveCursor(ARROW_DOWN);  break;
    case SHIFT_ARROW_LEFT:
        undo_last_insert = 0;
        editorSelSetAnchor(); editorMoveCursor(ARROW_LEFT);  break;
    case SHIFT_ARROW_RIGHT:
        undo_last_insert = 0;
        editorSelSetAnchor(); editorMoveCursor(ARROW_RIGHT); break;
    case SHIFT_ALT_ARROW_LEFT:
        undo_last_insert = 0;
        editorSelSetAnchor(); editorMoveWordLeft();           break;
    case SHIFT_ALT_ARROW_RIGHT:
        undo_last_insert = 0;
        editorSelSetAnchor(); editorMoveWordRight();          break;
    case SHIFT_ALT_ARROW_UP:
        undo_last_insert = 0;
        editorSelSetAnchor(); editorMoveCursor(ARROW_UP);    break;
    case SHIFT_ALT_ARROW_DOWN:
        undo_last_insert = 0;
        editorSelSetAnchor(); editorMoveCursor(ARROW_DOWN);  break;
    case SHIFT_CTRL_ARROW_LEFT:
        undo_last_insert = 0;
        editorSelSetAnchor(); editorMoveLineStart();          break;
    case SHIFT_CTRL_ARROW_RIGHT:
        undo_last_insert = 0;
        editorSelSetAnchor(); editorMoveLineEnd();            break;
    case SHIFT_CTRL_ARROW_UP:
        undo_last_insert = 0;
        editorSelSetAnchor(); editorMoveFileStart();          break;
    case SHIFT_CTRL_ARROW_DOWN:
        undo_last_insert = 0;
        editorSelSetAnchor(); editorMoveFileEnd();            break;
    case CTRL_ARROW_UP:
        undo_last_insert = 0;
        editorSelClear(); editorMoveFileStart();              break;
    case CTRL_ARROW_DOWN:
        undo_last_insert = 0;
        editorSelClear(); editorMoveFileEnd();                break;
    case CTRL_ARROW_LEFT:
        undo_last_insert = 0;
        editorSelClear(); editorMoveLineStart();              break;
    case CTRL_ARROW_RIGHT:
        undo_last_insert = 0;
        editorSelClear(); editorMoveLineEnd();                break;
    case ALT_ARROW_LEFT:
        undo_last_insert = 0;
        editorSelClear(); editorMoveWordLeft();               break;
    case ALT_ARROW_RIGHT:
        undo_last_insert = 0;
        editorSelClear(); editorMoveWordRight();              break;
    case ALT_ARROW_UP:
        undo_last_insert = 0;
        editorSelClear(); editorMoveCursor(ARROW_UP);         break;
    case ALT_ARROW_DOWN:
        undo_last_insert = 0;
        editorSelClear(); editorMoveCursor(ARROW_DOWN);       break;
    case CTRL_L:
        undo_last_insert = 0;
        break;
    case ESC:
        undo_last_insert = 0;
        editorSelClear();
        break;
    default:
        if (!undo_last_insert || E.sel_active) {
            editorPushSnapshot();
            undo_last_insert = 1;
        }
        if (E.sel_active) editorDeleteSelection();
        editorInsertChar(c);
        break;
    }

    quit_times = QUIP_QUIT_TIMES; /* Reset it to the original value. */
}

int editorFileWasModified(void) {
    return E.dirty;
}

void updateWindowSize(void) {
    if (getWindowSize(STDIN_FILENO,STDOUT_FILENO,
                      &E.screenrows,&E.screencols) == -1) {
        perror("Unable to query the screen for size (columns / rows)");
        exit(1);
    }
    E.screenrows -= 2; /* Get room for status bar. */
}

void handleSigWinCh(int unused __attribute__((unused))) {
    updateWindowSize();
    if (E.cy > E.screenrows) E.cy = E.screenrows - 1;
    if (E.cx > E.screencols) E.cx = E.screencols - 1;
    editorRefreshScreen();
}

void initEditor(void) {
    E.cx = 0;
    E.cy = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.numrows = 0;
    E.row = NULL;
    E.dirty = 0;
    E.filename = NULL;
    E.syntax = NULL;
    E.sel_active = 0;
    updateWindowSize();
    signal(SIGWINCH, handleSigWinCh);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr,"Usage: quip <filename>\n");
        exit(1);
    }

    initEditor();
    editorSelectSyntaxHighlight(argv[1]);
    editorOpen(argv[1]);
    editorPushSnapshot();
    undo_saved = undo_cur;
    E.dirty = 0;
    enableRawMode(STDIN_FILENO);
    editorSetStatusMessage("");
    while(1) {
        editorRefreshScreen();
        editorProcessKeypress(STDIN_FILENO);
    }
    return 0;
}
