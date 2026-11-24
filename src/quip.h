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

/* Declarations shared by the modules in src/. Every .c file includes this
 * first: it sets the feature macros the system headers need. */
#ifndef QUIP_H
#define QUIP_H

#ifndef QUIP_VERSION /* "make" sets it from git describe. */
#define QUIP_VERSION "0.0.1"
#endif
#define QUIP_TAB_STOP 2 /* Tab width; the Tab key inserts spaces up to it. */

#ifdef __linux__
#define _POSIX_C_SOURCE 200809L
#endif

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "vt.h"

/* Syntax highlight types */
#define HL_NORMAL    0
#define HL_NONPRINT  1
#define HL_COMMENT   2 /* Single line comment. */
#define HL_MLCOMMENT 3 /* Multi-line comment. */
#define HL_KEYWORD1  4
#define HL_KEYWORD2  5
#define HL_STRING    6
#define HL_NUMBER    7
#define HL_MATCH     8 /* Search match. */

#define HL_HIGHLIGHT_STRINGS   (1 << 0)
#define HL_HIGHLIGHT_NUMBERS   (1 << 1)
#define HL_NESTED_COMMENTS     (1 << 2) /* Block comments nest, as in OCaml. */
#define HL_MULTILINE_STRINGS   (1 << 3) /* "..." strings may span rows. */
#define HL_QUOTED_STRINGS      (1 << 4) /* OCaml {id|...|id} strings. */
/* ' opens a char literal only when one fits; otherwise it is a type
 * variable ('a) or an identifier prime (x'). */
#define HL_PRIME_QUOTES        (1 << 5)
/* A line comment starts only at the start of a word, as # in shell. */
#define HL_WORD_START_COMMENTS (1 << 6)
/* """...""" and '''...''' strings, which may span rows (Python docstrings,
 * Java text blocks). */
#define HL_TRIPLE_QUOTES       (1 << 7)

/* Lexer state at the end of a row, carried into the next row. The low byte
 * is the kind; the rest is an argument: the comment nesting depth, the
 * quote char of an open string or triple quoted string, or the hash of a
 * quoted string's id. */
#define HL_ST_NORMAL     0
#define HL_ST_COMMENT    1
#define HL_ST_STRING     2
#define HL_ST_QSTRING    3
#define HL_ST_TSTRING    4
#define HL_ST(kind, arg) ((kind) | ((arg) << 8))
#define HL_ST_INVALID    (-1) /* Never highlighted yet; differs from any state. */

/* A comment delimiter of any length; len 0 when the language has none. */
typedef struct hldelim {
    char *s;
    int len;
} hldelim;

struct editorSyntax {
    char *id;          /* Language file name without ".lang", e.g. "c". */
    char **extensions; /* ".c" style suffixes, NULL terminated. */
    char **filenames;  /* Exact base names ("Makefile"), NULL terminated. */
    char **keywords;   /* "word", or "word|" for a type, NULL terminated. */
    hldelim line_comment;
    hldelim block_start;
    hldelim block_end;
    int flags;
};

/* This structure represents a single line of the file we are editing. */
typedef struct erow {
    int idx;           /* Row index in the file, zero-based. */
    int size;          /* Size of the row, excluding the null term. */
    int rsize;         /* Size of the rendered row. */
    int chars_cap;     /* Allocated capacity of 'chars'. */
    int render_cap;    /* Allocated capacity of 'render'. */
    int hl_cap;        /* Allocated capacity of 'hl'. */
    char *chars;       /* Row content. */
    char *render;      /* Row content "rendered" for screen (for TABs). */
    unsigned char *hl; /* Syntax highlight type for each character in render.*/
    int hl_state;      /* Lexer state at the end of the row (HL_ST_*) as of
                          the last syntax highlight. */
} erow;

typedef struct hlcolor {
    int r, g, b;
} hlcolor;

struct editorConfig {
    int cx, cy;     /* Cursor x and y position in characters */
    int rowoff;     /* Offset of row displayed. */
    int coloff;     /* Offset of column displayed. */
    int screenrows; /* Number of rows that we can show */
    int screencols; /* Number of text cols we can show (excludes sidebar/gutter). */
    int termcols;   /* Physical terminal width in columns. */
    int ln_width;   /* Columns reserved for the line number gutter (0 if hidden). */
    int numrows;    /* Number of rows */
    int rowcap;     /* Allocated capacity of the row array. */
    int rawmode;    /* Is terminal raw mode enabled? */
    erow *row;      /* Rows */
    int dirty;      /* File modified but not saved. */
    char *filename; /* Currently open filename */
    char statusmsg[80];
    time_t statusmsg_time;
    struct editorSyntax *syntax;            /* Current syntax highlight, or NULL. */
    int sel_active;                         /* 1 if a selection is live. */
    int sel_anchor_row;                     /* File row where the selection was anchored. */
    int sel_anchor_col;                     /* File col where the selection was anchored. */
    int mouse_cx, mouse_cy;                 /* Screen position of last mouse click. */
    int click_count;                        /* Left presses in the current multi-click gesture. */
    int click_cx, click_cy;                 /* Screen cell of the last left press. */
    struct timespec click_time;             /* When the last left press arrived. */
    int unit_sr, unit_sc, unit_er, unit_ec; /* Word or line a multi-click
                                               picked: (sr,sc) up to (er,ec). */
    char *clipboard;                        /* Internal clipboard buffer (may contain '\n'). */
    int clipboard_len;                      /* Byte count of clipboard contents. */
    int ft_open;                            /* File-tree sidebar visible? */
    int ft_width;                           /* Columns reserved for the sidebar (0 when closed). */
    int ft_focus;           /* 1 = keys drive the tree, 0 = keys drive the editor. */
    struct ftnode *ft_root; /* Tree root, lazily built on first open. */
    struct ftnode **ft_vis; /* Flattened visible nodes, in display order. */
    int ft_vcount;          /* Number of entries in ft_vis. */
    int ft_vcap;            /* Allocated capacity of ft_vis. */
    int ft_sel;             /* Selected index into ft_vis. */
    int ft_scroll;          /* First ft_vis index shown at the top of the pane. */
};

/* A file-tree node; the definition is private to filetree.c. */
typedef struct ftnode ftnode;

/* The editor's whole state, defined in main.c. */
extern struct editorConfig E;

enum KEY_ACTION {
    KEY_NULL = 0,    /* NULL */
    CTRL_E = 5,      /* Ctrl-e — toggle file tree */
    CTRL_C = 3,      /* Ctrl-c */
    CTRL_D = 4,      /* Ctrl-d */
    CTRL_F = 6,      /* Ctrl-f */
    CTRL_H = 8,      /* Ctrl-h */
    TAB = 9,         /* Tab */
    CTRL_L = 12,     /* Ctrl+l */
    ENTER = 13,      /* Enter */
    CTRL_Q = 17,     /* Ctrl-q */
    CTRL_S = 19,     /* Ctrl-s */
    CTRL_U = 21,     /* Ctrl-u */
    CTRL_V = 22,     /* Ctrl-v */
    CTRL_X = 24,     /* Ctrl-x */
    CTRL_Y = 25,     /* Ctrl-y */
    CTRL_Z = 26,     /* Ctrl-z */
    ESC = 27,        /* Escape */
    CTRL_SLASH = 31, /* Ctrl-/ */
    BACKSPACE = 127, /* Backspace */
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
    SCROLL_LEFT,
    SCROLL_RIGHT,
    MOUSE_CLICK,
    MOUSE_DRAG,
    MOUSE_RELEASE,
    SHIFT_TAB
};

/* Language files loaded at startup (lang.c), and the first problem found in
 * them, shown on the status bar. */
extern struct editorSyntax **HLDB;
extern int HLDB_ENTRIES;
extern char hl_load_error[80];
extern int hl_load_errors;

/* buffer.c */
void *growBuf(void *buf, int *cap, int need);
void editorUpdateRow(erow *row);
void editorDelRow(int at);
char *editorRowsToString(int *buflen);
void editorRowAppendString(erow *row, char *s, size_t len);
void editorInsertChar(int c);
void editorInsertNewline(void);
void editorDelChar(void);
int editorOpen(char *filename);
int editorSave(void);
void editorFreeAllRows(void);
void editorRebuildRows(char *text, int len);

/* clipboard.c */
void editorCopySelection(void);
void editorPaste(void);

/* filetree.c */
void ftFreeNode(ftnode *n);
void ftBlankCell(struct abuf *ab);
void ftDrawSidebarRow(struct abuf *ab, int y);
void ftActivate(void);
void ftToggle(void);
int ftHandleKey(int c);

/* find.c */
void editorFind(int fd);

/* input.c */
int editorSelBounds(int *sr, int *sc, int *er, int *ec);
long editorSelCharCount(void);
void editorSetCol(int new_col);
void editorSetCursorPos(int row, int col);
void editorProcessKeypress(int fd);

/* lang.c */
void editorLoadLanguages(const char *argv0);

/* main.c */
int editorLoadFile(const char *path);

/* render.c */
void editorSanitize(char *s, int n);
int editorCharToRenderCol(erow *row, int char_col);
int editorRenderToCharCol(erow *row, int rcol);
void editorUpdateGeometry(void);
void editorRefreshScreen(void);
void editorSetStatusMessage(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void editorClearStatusMessage(void);

/* syntax.c */
int is_separator(int c);
int hlAt(const char *p, int i, int n, const hldelim *d);
void editorUpdateSyntax(erow *row);
int editorSyntaxToColor(int hl);
void editorSelectSyntaxHighlight(char *filename);

/* term.c */
void editorDie(const char *msg, int err);
int enableRawMode(int fd);
int editorReadKey(int fd);
int readByteTimeout(unsigned char *ch, int ms);
void updateWindowSize(void);
void editorWatchResize(void);

/* undo.c */
void editorPushSnapshot(void);
void editorCommitPending(void);
void editorUndo(void);
void editorRedo(void);
void editorUndoReset(void);
void editorUndoMarkSaved(void);
/* Set while typing extends the newest undo step; any other command clears it. */
extern int undo_last_insert;

#endif
