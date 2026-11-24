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
#define QUIP_TAB_STOP 2 /* Tab width; the Tab key inserts spaces up to it. */

#ifdef __linux__
#define _POSIX_C_SOURCE 200809L
#endif

#include <termios.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <dirent.h>
#include <unistd.h>
#include <stdarg.h>
#include <fcntl.h>
#include <signal.h>

/* Terminal escape sequences (VT100 and xterm). Write them with abAppendLit
 * or writeLit, which take the length from the literal so it is never counted
 * by hand. The "" on both sides only compiles for a string literal: a char *
 * would silently give sizeof(char *). */
#define LIT_LEN(s)         ((int)sizeof("" s "")-1)
#define abAppendLit(ab,s)  abAppend(ab,"" s "",LIT_LEN(s))
#define writeLit(fd,s)     write(fd,"" s "",LIT_LEN(s))

#define VT_HOME            "\x1b[H"
#define VT_CLEAR_EOL       "\x1b[0K"     /* Erase to the end of the line. */
#define VT_CURSOR_HIDE     "\x1b[?25l"
#define VT_CURSOR_SHOW     "\x1b[?25h"
#define VT_CURSOR_BAR      "\x1b[5 q"    /* Blinking bar cursor. */
#define VT_CURSOR_DEFAULT  "\x1b[0 q"    /* The terminal's own cursor shape. */
#define VT_CURSOR_FAR      "\x1b[999C\x1b[999B" /* To the bottom right. */
#define VT_QUERY_CURSOR    "\x1b[6n"     /* Reply: ESC [ row ; col R */
#define VT_ALT_SCREEN_ON   "\x1b[?1049h"
#define VT_ALT_SCREEN_OFF  "\x1b[?1049l"
#define VT_MOUSE_ON        "\x1b[?1002h" /* Button-event mouse reporting. */
#define VT_MOUSE_OFF       "\x1b[?1002l"
#define VT_CLIPBOARD_QUERY "\x1b]52;c;?\x07" /* OSC 52 clipboard read. */

/* Text attributes (SGR). */
#define VT_RESET           "\x1b[0m"     /* All attributes off. */
#define VT_BOLD            "\x1b[1m"
#define VT_REVERSE         "\x1b[7m"
#define VT_REVERSE_OFF     "\x1b[27m"
#define VT_FG_DEFAULT      "\x1b[39m"
#define VT_BG_DEFAULT      "\x1b[49m"
#define VT_FG_GRAY         "\x1b[90m"
#define VT_BOLD_BLUE       "\x1b[1;34m"

/* What each background means, as SGR numbers for "ESC [ n m". */
#define BG_DEFAULT           49
#define BG_TRAILING_SPACE    41  /* red */
#define BG_BRACKET_MATCH     100 /* gray */
#define BG_BRACKET_UNMATCHED 41  /* red */

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
#define HL_NESTED_COMMENTS   (1<<2) /* Block comments nest, as in OCaml. */
#define HL_MULTILINE_STRINGS (1<<3) /* "..." strings may span rows. */
#define HL_QUOTED_STRINGS    (1<<4) /* OCaml {id|...|id} strings. */
#define HL_PRIME_QUOTES      (1<<5) /* ' opens a char literal only when one
                                       fits; otherwise it is a type variable
                                       ('a) or an identifier prime (x'). */
#define HL_WORD_START_COMMENTS (1<<6) /* A line comment starts only at the
                                         start of a word, as # in shell. */
#define HL_TRIPLE_QUOTES     (1<<7) /* """...""" and '''...''' strings, which
                                       may span rows (Python docstrings,
                                       Java text blocks). */

/* Lexer state at the end of a row, carried into the next row. The low byte
 * is the kind; the rest is an argument: the comment nesting depth, the
 * quote char of an open string or triple quoted string, or the hash of a
 * quoted string's id. */
#define HL_ST_NORMAL  0
#define HL_ST_COMMENT 1
#define HL_ST_STRING  2
#define HL_ST_QSTRING 3
#define HL_ST_TSTRING 4
#define HL_ST(kind,arg) ((kind) | ((arg) << 8))
#define HL_ST_INVALID (-1) /* Never highlighted yet; differs from any state. */

/* A comment delimiter of any length; len 0 when the language has none. */
typedef struct hldelim {
    char *s;
    int len;
} hldelim;

struct editorSyntax {
    char *id;           /* Language file name without ".lang", e.g. "c". */
    char **extensions;  /* ".c" style suffixes, NULL terminated. */
    char **filenames;   /* Exact base names ("Makefile"), NULL terminated. */
    char **keywords;    /* "word", or "word|" for a type, NULL terminated. */
    hldelim line_comment;
    hldelim block_start;
    hldelim block_end;
    int flags;
};

/* This structure represents a single line of the file we are editing. */
typedef struct erow {
    int idx;            /* Row index in the file, zero-based. */
    int size;           /* Size of the row, excluding the null term. */
    int rsize;          /* Size of the rendered row. */
    int chars_cap;      /* Allocated capacity of 'chars'. */
    int render_cap;     /* Allocated capacity of 'render'. */
    int hl_cap;         /* Allocated capacity of 'hl'. */
    char *chars;        /* Row content. */
    char *render;       /* Row content "rendered" for screen (for TABs). */
    unsigned char *hl;  /* Syntax highlight type for each character in render.*/
    int hl_state;       /* Lexer state at the end of the row (HL_ST_*) as of
                           the last syntax highlight. */
} erow;

typedef struct hlcolor {
    int r,g,b;
} hlcolor;

struct editorConfig {
    int cx,cy;  /* Cursor x and y position in characters */
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
    struct editorSyntax *syntax;    /* Current syntax highlight, or NULL. */
    int sel_active;       /* 1 if a selection is live. */
    int sel_anchor_row;   /* File row where the selection was anchored. */
    int sel_anchor_col;   /* File col where the selection was anchored. */
    int mouse_cx, mouse_cy; /* Screen position of last mouse click. */
    int click_count;      /* Left presses in the current multi-click gesture. */
    int click_cx, click_cy;           /* Screen cell of the last left press. */
    struct timespec click_time;       /* When the last left press arrived. */
    int unit_sr, unit_sc, unit_er, unit_ec; /* Word or line a multi-click
                                               picked: (sr,sc) up to (er,ec). */
    char *clipboard;      /* Internal clipboard buffer (may contain '\n'). */
    int   clipboard_len;  /* Byte count of clipboard contents. */
    int ft_open;          /* File-tree sidebar visible? */
    int ft_width;         /* Columns reserved for the sidebar (0 when closed). */
    int ft_focus;         /* 1 = keys drive the tree, 0 = keys drive the editor. */
    struct ftnode *ft_root;  /* Tree root, lazily built on first open. */
    struct ftnode **ft_vis;  /* Flattened visible nodes, in display order. */
    int ft_vcount;        /* Number of entries in ft_vis. */
    int ft_vcap;          /* Allocated capacity of ft_vis. */
    int ft_sel;           /* Selected index into ft_vis. */
    int ft_scroll;        /* First ft_vis index shown at the top of the pane. */
};

static struct editorConfig E;

/* ================================ Undo/Redo ================================ */

/* Each snapshot stores only the delta from the previous snapshot's serialized
 * text, not a full copy: replacing bytes [pos, pos+oldlen) of the previous
 * text with 'new' (newlen bytes) yields this snapshot's text, and the reverse
 * splice undoes it. 'undo_base' always holds the full text of the snapshot at
 * 'undo_cur', so any neighbouring snapshot is one splice away. */
typedef struct editorSnapshot {
    int   pos;
    char *old;   /* bytes present in the previous snapshot's text */
    int   oldlen;
    char *new;   /* bytes present in this snapshot's text */
    int   newlen;
    int   cx, cy, rowoff, coloff;
} editorSnapshot;

#define UNDO_HISTORY_MAX 200

static editorSnapshot undo_hist[UNDO_HISTORY_MAX];
static int undo_cur   = -1;
static int undo_count =  0;
static int undo_saved = -1;
static int undo_last_insert = 0;
static char *undo_base = NULL; /* Full serialized text of the snapshot at undo_cur. */
static int   undo_base_len = 0;

enum KEY_ACTION{
        KEY_NULL = 0,       /* NULL */
        CTRL_E = 5,         /* Ctrl-e — toggle file tree */
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
        SCROLL_LEFT,
        SCROLL_RIGHT,
        MOUSE_CLICK,
        MOUSE_DRAG,
        MOUSE_RELEASE,
        SHIFT_TAB
};

void editorSetStatusMessage(const char *fmt, ...);

/* =========================== Syntax highlights DB =========================
 *
 * Languages are described by plain text ".lang" files (see langs/ and the
 * README), read once at startup from these directories, first match wins:
 *
 *   $XDG_CONFIG_HOME/quip/langs (~/.config/quip/langs)   the user's own
 *   $QUIP_LANGDIR                                        if set
 *   <dir of the quip binary>/langs                       a source checkout
 *   <dir of the quip binary>/../share/quip/langs         "make install"
 *   $XDG_DATA_HOME/quip/langs (~/.local/share/quip/langs)
 *   each $XDG_DATA_DIRS entry + /quip/langs (/usr/local/share, /usr/share)
 *
 * A file overrides any later file with the same name, so ~/.config/quip/
 * langs/python.lang replaces the installed python.lang. C and C++ are also
 * built in below, as one entry, so a bare binary still highlights them; any
 * c.lang replaces it, and C++ then comes from cpp.lang.
 *
 * A file only supplies words, comment delimiters and feature flags. How text
 * is lexed (strings, nesting, OCaml quotes) stays in editorHighlightRow, and
 * each "features" name in a file switches on one of the HL_* flags.
 *
 * Keywords with a trailing '|' are types, highlighted in a second color; the
 * loader adds the '|' to every word of a file's "types" line. */

/* C / C++, built in so that quip works without any language files. */
char *C_HL_extensions[] = {".c",".h",".cpp",".hpp",".cc",NULL};
char *C_HL_filenames[] = {NULL};
char *C_HL_keywords[] = {
	/* C Keywords */
	"auto","break","case","continue","default","do","else","enum",
	"extern","for","goto","if","register","return","sizeof","static",
	"struct","switch","typedef","union","volatile","while","NULL",

	/* C++ Keywords */
	"alignas","alignof","and","and_eq","asm","bitand","bitor","class",
	"compl","constexpr","const_cast","decltype","delete","dynamic_cast",
	"explicit","export","false","friend","inline","mutable","namespace",
	"new","noexcept","not","not_eq","nullptr","operator","or","or_eq",
	"private","protected","public","reinterpret_cast","static_assert",
	"static_cast","template","this","thread_local","throw","true","try",
	"typeid","typename","virtual","xor","xor_eq",

	/* C types */
        "int|","long|","double|","float|","char|","unsigned|","signed|",
        "void|","short|","auto|","const|","bool|",NULL
};

struct editorSyntax C_HL_builtin = {
    "c",
    C_HL_extensions,
    C_HL_filenames,
    C_HL_keywords,
    {"//",2},{"/*",2},{"*/",2},
    HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS
};

/* Every known language, in match priority order (see above). */
static struct editorSyntax **HLDB = NULL;
static int HLDB_ENTRIES = 0;

/* Names accepted on a language file's "features" line. */
static const struct { const char *name; int flag; } HL_FEATURES[] = {
    {"strings",           HL_HIGHLIGHT_STRINGS},
    {"numbers",           HL_HIGHLIGHT_NUMBERS},
    {"nested_comments",   HL_NESTED_COMMENTS},
    {"multiline_strings", HL_MULTILINE_STRINGS},
    {"quoted_strings",    HL_QUOTED_STRINGS},
    {"prime_quotes",      HL_PRIME_QUOTES},
    {"word_start_comments", HL_WORD_START_COMMENTS},
    {"triple_quotes",     HL_TRIPLE_QUOTES},
};

/* Problems found while loading language files, shown on the status line
 * once the editor is up. Only the first is spelled out. */
static char hl_load_error[80];
static int hl_load_errors = 0;

static void hlLoadError(const char *file, int line, const char *fmt, ...) {
    if (hl_load_errors++) return;
    int len = line ?
        snprintf(hl_load_error,sizeof(hl_load_error),"%s:%d: ",file,line) :
        snprintf(hl_load_error,sizeof(hl_load_error),"%s: ",file);
    if (len < 0 || len >= (int)sizeof(hl_load_error)) return;
    va_list ap;
    va_start(ap,fmt);
    vsnprintf(hl_load_error+len,sizeof(hl_load_error)-len,fmt,ap);
    va_end(ap);
}

/* Append 's' to the NULL terminated, malloc'd list '*list'. */
static void hlListAdd(char ***list, const char *s) {
    int n = 0;
    while (*list && (*list)[n]) n++;
    *list = realloc(*list,sizeof(char*)*(n+2));
    (*list)[n] = strdup(s);
    (*list)[n+1] = NULL;
}

static void hlListFree(char **list) {
    for (int i = 0; list && list[i]; i++) free(list[i]);
    free(list);
}

static void hlFreeSyntax(struct editorSyntax *s) {
    free(s->id);
    hlListFree(s->extensions);
    hlListFree(s->filenames);
    hlListFree(s->keywords);
    free(s->line_comment.s);
    free(s->block_start.s);
    free(s->block_end.s);
    free(s);
}

static void hlSetDelim(hldelim *d, const char *s) {
    d->s = strdup(s);
    d->len = strlen(s);
}

/* Split 'line' in place into whitespace separated tokens. Returns the count,
 * at most 'max'; more tokens than that make it return max+1. */
static int hlTokenize(char *line, char **tok, int max) {
    int n = 0;
    char *p = line;
    while (1) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) return n;
        if (n == max) return max+1;
        tok[n++] = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        if (*p) *p++ = '\0';
    }
}

/* Parse the language file 'path' (named 'file' in messages). Returns the
 * language, or NULL after reporting the first problem found. The format:
 *
 *   quip-lang 1                    must come first
 *   # comment                      whole lines only
 *   extensions    .py .pyw         any of these keys, each at most once
 *   filenames     SConstruct
 *   line_comment  #
 *   block_comment (* *)
 *   features      strings numbers
 *   keywords      def class        an indented line continues the key above
 *   types         int float
 */
static struct editorSyntax *hlLoadFile(const char *path, const char *file) {
    FILE *fp = fopen(path,"r");
    if (!fp) {
        hlLoadError(file,0,"%s",strerror(errno));
        return NULL;
    }

    struct editorSyntax *s = calloc(1,sizeof(*s));
    char *line = NULL;
    size_t linecap = 0;
    int lineno = 0, have_version = 0, ok = 1;
    char key[16] = "";  /* The key an indented line continues, if any. */
    unsigned seen = 0;  /* Bit per key in 'keys', to catch repeats. */
    static const char *keys[] = {"extensions","filenames","line_comment",
        "block_comment","features","keywords","types"};
    enum { K_EXT, K_FILES, K_LINE, K_BLOCK, K_FEAT, K_KW, K_TYPES, K_N };

    while (ok && getline(&line,&linecap,fp) != -1) {
        lineno++;
        char *tok[64];
        int cont = isspace((unsigned char)line[0]);
        char *first = line;
        while (*first && isspace((unsigned char)*first)) first++;
        if (*first == '\0' || *first == '#') continue;

        int ntok = hlTokenize(line,tok,64);
        if (ntok > 64) {
            hlLoadError(file,lineno,"more than 64 words on a line");
            ok = 0;
            break;
        }

        if (!have_version) {
            if (cont || ntok != 2 || strcmp(tok[0],"quip-lang")) {
                hlLoadError(file,lineno,"must start with \"quip-lang 1\"");
                ok = 0;
            } else if (strcmp(tok[1],"1")) {
                hlLoadError(file,lineno,"unsupported version %.10s",tok[1]);
                ok = 0;
            }
            have_version = 1;
            continue;
        }

        /* An indented line continues the key above; otherwise the first
         * word is the key. */
        char **args = tok;
        int nargs = ntok;
        if (!cont) {
            snprintf(key,sizeof(key),"%s",tok[0]);
            args++;
            nargs--;
        } else if (!key[0]) {
            hlLoadError(file,lineno,"indented line continues no key");
            ok = 0;
            break;
        }

        int k;
        for (k = 0; k < K_N; k++) if (!strcmp(key,keys[k])) break;
        if (k == K_N) {
            hlLoadError(file,lineno,"unknown key \"%.20s\"",key);
            ok = 0;
            break;
        }
        if (!cont) {
            if (seen & (1u << k)) {
                hlLoadError(file,lineno,"\"%s\" given twice",key);
                ok = 0;
                break;
            }
            seen |= 1u << k;
        }
        if (cont && (k == K_LINE || k == K_BLOCK)) {
            hlLoadError(file,lineno,"\"%s\" takes one line",key);
            ok = 0;
            break;
        }

        switch (k) {
        case K_EXT:
            for (int i = 0; i < nargs && ok; i++) {
                if (args[i][0] != '.' || args[i][1] == '\0') {
                    hlLoadError(file,lineno,
                        "extension \"%.20s\" must start with '.'",args[i]);
                    ok = 0;
                } else {
                    hlListAdd(&s->extensions,args[i]);
                }
            }
            break;
        case K_FILES:
            for (int i = 0; i < nargs; i++) hlListAdd(&s->filenames,args[i]);
            break;
        case K_LINE:
            if (nargs != 1) {
                hlLoadError(file,lineno,"line_comment takes one delimiter");
                ok = 0;
            } else {
                hlSetDelim(&s->line_comment,args[0]);
            }
            break;
        case K_BLOCK:
            if (nargs != 2) {
                hlLoadError(file,lineno,
                    "block_comment takes a start and an end");
                ok = 0;
            } else {
                hlSetDelim(&s->block_start,args[0]);
                hlSetDelim(&s->block_end,args[1]);
            }
            break;
        case K_FEAT:
            for (int i = 0; i < nargs && ok; i++) {
                unsigned f;
                for (f = 0; f < sizeof(HL_FEATURES)/sizeof(HL_FEATURES[0]); f++)
                    if (!strcmp(args[i],HL_FEATURES[f].name)) break;
                if (f == sizeof(HL_FEATURES)/sizeof(HL_FEATURES[0])) {
                    hlLoadError(file,lineno,"unknown feature \"%.20s\"",
                                args[i]);
                    ok = 0;
                } else {
                    s->flags |= HL_FEATURES[f].flag;
                }
            }
            break;
        case K_KW:
            for (int i = 0; i < nargs; i++) hlListAdd(&s->keywords,args[i]);
            break;
        case K_TYPES:
            for (int i = 0; i < nargs; i++) {
                size_t len = strlen(args[i]);
                char *type = malloc(len+2);
                memcpy(type,args[i],len);
                memcpy(type+len,"|",2);
                hlListAdd(&s->keywords,type);
                free(type);
            }
            break;
        }
    }
    free(line);
    fclose(fp);

    if (ok && !have_version) {
        hlLoadError(file,0,"empty, must start with \"quip-lang 1\"");
        ok = 0;
    }
    if (ok && !s->extensions && !s->filenames) {
        hlLoadError(file,0,"needs \"extensions\" or \"filenames\"");
        ok = 0;
    }
    if (!ok) {
        hlFreeSyntax(s);
        return NULL;
    }

    /* Empty lists and delimiters rather than NULLs keep the users simple. */
    if (!s->extensions) s->extensions = calloc(1,sizeof(char*));
    if (!s->filenames) s->filenames = calloc(1,sizeof(char*));
    if (!s->keywords) s->keywords = calloc(1,sizeof(char*));
    if (!s->line_comment.s) hlSetDelim(&s->line_comment,"");
    if (!s->block_start.s) {
        hlSetDelim(&s->block_start,"");
        hlSetDelim(&s->block_end,"");
    }
    s->id = strndup(file,strlen(file)-5); /* Drop ".lang". */
    return s;
}

/* Return malloc'd 'a' (its first 'alen' bytes, or all of it if alen is -1)
 * followed by 'b' and 'c'. Paths are built this way, at their exact length,
 * so that a long one is never cut short into some other path. */
static char *hlPath(const char *a, int alen, const char *b, const char *c) {
    size_t la = alen < 0 ? strlen(a) : (size_t)alen;
    size_t lb = strlen(b), lc = strlen(c);
    char *p = malloc(la+lb+lc+1);
    memcpy(p,a,la);
    memcpy(p+la,b,lb);
    memcpy(p+la+lb,c,lc+1);
    return p;
}

static int hlKnown(const char *id) {
    for (int i = 0; i < HLDB_ENTRIES; i++)
        if (!strcmp(HLDB[i]->id,id)) return 1;
    return 0;
}

static void hlAdd(struct editorSyntax *s) {
    HLDB = realloc(HLDB,sizeof(*HLDB)*(HLDB_ENTRIES+1));
    HLDB[HLDB_ENTRIES++] = s;
}

static int hlCmpNames(const void *a, const void *b) {
    return strcmp(*(char * const *)a,*(char * const *)b);
}

/* Load every *.lang file in 'dir' whose name was not loaded already, in
 * name order so that extension clashes resolve the same way every time. */
static void hlLoadDir(const char *dir) {
    /* The same directory can be named twice (QUIP_LANGDIR and an XDG dir);
     * scan it once so its problems are not reported twice. */
    static struct { dev_t dev; ino_t ino; } scanned[16];
    static int nscanned = 0;
    struct stat st;
    if (stat(dir,&st) == -1 || !S_ISDIR(st.st_mode))
        return; /* Missing directories are normal. */
    for (int i = 0; i < nscanned; i++)
        if (scanned[i].dev == st.st_dev && scanned[i].ino == st.st_ino) return;
    if (nscanned < (int)(sizeof(scanned)/sizeof(scanned[0]))) {
        scanned[nscanned].dev = st.st_dev;
        scanned[nscanned].ino = st.st_ino;
        nscanned++;
    }

    DIR *d = opendir(dir);
    if (!d) return;
    char **names = NULL;
    int n = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        size_t len = strlen(de->d_name);
        if (len > 5 && !strcmp(de->d_name+len-5,".lang")) {
            names = realloc(names,sizeof(char*)*(n+1));
            names[n++] = strdup(de->d_name);
        }
    }
    closedir(d);
    if (n) qsort(names,n,sizeof(char*),hlCmpNames);

    for (int i = 0; i < n; i++) {
        char *id = strndup(names[i],strlen(names[i])-5); /* Drop ".lang". */
        if (!hlKnown(id)) {
            char *path = hlPath(dir,-1,"/",names[i]);
            struct editorSyntax *s = hlLoadFile(path,names[i]);
            if (s) hlAdd(s);
            free(path);
        }
        free(id);
        free(names[i]);
    }
    free(names);
}

/* Load the language directory 'base' (its first 'len' bytes, or all of it
 * if len is -1) followed by 'sub'. */
static void hlLoadSubdir(const char *base, int len, const char *sub) {
    char *dir = hlPath(base,len,sub,"");
    hlLoadDir(dir);
    free(dir);
}

/* Return the malloc'd directory holding the running binary, found from
 * argv[0] as the shell did: a name with a slash is a path, a bare name was
 * looked up in $PATH. Returns NULL if it cannot be found. */
static char *hlExeDir(const char *argv0) {
    const char *slash = strrchr(argv0,'/');
    if (slash == argv0) return strdup("/");     /* "/quip" */
    if (slash) return strndup(argv0,slash-argv0);
    const char *path = getenv("PATH");
    while (path && *path) {
        int len = strcspn(path,":");
        /* An empty entry means the current directory. */
        char *dir = len ? strndup(path,len) : strdup(".");
        char *exe = hlPath(dir,-1,"/",argv0);
        int found = access(exe,X_OK) == 0;
        free(exe);
        if (found) return dir;
        free(dir);
        path += len;
        if (*path == ':') path++;
    }
    return NULL;
}

/* Build HLDB from the language directories, in the order described at the
 * top of this section, then the built-in C. */
void editorLoadLanguages(const char *argv0) {
    const char *home = getenv("HOME");
    const char *env;

    if ((env = getenv("XDG_CONFIG_HOME")) && env[0])
        hlLoadSubdir(env,-1,"/quip/langs");
    else if (home)
        hlLoadSubdir(home,-1,"/.config/quip/langs");

    if ((env = getenv("QUIP_LANGDIR")) && env[0]) hlLoadDir(env);

    char *exedir = hlExeDir(argv0);
    if (exedir) {
        hlLoadSubdir(exedir,-1,"/langs");
        hlLoadSubdir(exedir,-1,"/../share/quip/langs");
        free(exedir);
    }

    if ((env = getenv("XDG_DATA_HOME")) && env[0])
        hlLoadSubdir(env,-1,"/quip/langs");
    else if (home)
        hlLoadSubdir(home,-1,"/.local/share/quip/langs");

    env = getenv("XDG_DATA_DIRS");
    if (!env || !env[0]) env = "/usr/local/share:/usr/share";
    while (*env) {
        int len = strcspn(env,":");
        if (len) hlLoadSubdir(env,len,"/quip/langs");
        env += len;
        if (*env == ':') env++;
    }

    if (!hlKnown("c")) hlAdd(&C_HL_builtin);
}

/* ======================= Low level terminal handling ====================== */

static struct termios orig_termios; /* In order to restore at exit.*/

void disableRawMode(int fd) {
    /* Don't even check the return value as it's too late. */
    if (E.rawmode) {
        writeLit(STDOUT_FILENO,VT_MOUSE_OFF);
        writeLit(STDOUT_FILENO,VT_CURSOR_DEFAULT);
        writeLit(STDOUT_FILENO,VT_ALT_SCREEN_OFF); /* back to the shell's screen */
        tcsetattr(fd,TCSAFLUSH,&orig_termios);
        E.rawmode = 0;
    }
}

/* Leave the editor screen, then report a fatal error where the user can
 * still read it after exit. 'err' is an errno value, or 0 for none. */
static void editorDie(const char *msg, int err) {
    disableRawMode(STDIN_FILENO);
    if (err) fprintf(stderr, "%s: %s\n", msg, strerror(err));
    else     fprintf(stderr, "%s\n", msg);
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
    /* Draw on the alternate screen so quitting restores the shell's screen
     * instead of leaving the file's contents behind. */
    writeLit(STDOUT_FILENO,VT_ALT_SCREEN_ON);
    signal(SIGTERM, handleSigTerm);
    signal(SIGHUP, handleSigTerm);
    signal(SIGINT, handleSigTerm);
    writeLit(STDOUT_FILENO,VT_MOUSE_ON);
    writeLit(STDOUT_FILENO,VT_CURSOR_BAR);
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
                    case 'Z': return SHIFT_TAB;
                    case 'M': {
                        if (read(fd,seq+2,1) == 0) return ESC;
                        if (read(fd,seq+3,1) == 0) return ESC;
                        if (read(fd,seq+4,1) == 0) return ESC;
                        unsigned char mbtn = (unsigned char)seq[2];
                        int b = mbtn - 32;
                        if (b & 64) {
                            /* Wheel: bits 0-1 give up/down/left/right. Shift
                             * turns a vertical wheel into a horizontal one,
                             * for mice without a tilt wheel. */
                            int dir = b & 3;
                            if ((b & 4) && dir < 2) dir += 2;
                            if (dir == 0) return SCROLL_UP;
                            if (dir == 1) return SCROLL_DOWN;
                            if (dir == 2) return SCROLL_LEFT;
                            return SCROLL_RIGHT;
                        }
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
    if (writeLit(ofd,VT_QUERY_CURSOR) != LIT_LEN(VT_QUERY_CURSOR)) return -1;

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
        if (writeLit(ofd,VT_CURSOR_FAR) != LIT_LEN(VT_CURSOR_FAR)) goto failed;
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

/* Ensure 'buf' can hold at least 'need' bytes, growing its capacity
 * geometrically. '*cap' tracks the current capacity and is updated in place.
 * Never shrinks; returns the (possibly reallocated) buffer. */
static void *growBuf(void *buf, int *cap, int need) {
    if (need <= *cap) return buf;
    int nc = *cap ? *cap : 8;
    while (nc < need) nc *= 2;
    *cap = nc;
    return realloc(buf,nc);
}

/* True if delimiter 'd' (never, if the language has none) starts at
 * offset i of the n-byte row text p. */
static int hlAt(const char *p, int i, int n, const hldelim *d) {
    return d->len && i+d->len <= n && !memcmp(p+i,d->s,d->len);
}

/* True for chars that a keyword cannot touch: identifier chars ('$' for
 * JavaScript and shell names, UTF-8 bytes) and '#', so the "if" in C's "#if"
 * stays uncolored like the rest of the preprocessor directives. */
static int hlIsWordChar(int c) {
    return isalnum(c) || c == '_' || c == '$' || c == '#' || c >= 0x80;
}

/* True for chars that end a shell word, so a # after them starts a comment:
 * ${#arr}, x#y and a=#x are not comments, but cmd;# and (# are. */
static int hlIsShellWordBreak(int c) {
    return isspace(c) || (c && strchr(";&|()<>",c));
}

/* True for chars that continue an identifier, OCaml primes included. */
static int hlIsIdentChar(int c) {
    return isalnum(c) || c == '_' || c == '\'';
}

/* Length of the OCaml quoted string id ({id|...|id}) at p: lowercase
 * letters and underscores, possibly none. */
static int hlQuotedIdLen(const char *p, int n) {
    int j = 0;
    while (j < n && (islower((unsigned char)p[j]) || p[j] == '_')) j++;
    return j;
}

/* Hash of a quoted string id, small enough to fit a lexer state argument. */
static int hlQuotedIdHash(const char *s, int len) {
    unsigned int h = 2166136261u;
    for (int k = 0; k < len; k++) h = (h ^ (unsigned char)s[k]) * 16777619u;
    return (int)(h & 0x7fffff);
}

/* Length of the OCaml char literal starting at the quote at p ('a', '\n',
 * '\'', '\065', '\xff', '\o377'), or 0 if none fits there. */
static int hlCharLiteralLen(const char *p, int n) {
    if (n >= 3 && p[1] != '\\' && p[2] == '\'') return 3;
    if (n >= 4 && p[1] == '\\')
        for (int j = 3; j < n && j <= 6; j++)
            if (p[j] == '\'') return j+1;
    return 0;
}

/* Set row->hl for every rendered char of the row, starting from lexer
 * 'state' (the state the previous row ended in). Returns the state this
 * row ends in. */
static int editorHighlightRow(erow *row, int state) {
    char **keywords = E.syntax->keywords;
    const hldelim *scs = &E.syntax->line_comment;
    const hldelim *mcs = &E.syntax->block_start;
    const hldelim *mce = &E.syntax->block_end;
    int flags = E.syntax->flags;
    char *p = row->render;
    unsigned char *hl = row->hl;
    int n = row->rsize;
    int kind = state & 0xff, arg = state >> 8;
    int i = 0;
    int escaped_eol = 0; /* Row ends in a backslash inside a string. */

    while (i < n) {
        int c = (unsigned char)p[i];

        /* Inside a block comment, possibly opened on an earlier row. */
        if (kind == HL_ST_COMMENT) {
            if (hlAt(p,i,n,mce)) {
                memset(hl+i,HL_MLCOMMENT,mce->len);
                i += mce->len;
                if (--arg == 0) kind = HL_ST_NORMAL;
            } else if ((flags & HL_NESTED_COMMENTS) && hlAt(p,i,n,mcs)) {
                memset(hl+i,HL_MLCOMMENT,mcs->len);
                i += mcs->len;
                if (arg < 0x7fffff) arg++;
            } else {
                hl[i++] = HL_MLCOMMENT;
            }
            continue;
        }

        /* Inside a "" or '' string. */
        if (kind == HL_ST_STRING) {
            hl[i] = HL_STRING;
            if (c == '\\') {
                if (i+1 < n) {
                    hl[i+1] = HL_STRING;
                    i += 2;
                } else {
                    escaped_eol = 1;
                    i++;
                }
                continue;
            }
            i++;
            if (c == arg) kind = HL_ST_NORMAL;
            continue;
        }

        /* Inside a """ or ''' string: only three of the same quote end it,
         * and a backslash escapes the char after it, as in Python, raw
         * strings included. A newline never ends it. */
        if (kind == HL_ST_TSTRING) {
            hl[i] = HL_STRING;
            if (c == '\\') {
                if (i+1 < n) hl[i+1] = HL_STRING;
                i += 2;
                continue;
            }
            if (c == arg && i+2 < n && p[i+1] == arg && p[i+2] == arg) {
                memset(hl+i,HL_STRING,3);
                i += 3;
                kind = HL_ST_NORMAL;
                continue;
            }
            i++;
            continue;
        }

        /* Inside a {id|...|id} string: only |id} with the same id ends it. */
        if (kind == HL_ST_QSTRING) {
            hl[i] = HL_STRING;
            if (c == '|') {
                int idlen = hlQuotedIdLen(p+i+1,n-i-1);
                int end = i+1+idlen;
                if (end < n && p[end] == '}' &&
                    hlQuotedIdHash(p+i+1,idlen) == arg)
                {
                    memset(hl+i,HL_STRING,end+1-i);
                    i = end+1;
                    kind = HL_ST_NORMAL;
                    continue;
                }
            }
            i++;
            continue;
        }

        /* Single line comment: the rest of the row. It starts anywhere
         * outside a string, or only at the start of a word in shell. */
        if (hlAt(p,i,n,scs) &&
            (!(flags & HL_WORD_START_COMMENTS) || i == 0 ||
             hlIsShellWordBreak((unsigned char)p[i-1])))
        {
            memset(hl+i,HL_COMMENT,n-i);
            break;
        }

        /* Block comment start. */
        if (hlAt(p,i,n,mcs)) {
            memset(hl+i,HL_MLCOMMENT,mcs->len);
            i += mcs->len;
            kind = HL_ST_COMMENT;
            arg = 1;
            continue;
        }

        /* OCaml quote: a char literal if one fits, else a type variable
         * ('a), unless it is the prime of an identifier (x'). */
        if ((flags & HL_PRIME_QUOTES) && c == '\'' &&
            !(i > 0 && hlIsIdentChar((unsigned char)p[i-1])))
        {
            int len = (flags & HL_HIGHLIGHT_STRINGS) ?
                      hlCharLiteralLen(p+i,n-i) : 0;
            if (len) {
                memset(hl+i,HL_STRING,len);
                i += len;
                continue;
            }
            if (i+1 < n && (isalpha((unsigned char)p[i+1]) || p[i+1] == '_')) {
                int j = i+1;
                while (j < n && (isalnum((unsigned char)p[j]) || p[j] == '_'))
                    j++;
                memset(hl+i,HL_KEYWORD2,j-i);
                i = j;
                continue;
            }
        }

        /* String starts. */
        if (flags & HL_HIGHLIGHT_STRINGS) {
            if (c == '"' || (c == '\'' && !(flags & HL_PRIME_QUOTES))) {
                if ((flags & HL_TRIPLE_QUOTES) &&
                    i+2 < n && p[i+1] == c && p[i+2] == c)
                {
                    memset(hl+i,HL_STRING,3);
                    i += 3;
                    kind = HL_ST_TSTRING;
                    arg = c;
                    continue;
                }
                hl[i++] = HL_STRING;
                kind = HL_ST_STRING;
                arg = c;
                continue;
            }
            if ((flags & HL_QUOTED_STRINGS) && c == '{') {
                int idlen = hlQuotedIdLen(p+i+1,n-i-1);
                int end = i+1+idlen;
                if (end < n && p[end] == '|') {
                    memset(hl+i,HL_STRING,end+1-i);
                    arg = hlQuotedIdHash(p+i+1,idlen);
                    kind = HL_ST_QSTRING;
                    i = end+1;
                    continue;
                }
            }
        }

        /* Control characters. Bytes >= 0x80 are UTF-8 text. */
        if (c < 0x20 || c == 0x7f) {
            hl[i++] = HL_NONPRINT;
            continue;
        }

        /* Numbers: a digit at the start of a word, then the rest of the
         * literal (0x1F, 1_000, 3.14, 1e-5, 0x1p-3, 10UL, 42L). Any char
         * that cannot be part of a word starts one, as for keywords, so the
         * 2 in a[1:2], {1:2} and y?1:2 is a number but the 1 in x1 is not. */
        if ((flags & HL_HIGHLIGHT_NUMBERS) && isdigit(c) &&
            (i == 0 || !hlIsWordChar((unsigned char)p[i-1])))
        {
            int hex = c == '0' && i+1 < n && (p[i+1] == 'x' || p[i+1] == 'X');
            int j = i+1;
            while (j < n) {
                int d = (unsigned char)p[j], e = (unsigned char)p[j-1];
                if (isalnum(d) || d == '_' || d == '.') {
                    j++;
                } else if ((d == '+' || d == '-') &&
                           (hex ? (e == 'p' || e == 'P') :
                                  (e == 'e' || e == 'E'))) {
                    j++;
                } else {
                    break;
                }
            }
            memset(hl+i,HL_NUMBER,j-i);
            i = j;
            continue;
        }

        /* Keywords and types: a whole word, so "int" matches in "vector<int>"
         * and "std::string" but not in "print". */
        if (i == 0 || !hlIsWordChar((unsigned char)p[i-1])) {
            int j;
            for (j = 0; keywords[j]; j++) {
                int klen = strlen(keywords[j]);
                int kw2 = keywords[j][klen-1] == '|';
                if (kw2) klen--;

                /* render is NUL terminated, so p[n] ends a word. */
                if (i+klen <= n &&
                    !memcmp(p+i,keywords[j],klen) &&
                    !hlIsWordChar((unsigned char)p[i+klen]))
                {
                    memset(hl+i,kw2 ? HL_KEYWORD2 : HL_KEYWORD1,klen);
                    i += klen;
                    break;
                }
            }
            if (keywords[j] != NULL) continue;
        }

        /* Not special chars */
        i++;
    }

    /* A string left open at the end of the row only carries over if the
     * language allows multi row strings or the newline is escaped. */
    if (kind == HL_ST_STRING && !(flags & HL_MULTILINE_STRINGS) &&
        !escaped_eol) kind = HL_ST_NORMAL;
    return kind == HL_ST_NORMAL ? HL_ST_NORMAL : HL_ST(kind,arg);
}

/* Set every byte of row->hl (that corresponds to every character in the line)
 * to the right syntax highlight type (HL_* defines). The state a row ends in
 * is where the next row starts, so while that state changes, the following
 * rows are highlighted again too. This is a loop, not recursion: opening a
 * comment at the top of a huge file must not overflow the stack. */
void editorUpdateSyntax(erow *row) {
    while (1) {
        row->hl = growBuf(row->hl,&row->hl_cap,row->rsize);
        memset(row->hl,HL_NORMAL,row->rsize);
        if (E.syntax == NULL) { /* No syntax, everything is HL_NORMAL. */
            row->hl_state = HL_ST_NORMAL;
            return;
        }

        int in = row->idx > 0 ? E.row[row->idx-1].hl_state : HL_ST_NORMAL;
        if (in == HL_ST_INVALID) in = HL_ST_NORMAL;
        int out = editorHighlightRow(row,in);
        int changed = out != row->hl_state;
        row->hl_state = out;
        if (!changed || row->idx+1 >= E.numrows) return;
        row = &E.row[row->idx+1];
    }
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
 * setting it in the global state E.syntax (NULL if nothing matches). */
void editorSelectSyntaxHighlight(char *filename) {
    const char *base = strrchr(filename,'/');
    base = base ? base+1 : filename;
    size_t blen = strlen(base);
    E.syntax = NULL;
    for (int j = 0; j < HLDB_ENTRIES; j++) {
        struct editorSyntax *s = HLDB[j];
        for (int i = 0; s->filenames[i]; i++) {
            if (!strcmp(base,s->filenames[i])) {
                E.syntax = s;
                return;
            }
        }
        for (int i = 0; s->extensions[i]; i++) {
            size_t elen = strlen(s->extensions[i]);
            if (blen > elen && !strcmp(base+blen-elen,s->extensions[i])) {
                E.syntax = s;
                return;
            }
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
    for (j = 0; j < row->size; j++)
        if (row->chars[j] == TAB) tabs++;

    unsigned long long allocsize =
        (unsigned long long) row->size + tabs*QUIP_TAB_STOP + nonprint*9 + 1;
    if (allocsize > UINT32_MAX) {
        editorDie("Some line of the edited file is too long for quip", 0);
    }

    row->render = growBuf(row->render,&row->render_cap,
                          row->size + tabs*QUIP_TAB_STOP + nonprint*9 + 1);
    idx = 0;
    for (j = 0; j < row->size; j++) {
        if (row->chars[j] == TAB) {
            do row->render[idx++] = ' '; while (idx % QUIP_TAB_STOP != 0);
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
    if (E.numrows+1 > E.rowcap) {
        E.rowcap = E.rowcap ? E.rowcap*2 : 16;
        E.row = realloc(E.row,sizeof(erow)*E.rowcap);
    }
    if (at != E.numrows) {
        memmove(E.row+at+1,E.row+at,sizeof(E.row[0])*(E.numrows-at));
        for (int j = at+1; j <= E.numrows; j++) E.row[j].idx++;
    }
    E.row[at].size = len;
    E.row[at].chars_cap = len+1;
    E.row[at].chars = malloc(len+1);
    memcpy(E.row[at].chars,s,len+1);
    E.row[at].hl = NULL;
    E.row[at].hl_cap = 0;
    E.row[at].hl_state = HL_ST_INVALID; /* So the rows below get re-lexed. */
    E.row[at].render = NULL;
    E.row[at].render_cap = 0;
    E.row[at].rsize = 0;
    E.row[at].idx = at;
    E.numrows++; /* First, so highlight changes can reach the last row. */
    editorUpdateRow(E.row+at);
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
    for (int j = at; j < E.numrows-1; j++) E.row[j].idx--;
    E.numrows--;
    E.dirty++;
    /* The row that moved up now follows a different row: re-lex it. */
    if (at < E.numrows) editorUpdateSyntax(E.row+at);
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
        row->chars = growBuf(row->chars,&row->chars_cap,row->size+padlen+2);
        memset(row->chars+row->size,' ',padlen);
        row->chars[row->size+padlen+1] = '\0';
        row->size += padlen+1;
    } else {
        /* If we are in the middle of the string just make space for 1 new
         * char plus the (already existing) null term. */
        row->chars = growBuf(row->chars,&row->chars_cap,row->size+2);
        memmove(row->chars+at+1,row->chars+at,row->size-at+1);
        row->size++;
    }
    row->chars[at] = c;
    editorUpdateRow(row);
    E.dirty++;
}

/* Append the string 's' at the end of a row */
void editorRowAppendString(erow *row, char *s, size_t len) {
    row->chars = growBuf(row->chars,&row->chars_cap,row->size+len+1);
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
    row->size--;
    editorUpdateRow(row);
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
            editorDie("Opening file", errno);
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

/* Bytes that a terminal would obey instead of print: a C0 control or DEL
 * (1 byte), or a C1 control U+0080-U+009F written in UTF-8 (2 bytes).
 * Returns how many bytes at s[i] form one, or 0. Text from files, file names
 * and messages must never reach the terminal with these intact. */
static int editorCtrlLen(const char *s, int i, int n) {
    unsigned char b = (unsigned char)s[i];
    if (b < 0x20 || b == 0x7f) return 1;
    if (b == 0xc2 && i+1 < n &&
        (unsigned char)s[i+1] >= 0x80 && (unsigned char)s[i+1] <= 0x9f)
        return 2;
    return 0;
}

/* Replace control characters in 's' with '?', one per byte. */
static void editorSanitize(char *s, int n) {
    int i = 0;
    while (i < n) {
        int k = editorCtrlLen(s,i,n);
        if (k == 0) { i++; continue; }
        while (k--) s[i++] = '?';
    }
}

/* We define a very simple "append buffer" structure, that is an heap
 * allocated string where we can append to. This is useful in order to
 * write all the escape sequences in a buffer and flush them to the standard
 * output in a single call, to avoid flickering effects. */
struct abuf {
    char *b;
    int len;
    int cap;
};

#define ABUF_INIT {NULL,0,0}

void abAppend(struct abuf *ab, const char *s, int len) {
    if (ab->len+len > ab->cap) {
        int nc = ab->cap ? ab->cap : 1024;
        while (nc < ab->len+len) nc *= 2;
        char *new = realloc(ab->b,nc);
        if (new == NULL) return;
        ab->b = new;
        ab->cap = nc;
    }
    memcpy(ab->b+ab->len,s,len);
    ab->len += len;
}

void abFree(struct abuf *ab) {
    free(ab->b);
}

int editorCharToRenderCol(erow *row, int char_col) {
    int rcol = 0;
    for (int i = 0; i < char_col && i < row->size; i++) {
        if (row->chars[i] == TAB) rcol += QUIP_TAB_STOP - (rcol % QUIP_TAB_STOP);
        else rcol++;
    }
    return rcol;
}

/* Insert spaces up to the next tab stop instead of a TAB character. */
void editorInsertTab(void) {
    int filerow = E.rowoff+E.cy;
    int filecol = E.coloff+E.cx;
    int rcol = filerow < E.numrows ?
               editorCharToRenderCol(&E.row[filerow],filecol) : 0;
    int n = QUIP_TAB_STOP - (rcol % QUIP_TAB_STOP);
    while (n--) editorInsertChar(' ');
}

/* Inverse of editorCharToRenderCol: the char column whose rendering covers
 * render column 'rcol' (a click inside a TAB lands just after it). */
int editorRenderToCharCol(erow *row, int rcol) {
    int fc = 0, rc = 0;
    while (fc < row->size && rc < rcol) {
        if (row->chars[fc] == TAB) rc += QUIP_TAB_STOP - (rc % QUIP_TAB_STOP);
        else rc++;
        fc++;
    }
    return fc;
}

void editorSelClear(void) { E.sel_active = 0; }

/* Ordered bounds of the selection: from (*sr,*sc) up to (*er,*ec). An end on
 * the empty line past the last row (the keyboard can reach it) is clamped to
 * the end of the file, so callers only ever see real rows. Returns 0 when
 * nothing is selected. */
static int editorSelBounds(int *sr, int *sc, int *er, int *ec) {
    if (!E.sel_active || E.numrows == 0) return 0;
    int ar = E.sel_anchor_row,  ac = E.sel_anchor_col;
    int cr = E.rowoff + E.cy,   cc = E.coloff + E.cx;
    if (ar < cr || (ar == cr && ac <= cc)) {
        *sr = ar; *sc = ac; *er = cr; *ec = cc;
    } else {
        *sr = cr; *sc = cc; *er = ar; *ec = ac;
    }
    if (*er >= E.numrows) { *er = E.numrows - 1; *ec = E.row[*er].size; }
    if (*sr > *er)        { *sr = *er; *sc = *ec; }
    return !(*sr == *er && *sc == *ec);
}

/* Number of characters in the selection, each line break counting as one.
 * UTF-8 continuation bytes are skipped so a multibyte character counts once. */
static long editorSelCharCount(void) {
    int sr, sc, er, ec;
    if (!editorSelBounds(&sr,&sc,&er,&ec)) return 0;
    long count = er - sr;
    for (int i = sr; i <= er; i++) {
        erow *row = &E.row[i];
        int from = (i == sr) ? sc : 0;
        int to = (i == er) ? ec : row->size;
        for (int j = from; j < to; j++)
            if (((unsigned char)row->chars[j] & 0xC0) != 0x80) count++;
    }
    return count;
}

/* Character column of the cursor: UTF-8 continuation bytes before it are not
 * counted, so a multibyte character advances the column by one. */
static int editorCursorCharCol(void) {
    int filerow = E.rowoff+E.cy, col = E.coloff+E.cx;
    if (filerow >= E.numrows) return col;
    erow *row = &E.row[filerow];
    int bytes = col < row->size ? col : row->size, n = col - bytes;
    for (int j = 0; j < bytes; j++)
        if (((unsigned char)row->chars[j] & 0xC0) != 0x80) n++;
    return n;
}

/* Bracket matching. When the cursor is over a bracket, it and its partner
 * get a gray background; a bracket without a partner gets a red one. */
#define BRACKET_NONE      0
#define BRACKET_MATCHED   1
#define BRACKET_UNMATCHED 2

/* True if the render column holds code: brackets in strings and comments
 * are text, so they neither match nor count toward nesting. */
static int editorIsCodeAt(const erow *row, int rcol) {
    unsigned char h = row->hl[rcol];
    return h != HL_STRING && h != HL_COMMENT && h != HL_MLCOMMENT;
}

/* If the cursor is over a bracket in code, store its row and render column
 * in *brow,*bcol and look for its partner of the same kind, (), [] or {},
 * skipping nested pairs. On BRACKET_MATCHED the partner's position is in
 * *mrow,*mcol. Scans the whole file at worst, which is a plain byte loop. */
static int editorFindBracketPair(int *brow, int *bcol, int *mrow, int *mcol) {
    static const char open[] = "([{", close[] = ")]}";
    int y = E.rowoff+E.cy, col = E.coloff+E.cx;
    if (y >= E.numrows) return BRACKET_NONE;
    erow *row = &E.row[y];
    if (col >= row->size) return BRACKET_NONE;

    char c = row->chars[col];
    const char *o = c ? strchr(open,c) : NULL;
    const char *cl = c ? strchr(close,c) : NULL;
    if (!o && !cl) return BRACKET_NONE;
    int x = editorCharToRenderCol(row,col);
    if (!editorIsCodeAt(row,x)) return BRACKET_NONE;

    char want = o ? close[o-open] : open[cl-close];
    int dir = o ? 1 : -1, depth = 0;
    *brow = y;
    *bcol = x;
    for (;;) {
        x += dir;
        while (x < 0 || x >= E.row[y].rsize) {
            y += dir;
            if (y < 0 || y >= E.numrows) return BRACKET_UNMATCHED;
            x = dir > 0 ? 0 : E.row[y].rsize-1;
        }
        char ch = E.row[y].render[x];
        if (ch != c && ch != want) continue;
        if (!editorIsCodeAt(&E.row[y],x)) continue;
        if (ch == c) {
            depth++;
        } else if (depth-- == 0) {
            *mrow = y;
            *mcol = x;
            return BRACKET_MATCHED;
        }
    }
}

static void editorSelSetAnchor(void) {
    if (!E.sel_active) {
        E.sel_active     = 1;
        E.sel_anchor_row = E.rowoff + E.cy;
        E.sel_anchor_col = E.coloff + E.cx;
    }
}

/* This function writes the whole screen using VT100 escape characters
 * starting from the logical state of the editor in the global state 'E'. */
/* Defined in the file-tree section below; used while composing the frame. */
static void ftDrawSidebarRow(struct abuf *ab, int y);
static void ftBlankCell(struct abuf *ab);

#define LN_MIN_DIGITS 3  /* Gutter fits at least this many digits. */
#define LN_MIN_TEXT 20   /* Hide the gutter rather than leave fewer text cols. */

/* Recompute the text width from the terminal width, the sidebar and the line
 * number gutter, whose width follows the number of digits in the last line
 * number. 'E.screencols' always means text columns, so the scroll math never
 * needs to know about either pane. */
static void editorUpdateGeometry(void) {
    int digits = 1;
    for (int n = E.numrows; n >= 10; n /= 10) digits++;
    if (digits < LN_MIN_DIGITS) digits = LN_MIN_DIGITS;
    int avail = E.termcols - E.ft_width;
    E.ln_width = (avail - (digits+1) >= LN_MIN_TEXT) ? digits+1 : 0;
    E.screencols = avail - E.ln_width;
    if (E.screencols < 1) E.screencols = 1;
    /* Keep the cursor inside the (possibly narrower) text area. */
    if (E.cx >= E.screencols) {
        E.coloff += E.cx - (E.screencols-1);
        E.cx = E.screencols-1;
    }
    if (E.cx < 0) E.cx = 0;
    if (E.coloff < 0) E.coloff = 0;
}

/* Draw the gutter cell for 'filerow': its right-aligned line number and a
 * separating space, with the cursor's line brighter than the rest. Rows past
 * the end of the file get a '~' marker instead. */
static void editorDrawGutter(struct abuf *ab, int filerow) {
    char buf[32];
    if (filerow >= E.numrows) {
        int len = snprintf(buf,sizeof(buf),"%-*s",E.ln_width,"~");
        abAppend(ab,buf,len);
        return;
    }
    int current = (filerow == E.rowoff+E.cy);
    if (current) abAppendLit(ab,VT_BOLD);
    else abAppendLit(ab,VT_FG_GRAY);
    int len = snprintf(buf,sizeof(buf),"%*d ",E.ln_width-1,filerow+1);
    abAppend(ab,buf,len);
    abAppendLit(ab,VT_RESET);
}

void editorRefreshScreen(void) {
    int y;
    erow *r;
    char buf[32];
    struct abuf ab = ABUF_INIT;

    editorUpdateGeometry();
    abAppendLit(&ab,VT_CURSOR_HIDE); /* Hide cursor. */
    abAppendLit(&ab,VT_HOME); /* Go home. */

    /* The bracket under the cursor and its partner, if any. No cursor is
     * shown in the text while the file tree has focus. */
    int br_row = -1, br_col = -1, bm_row = -1, bm_col = -1;
    int br_bg = BG_DEFAULT;
    if (!(E.ft_open && E.ft_focus)) {
        int kind = editorFindBracketPair(&br_row,&br_col,&bm_row,&bm_col);
        if (kind == BRACKET_MATCHED) br_bg = BG_BRACKET_MATCH;
        else if (kind == BRACKET_UNMATCHED) br_bg = BG_BRACKET_UNMATCHED;
    }

    for (y = 0; y < E.screenrows; y++) {
        int filerow = E.rowoff+y;

        if (E.ft_open) {
            /* Draw the sidebar cell for this row, then move to the text start
             * column so the existing text-drawing code runs unchanged. */
            char pos[32];
            int pl = snprintf(pos,sizeof(pos),"\x1b[%d;1H",y+1);
            abAppend(&ab,pos,pl);
            ftDrawSidebarRow(&ab,y);
        }

        if (E.ln_width) editorDrawGutter(&ab,filerow);

        if (filerow >= E.numrows) {
            if (!E.ln_width) abAppendLit(&ab,"~");
            abAppendLit(&ab,VT_CLEAR_EOL "\r\n");
            continue;
        }

        r = &E.row[filerow];

        /* Compute selection render-column range for this row. */
        int sel_rcol_start = -1, sel_rcol_end = -1;
        int sel_newline = 0; /* Is this row's newline inside the selection? */
        int sr, sc, er, ec;
        if (editorSelBounds(&sr,&sc,&er,&ec)) {
            if (filerow >= sr && filerow <= er) {
                sel_newline = filerow < er;
                int char_start = (filerow == sr) ? sc : 0;
                int char_end   = (filerow == er) ? ec : r->size;
                sel_rcol_start = editorCharToRenderCol(r, char_start);
                sel_rcol_end   = (filerow < er) ? r->rsize
                                                : editorCharToRenderCol(r, char_end);
            }
        }

        /* Spaces that end the line get a red background. */
        int trail = r->size;
        while (trail > 0 && r->chars[trail-1] == ' ') trail--;
        int trail_rcol = editorCharToRenderCol(r, trail);

        int len = r->rsize - E.coloff;
        int current_color = -1;
        int in_selection = 0;
        int cur_bg = BG_DEFAULT; /* Background SGR in effect, a BG_* value. */
        if (len > 0) {
            if (len > E.screencols) len = E.screencols;
            char *c = r->render+E.coloff;
            unsigned char *hl = r->hl+E.coloff;
            int j;
            /* Coalesce consecutive characters that share the same style into a
             * single append. 'run_start' marks the first not-yet-flushed char;
             * we flush the pending slice right before emitting any escape. */
            int run_start = 0;
            for (j = 0; j < len; j++) {
                int rcol = E.coloff + j;

                /* Handle selection highlight transitions. */
                if (sel_rcol_start >= 0) {
                    if (!in_selection && rcol >= sel_rcol_start && rcol < sel_rcol_end) {
                        if (j > run_start) abAppend(&ab,c+run_start,j-run_start);
                        run_start = j;
                        abAppendLit(&ab,VT_REVERSE);
                        in_selection = 1;
                        current_color = -1;
                    } else if (in_selection && rcol >= sel_rcol_end) {
                        if (j > run_start) abAppend(&ab,c+run_start,j-run_start);
                        run_start = j;
                        abAppendLit(&ab,VT_REVERSE_OFF);
                        in_selection = 0;
                        current_color = -1;
                    }
                }

                /* Background: a selection takes precedence, then the
                 * matched or unmatched brackets, then trailing spaces. */
                int want_bg = BG_DEFAULT;
                if (!in_selection) {
                    if (filerow == br_row && rcol == br_col)
                        want_bg = br_bg;
                    else if (filerow == bm_row && rcol == bm_col)
                        want_bg = br_bg;
                    else if (rcol >= trail_rcol)
                        want_bg = BG_TRAILING_SPACE;
                }
                if (want_bg != cur_bg) {
                    if (j > run_start) abAppend(&ab,c+run_start,j-run_start);
                    run_start = j;
                    char sgr[8];
                    int slen = snprintf(sgr,sizeof(sgr),"\x1b[%dm",want_bg);
                    abAppend(&ab,sgr,slen);
                    cur_bg = want_bg;
                }

                int ctrl = editorCtrlLen(c,j,len);
                if (ctrl) {
                    /* A control character is never sent as is, whatever the
                     * lexer made of it: draw it reversed in caret notation,
                     * ^A as 'A' and ESC as '[', and DEL or a C1 control as
                     * '?', one cell per byte. */
                    unsigned char b = (unsigned char)c[j];
                    char sym[2] = {'?','?'};
                    if (b < 0x20) sym[0] = '@'+b;
                    if (j > run_start) abAppend(&ab,c+run_start,j-run_start);
                    if (!in_selection) abAppendLit(&ab,VT_REVERSE);
                    abAppend(&ab,sym,ctrl);
                    abAppendLit(&ab,VT_RESET);
                    current_color = -1;
                    cur_bg = BG_DEFAULT; /* VT_RESET cleared it too. */
                    if (in_selection) abAppendLit(&ab,VT_REVERSE);
                    j += ctrl-1;
                    run_start = j+1; /* 'sym' replaces the bytes. */
                } else if (hl[j] == HL_NORMAL) {
                    if (current_color != -1) {
                        if (j > run_start) abAppend(&ab,c+run_start,j-run_start);
                        run_start = j;
                        abAppendLit(&ab,VT_FG_DEFAULT);
                        current_color = -1;
                    }
                    /* Character joins the pending run. */
                } else {
                    int color = editorSyntaxToColor(hl[j]);
                    if (color != current_color) {
                        if (j > run_start) abAppend(&ab,c+run_start,j-run_start);
                        run_start = j;
                        char buf[16];
                        int clen = snprintf(buf,sizeof(buf),"\x1b[%dm",color);
                        current_color = color;
                        abAppend(&ab,buf,clen);
                    }
                    /* Character joins the pending run. */
                }
            }
            /* Flush the trailing run. */
            if (j > run_start) abAppend(&ab,c+run_start,j-run_start);
        }
        abAppendLit(&ab,VT_FG_DEFAULT);
        /* Keep a colored background out of the EOL erase. */
        if (cur_bg != BG_DEFAULT) abAppendLit(&ab,VT_BG_DEFAULT);
        /* A selected newline extends the highlight to the edge of the screen
         * (erase to EOL with a reverse-video bg); otherwise it stops at the
         * last selected character. */
        if (sel_newline && !in_selection) abAppendLit(&ab,VT_REVERSE);
        if (!sel_newline && in_selection) abAppendLit(&ab,VT_REVERSE_OFF);
        abAppendLit(&ab,VT_CLEAR_EOL);
        if (sel_newline) abAppendLit(&ab,VT_REVERSE_OFF);
        abAppendLit(&ab,"\r\n");
    }

    /* Create a two rows status, spanning the gutter and text. First row: */
    int statuscols = E.ln_width + E.screencols;
    if (E.ft_open) ftBlankCell(&ab); /* continue the divider under the panes */
    abAppendLit(&ab,VT_CLEAR_EOL);
    abAppendLit(&ab,VT_REVERSE);
    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
        E.filename, E.numrows, E.dirty ? "(modified)" : "");
    long selchars = editorSelCharCount();
    int rlen = selchars ?
        snprintf(rstatus, sizeof(rstatus), "%ld selected  %d:%d",
            selchars,editorCursorCharCol(),E.rowoff+E.cy+1) :
        snprintf(rstatus, sizeof(rstatus), "%d:%d",
            editorCursorCharCol(),E.rowoff+E.cy+1);
    if (len > statuscols) len = statuscols;
    editorSanitize(status,len); /* The file name may hold anything. */
    abAppend(&ab,status,len);
    while(len < statuscols) {
        if (statuscols - len == rlen) {
            abAppend(&ab,rstatus,rlen);
            break;
        } else {
            abAppendLit(&ab," ");
            len++;
        }
    }
    abAppendLit(&ab,VT_RESET "\r\n");

    /* Second row depends on E.statusmsg and the status message update time. */
    if (E.ft_open) ftBlankCell(&ab);
    abAppendLit(&ab,VT_CLEAR_EOL);
    int msglen = strlen(E.statusmsg);
    if (msglen && time(NULL)-E.statusmsg_time < 5)
        abAppend(&ab,E.statusmsg,msglen <= statuscols ? msglen : statuscols);

    /* Put cursor at its current position. Note that the horizontal position
     * at which the cursor is displayed may be different compared to 'E.cx'
     * because of TABs. */
    if (E.ft_open && E.ft_focus) {
        /* Park the hardware cursor on the selected sidebar row. */
        int srow = E.ft_sel - E.ft_scroll;
        if (srow < 0) srow = 0;
        if (srow >= E.screenrows) srow = E.screenrows-1;
        snprintf(buf,sizeof(buf),"\x1b[%d;1H",srow+1);
        abAppend(&ab,buf,strlen(buf));
    } else {
        /* The text is drawn from render column 'coloff', so the cursor goes
         * at its char column's render column, past the sidebar and gutter. */
        int filerow = E.rowoff+E.cy;
        erow *row = (filerow >= E.numrows) ? NULL : &E.row[filerow];
        int col = E.coloff+E.cx;
        int tx = E.cx;
        if (row) {
            tx = editorCharToRenderCol(row,col) - E.coloff;
            if (col > row->size) tx += col - row->size; /* past end of line */
            if (tx < 0) tx = 0;
        }
        snprintf(buf,sizeof(buf),"\x1b[%d;%dH",E.cy+1,1+E.ft_width+E.ln_width+tx);
        abAppend(&ab,buf,strlen(buf));
    }
    abAppendLit(&ab,VT_CURSOR_SHOW); /* Show cursor. */
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
    /* Messages quote paths and file names, which may hold anything. */
    editorSanitize(E.statusmsg,(int)strlen(E.statusmsg));
    E.statusmsg_time = time(NULL);
}

/* =============================== Find mode ================================ */

#define QUIP_QUERY_LEN 256

static void editorSetCol(int new_col);

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
                E.rowoff = current;
                editorSetCol(editorRenderToCharCol(row,match_offset));
            }
        }
    }
}

/* ========================= Editor events handling  ======================== */

static void editorSetCol(int new_col) {
    if (new_col < E.screencols) {
        E.cx = new_col;
        E.coloff = 0;
    } else {
        E.coloff = new_col - E.screencols + 1;
        E.cx = E.screencols - 1;
    }
}

/* Move the cursor to file position (row,col), scrolling it into view. */
static void editorSetCursorPos(int row, int col) {
    if (row < E.rowoff) {
        E.rowoff = row; E.cy = 0;
    } else if (row >= E.rowoff + E.screenrows) {
        E.rowoff = row - E.screenrows + 1;
        E.cy = E.screenrows - 1;
    } else {
        E.cy = row - E.rowoff;
    }
    editorSetCol(col);
}

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
        /* The line ends left of the view: scroll back so it is visible. */
        if (E.cx < 0) editorSetCol(rowlen);
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

/* Remove 'len' chars at 'at' from 'row'. */
static void editorRowDelRange(erow *row, int at, int len) {
    memmove(row->chars+at,row->chars+at+len,row->size-at-len+1);
    row->size -= len;
}

/* Insert the 'len' chars of 's' at 'at' in 'row'. */
static void editorRowInsRange(erow *row, int at, const char *s, int len) {
    row->chars = growBuf(row->chars,&row->chars_cap,row->size+len+1);
    memmove(row->chars+at+len,row->chars+at,row->size-at+1);
    memcpy(row->chars+at,s,len);
    row->size += len;
}

/* Where a cursor at 'col' lands after 'len' chars at 'at' are removed. */
static int editorColAfterDel(int col, int at, int len) {
    if (col >= at+len) return col-len;
    return col > at ? at : col;
}

/* If the text of 'row' between 'start' and 'end' is exactly one block
 * comment, return 1. "(* a *) x (* b *)" starts and ends like one but is
 * two, so this follows the comment's nesting to its real end. */
static int editorRowIsOneBlockComment(erow *row, int start, int end) {
    const hldelim *mcs = &E.syntax->block_start;
    const hldelim *mce = &E.syntax->block_end;
    char *p = row->chars;
    if (end-start < mcs->len+mce->len || !hlAt(p,start,end,mcs)) return 0;
    int depth = 1;
    for (int i = start+mcs->len; i < end; ) {
        if (hlAt(p,i,end,mce)) {
            i += mce->len;
            if (--depth == 0) return i == end;
        } else if ((E.syntax->flags & HL_NESTED_COMMENTS) &&
                   hlAt(p,i,end,mcs)) {
            i += mcs->len;
            depth++;
        } else {
            i++;
        }
    }
    return 0;
}

/* True if Ctrl-/ has something to do on 'filerow'. A language without line
 * comments wraps lines in a block comment, which a blank line does not get. */
int editorCanToggleComment(int filerow) {
    if (filerow >= E.numrows || E.syntax == NULL) return 0;
    if (E.syntax->line_comment.len) return 1;
    if (!E.syntax->block_start.len) return 0;
    erow *row = &E.row[filerow];
    for (int i = 0; i < row->size; i++)
        if (!isspace((unsigned char)row->chars[i])) return 1;
    return 0;
}

/* Ctrl-/: toggle a line comment on 'filerow'. Languages without line
 * comments (OCaml) wrap the line in "(* " ... " *)" instead, or unwrap it.
 * The cursor stays on the same character. */
void editorToggleLineComment(int filerow) {
    if (!editorCanToggleComment(filerow)) return;

    erow *row = &E.row[filerow];
    const hldelim *scs = &E.syntax->line_comment;
    const hldelim *mcs = &E.syntax->block_start;
    const hldelim *mce = &E.syntax->block_end;
    int col = E.coloff + E.cx;
    int on_row = filerow == E.rowoff + E.cy;

    int indent = 0;
    while (indent < row->size && isspace((unsigned char)row->chars[indent]))
        indent++;

    if (scs->len) {
        if (hlAt(row->chars,indent,row->size,scs)) {
            editorRowDelRange(row,indent,scs->len);
            col = editorColAfterDel(col,indent,scs->len);
        } else {
            editorRowInsRange(row,indent,scs->s,scs->len);
            if (col >= indent) col += scs->len;
        }
    } else {
        int end = row->size;
        while (end > indent && isspace((unsigned char)row->chars[end-1]))
            end--;
        if (editorRowIsOneBlockComment(row,indent,end)) {
            /* Unwrap, taking one space of padding on each side with it. */
            int close = end-mce->len;
            if (close > indent+mcs->len && row->chars[close-1] == ' ') close--;
            editorRowDelRange(row,close,end-close);
            col = editorColAfterDel(col,close,end-close);
            int open = mcs->len;
            if (indent+open < close && row->chars[indent+open] == ' ') open++;
            editorRowDelRange(row,indent,open);
            col = editorColAfterDel(col,indent,open);
        } else {
            editorRowInsRange(row,end,mce->s,mce->len);
            editorRowInsRange(row,end," ",1);
            if (col > end) col += mce->len+1;
            editorRowInsRange(row,indent," ",1);
            editorRowInsRange(row,indent,mcs->s,mcs->len);
            if (col >= indent) col += mcs->len+1;
        }
    }
    editorUpdateRow(row);
    E.dirty++;
    if (on_row) editorSetCol(col);
}

/* True if 'filerow' starts with at least one tab stop's worth of spaces. */
static int editorCanOutdent(int filerow) {
    if (filerow >= E.numrows || E.row[filerow].size < QUIP_TAB_STOP) return 0;
    for (int i = 0; i < QUIP_TAB_STOP; i++)
        if (E.row[filerow].chars[i] != ' ') return 0;
    return 1;
}

/* Shift-Tab: drop one tab stop's worth of leading spaces from the cursor's
 * line. The cursor and a selection anchor on that line shift left with the
 * text, stopping at the start of the line. */
void editorOutdentLine(void) {
    int filerow = E.rowoff + E.cy;
    if (!editorCanOutdent(filerow)) return;
    erow *row = &E.row[filerow];
    int n = QUIP_TAB_STOP;
    memmove(row->chars, row->chars + n, row->size - n + 1);
    row->size -= n;
    editorUpdateRow(row);
    E.dirty++;
    int col = E.coloff + E.cx;
    editorSetCol(col > n ? col - n : 0);
    if (E.sel_active && E.sel_anchor_row == filerow)
        E.sel_anchor_col = E.sel_anchor_col > n ? E.sel_anchor_col - n : 0;
}

void editorDeleteSelection(void) {
    int sr, sc, er, ec;
    if (!editorSelBounds(&sr,&sc,&er,&ec)) { editorSelClear(); return; }

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

    editorSetCursorPos(sr, sc);
    E.sel_active = 0;
}

/* Put the cursor as close to file column 'col' as possible without moving
 * the view. If the cursor's line ends left of the view, the cursor parks at
 * the left edge past the end of the line, and the next key that acts on it
 * snaps it back (see editorProcessKeypress). */
static void editorPlaceCursorInView(int col) {
    int filerow = E.rowoff + E.cy;
    int len = (filerow < E.numrows) ? E.row[filerow].size : 0;
    if (col > len) col = len;
    if (col < E.coloff) col = E.coloff;
    if (col > E.coloff + E.screencols - 1) col = E.coloff + E.screencols - 1;
    E.cx = col - E.coloff;
}

/* Scroll the view sideways by 'delta' columns, stopping once the end of the
 * longest line on screen is in view. The cursor keeps its column when it can
 * and is otherwise dragged along with the view. */
static void editorScrollCols(int delta) {
    int col = E.coloff + E.cx;
    int maxlen = 0;
    for (int y = 0; y < E.screenrows && E.rowoff + y < E.numrows; y++) {
        int len = E.row[E.rowoff + y].rsize;
        if (len > maxlen) maxlen = len;
    }
    int limit = maxlen - E.screencols + 1; /* +1 leaves room for the cursor */
    int coloff = E.coloff + delta;
    if (delta > 0 && coloff > limit) coloff = (limit > E.coloff) ? limit : E.coloff;
    if (coloff < 0) coloff = 0;
    E.coloff = coloff;
    editorPlaceCursorInView(col);
}

/* True if the cursor sits past the end of its line, which only a scroll can
 * cause. */
static int editorCursorPastEnd(void) {
    int filerow = E.rowoff + E.cy;
    int len = (filerow < E.numrows) ? E.row[filerow].size : 0;
    return E.coloff + E.cx > len;
}

/* Moves cursor to E.mouse_cx/cy, converting render column → file column.
 * A click below the last line lands at the end of the last line. */
static void editorMouseSetCursor(void) {
    if (E.mouse_cy >= 0 && E.mouse_cy < E.screenrows)
        E.cy = E.mouse_cy;
    int last = E.numrows ? E.numrows - 1 : 0;
    if (E.rowoff + E.cy > last) {
        if (last < E.rowoff) E.rowoff = last;
        E.cy = last - E.rowoff;
        editorSetCol(E.numrows ? E.row[last].size : 0);
        return;
    }
    int text_cx = E.mouse_cx - E.ft_width - E.ln_width; /* physical → text */
    if (text_cx < 0 && E.mouse_cx >= E.ft_width) text_cx = 0; /* in gutter */
    if (text_cx >= 0 && text_cx < E.screencols) {
        int file_row = E.rowoff + E.cy;
        if (file_row < E.numrows) {
            editorSetCol(editorRenderToCharCol(&E.row[file_row],
                                               E.coloff + text_cx));
        } else {
            editorSetCol(0);
        }
    }
}

/* Character class for double-click word selection: a run of word
 * characters, of whitespace, or of punctuation selects as one unit. Bytes
 * >= 0x80 count as word characters so UTF-8 words stay whole. */
static int editorWordClass(int c) {
    if (isalnum(c) || c == '_' || c >= 0x80) return 2;
    if (isspace(c)) return 0;
    return 1;
}

/* Bounds [*start,*end) of the run of same-class characters at 'col' in
 * 'row'. A column past the end of the line picks the line's last run. */
static void editorWordAt(erow *row, int col, int *start, int *end) {
    if (row->size == 0) { *start = *end = 0; return; }
    if (col >= row->size) col = row->size - 1;
    unsigned char *s = (unsigned char *)row->chars;
    int cls = editorWordClass(s[col]);
    int a = col, b = col + 1;
    while (a > 0 && editorWordClass(s[a-1]) == cls) a--;
    while (b < row->size && editorWordClass(s[b]) == cls) b++;
    *start = a; *end = b;
}

/* File column of the character under the mouse, after editorMouseSetCursor.
 * That puts the cursor on the nearest boundary, which for a click inside a
 * TAB is just past it; step back so the TAB itself counts as clicked. */
static int editorMouseCharCol(void) {
    int filerow = E.rowoff + E.cy;
    int col = E.coloff + E.cx;
    if (filerow >= E.numrows || col == 0) return col;
    int rcol = E.coloff + E.mouse_cx - E.ft_width - E.ln_width;
    if (editorCharToRenderCol(&E.row[filerow], col) > rcol) col--;
    return col;
}

/* The unit under the mouse for a multi-click: the word (clicks == 2) or the
 * whole line including its newline (clicks == 3), as (sr,sc) up to (er,ec).
 * The last line has no newline, so its unit ends at the end of the line.
 * Moves the cursor to the mouse first. Returns 0 if there is no text. */
static int editorMouseUnit(int clicks, int *sr, int *sc, int *er, int *ec) {
    editorMouseSetCursor();
    int filerow = E.rowoff + E.cy;
    if (filerow >= E.numrows) return 0;
    *sr = *er = filerow;
    if (clicks == 2) {
        editorWordAt(&E.row[filerow], editorMouseCharCol(), sc, ec);
    } else if (filerow + 1 < E.numrows) {
        *sc = 0; *er = filerow + 1; *ec = 0;
    } else {
        *sc = 0; *ec = E.row[filerow].size;
    }
    return 1;
}

/* Select exactly the clicked unit. The cursor goes to the unit's end when
 * that is on the clicked row, else to its start: a line's newline ends on
 * the next row, which may be off screen, and the view must not scroll. */
static void editorMouseSelectClickedUnit(void) {
    E.sel_active = 1;
    if (E.unit_er == E.unit_sr) {
        E.sel_anchor_row = E.unit_sr; E.sel_anchor_col = E.unit_sc;
        editorSetCursorPos(E.unit_er, E.unit_ec);
    } else {
        E.sel_anchor_row = E.unit_er; E.sel_anchor_col = E.unit_ec;
        editorSetCursorPos(E.unit_sr, E.unit_sc);
    }
}

/* Start a double- or triple-click selection of the unit under the mouse. */
static void editorMouseSelectUnit(int clicks) {
    int sr, sc, er, ec;
    if (!editorMouseUnit(clicks, &sr, &sc, &er, &ec)) {
        int row = E.rowoff + E.cy, col = E.coloff + E.cx;
        sr = er = row; sc = ec = col;
    }
    E.unit_sr = sr; E.unit_sc = sc; E.unit_er = er; E.unit_ec = ec;
    editorMouseSelectClickedUnit();
}

/* Extend a multi-click selection by whole units (words or lines) toward the
 * mouse, keeping the originally clicked unit selected. */
static void editorMouseExtendUnits(int clicks) {
    int sr, sc, er, ec;
    if (!editorMouseUnit(clicks, &sr, &sc, &er, &ec)) return;
    if (sr < E.unit_sr || (sr == E.unit_sr && sc < E.unit_sc)) {
        E.sel_anchor_row = E.unit_er; E.sel_anchor_col = E.unit_ec;
        editorSetCursorPos(sr, sc);
    } else if (er > E.unit_er || (er == E.unit_er && ec > E.unit_ec)) {
        E.sel_anchor_row = E.unit_sr; E.sel_anchor_col = E.unit_sc;
        editorSetCursorPos(er, ec);
    } else {
        editorMouseSelectClickedUnit(); /* back inside the clicked unit */
    }
}

/* Register a left press and return how many presses in a row it makes:
 * 2 is a double-click, 3 a triple-click (same cell, each press within 400ms
 * of the previous one). */
static int editorCountClick(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long ms = (now.tv_sec - E.click_time.tv_sec) * 1000 +
              (now.tv_nsec - E.click_time.tv_nsec) / 1000000;
    if (E.click_count && ms <= 400 &&
        E.mouse_cx == E.click_cx && E.mouse_cy == E.click_cy)
        E.click_count++;
    else
        E.click_count = 1;
    if (E.click_count > 3) E.click_count = 1; /* a fourth press starts over */
    E.click_time = now;
    E.click_cx = E.mouse_cx;
    E.click_cy = E.mouse_cy;
    return E.click_count;
}

/* ========================= Clipboard ======================================*/

/* Returns malloc'd string of selected text, sets *outlen. Caller frees.
 * Returns NULL if no selection or zero-length selection. */
static char *editorGetSelection(int *outlen) {
    int sr, sc, er, ec;
    if (!editorSelBounds(&sr,&sc,&er,&ec)) return NULL;

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

    writeLit(STDOUT_FILENO,VT_CLIPBOARD_QUERY);

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

/* Produce src with bytes [pos, pos+rmlen) replaced by 'ins' (inslen bytes).
 * Returns a freshly allocated, NUL-terminated buffer; *outlen excludes NUL. */
static char *editorSplice(const char *src, int srclen, int pos, int rmlen,
                          const char *ins, int inslen, int *outlen) {
    int nl = srclen - rmlen + inslen;
    char *out = malloc(nl + 1);
    memcpy(out, src, pos);
    memcpy(out + pos, ins, inslen);
    memcpy(out + pos + inslen, src + pos + rmlen, srclen - pos - rmlen);
    out[nl] = '\0';
    *outlen = nl;
    return out;
}

/* Tear down the current rows and rebuild them from serialized 'text' (rows
 * separated by '\n'). Cursor/dirty state is set by the caller. */
/* Free every row buffer and reset the row array to empty. */
static void editorFreeAllRows(void) {
    for (int i = 0; i < E.numrows; i++) {
        free(E.row[i].render);
        free(E.row[i].chars);
        free(E.row[i].hl);
    }
    free(E.row);
    E.row     = NULL;
    E.numrows = 0;
    E.rowcap  = 0;
}

static void editorRebuildRows(char *text, int len) {
    editorFreeAllRows();

    char *p   = text;
    int   rem = len;
    while (rem > 0) {
        char *nl      = memchr(p, '\n', rem);
        int   linelen = nl ? (int)(nl - p) : rem;
        editorInsertRow(E.numrows, p, linelen);
        if (!nl) break;
        rem -= linelen + 1;
        p   += linelen + 1;
    }
}

void editorPushSnapshot(void) {
    /* Discard any redo entries above undo_cur. */
    for (int i = undo_cur + 1; i < undo_count; i++) {
        free(undo_hist[i].old);
        free(undo_hist[i].new);
    }
    undo_count = undo_cur + 1;

    /* If the history is full, drop the oldest entry. */
    if (undo_count == UNDO_HISTORY_MAX) {
        free(undo_hist[0].old);
        free(undo_hist[0].new);
        memmove(&undo_hist[0], &undo_hist[1],
                (UNDO_HISTORY_MAX - 1) * sizeof(editorSnapshot));
        undo_count--;
        if (undo_cur > 0) undo_cur--;
        if (undo_saved > 0) undo_saved--;
        else if (undo_saved == 0) undo_saved = -1;
        /* The new head keeps its cursor state but its delta now has no
         * predecessor and is never applied, so release it. */
        free(undo_hist[0].old);
        free(undo_hist[0].new);
        undo_hist[0].old = NULL; undo_hist[0].oldlen = 0;
        undo_hist[0].new = NULL; undo_hist[0].newlen = 0;
        undo_hist[0].pos = 0;
    }

    /* Serialize the current document and diff it against the base (the text of
     * the snapshot at undo_cur), storing only the changed middle span. */
    int curlen;
    char *cur = editorRowsToString(&curlen);

    int minlen = (undo_base_len < curlen) ? undo_base_len : curlen;
    int p = 0, s = 0;
    while (p < minlen && undo_base[p] == cur[p]) p++;
    while (s < minlen - p &&
           undo_base[undo_base_len-1-s] == cur[curlen-1-s]) s++;

    editorSnapshot *e = &undo_hist[undo_count];
    e->pos    = p;
    e->oldlen = undo_base_len - s - p;
    e->newlen = curlen - s - p;
    e->old    = malloc(e->oldlen);
    memcpy(e->old, undo_base + p, e->oldlen);
    e->new    = malloc(e->newlen);
    memcpy(e->new, cur + p, e->newlen);
    e->cx     = E.cx;
    e->cy     = E.cy;
    e->rowoff = E.rowoff;
    e->coloff = E.coloff;
    undo_cur = undo_count;
    undo_count++;

    /* The current document is now the base. */
    free(undo_base);
    undo_base     = cur;
    undo_base_len = curlen;
}

void editorUndo(void) {
    if (undo_cur <= 0) {
        editorSetStatusMessage("Nothing to undo.");
        return;
    }
    /* Reverse the delta at undo_cur to reconstruct the previous text. */
    editorSnapshot *e = &undo_hist[undo_cur];
    int prevlen;
    char *prev = editorSplice(undo_base, undo_base_len, e->pos, e->newlen,
                              e->old, e->oldlen, &prevlen);
    free(undo_base);
    undo_base     = prev;
    undo_base_len = prevlen;
    undo_cur--;

    editorRebuildRows(undo_base, undo_base_len);
    editorSnapshot *t = &undo_hist[undo_cur];
    E.cx     = t->cx;
    E.cy     = t->cy;
    E.rowoff = t->rowoff;
    E.coloff = t->coloff;
    E.dirty  = (undo_cur != undo_saved);
    E.sel_active = 0;
    undo_last_insert = 0;
}

void editorRedo(void) {
    if (undo_cur >= undo_count - 1) {
        editorSetStatusMessage("Nothing to redo.");
        return;
    }
    undo_cur++;
    /* Apply the delta at undo_cur to reconstruct the next text. */
    editorSnapshot *e = &undo_hist[undo_cur];
    int nextlen;
    char *next = editorSplice(undo_base, undo_base_len, e->pos, e->oldlen,
                              e->new, e->newlen, &nextlen);
    free(undo_base);
    undo_base     = next;
    undo_base_len = nextlen;

    editorRebuildRows(undo_base, undo_base_len);
    E.cx     = e->cx;
    E.cy     = e->cy;
    E.rowoff = e->rowoff;
    E.coloff = e->coloff;
    E.dirty  = (undo_cur != undo_saved);
    E.sel_active = 0;
    undo_last_insert = 0;
}

/* ================================ File tree =============================== */

/* A node in the file-tree sidebar. Directories load their children lazily the
 * first time they are expanded, so a huge project is never walked eagerly. */
typedef struct ftnode {
    char *name;              /* Basename shown in the sidebar. */
    char *path;              /* Full path, used to open / descend. */
    int   is_dir;
    int   depth;             /* Indentation level (root = 0). */
    int   expanded;          /* Directories only. */
    int   loaded;            /* Children already read? */
    struct ftnode **child;   /* Child node pointers. */
    int   nchild;
} ftnode;

#define FT_WIDTH 30      /* Sidebar width in columns (including the divider). */
#define FT_MIN_TEXT 20   /* Minimum text columns; refuse to open below this. */

int editorLoadFile(const char *path);
void updateWindowSize(void);

/* Sort directories before files, then case-insensitively by name. */
static int ftCmp(const void *a, const void *b) {
    const ftnode *x = *(const ftnode *const *)a;
    const ftnode *y = *(const ftnode *const *)b;
    if (x->is_dir != y->is_dir) return y->is_dir - x->is_dir;
    return strcasecmp(x->name, y->name);
}

/* Read the entries of 'dir' into dir->child. Marks the node loaded either way;
 * on failure it just stays childless and reports via the status message. */
static void ftReadDir(ftnode *dir) {
    dir->loaded = 1;
    DIR *d = opendir(dir->path);
    if (!d) {
        editorSetStatusMessage("Can't open %s: %s", dir->path, strerror(errno));
        return;
    }
    int cap = 0;
    struct dirent *de;
    size_t plen = strlen(dir->path);
    int rootslash = (plen == 1 && dir->path[0] == '/'); /* avoid "//name" */
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        ftnode *c = calloc(1, sizeof(ftnode));
        size_t nlen = strlen(de->d_name);
        c->name = malloc(nlen+1);
        memcpy(c->name, de->d_name, nlen+1);
        c->path = malloc(plen + (rootslash ? 0 : 1) + nlen + 1);
        memcpy(c->path, dir->path, plen);
        size_t off = plen;
        if (!rootslash) c->path[off++] = '/';
        memcpy(c->path + off, de->d_name, nlen+1);
        c->depth = dir->depth + 1;
        struct stat st;
        c->is_dir = (stat(c->path, &st) == 0 && S_ISDIR(st.st_mode));
        dir->child = growBuf(dir->child, &cap,
                             (dir->nchild+1) * (int)sizeof(ftnode *));
        dir->child[dir->nchild++] = c;
    }
    closedir(d);
    if (dir->nchild)
        qsort(dir->child, dir->nchild, sizeof(ftnode *), ftCmp);
}

/* Recursively free a node and its subtree. */
static void ftFreeNode(ftnode *n) {
    if (!n) return;
    for (int i = 0; i < n->nchild; i++) ftFreeNode(n->child[i]);
    free(n->child);
    free(n->name);
    free(n->path);
    free(n);
}

/* Build the tree root from the directory containing 'filepath'. */
static void ftBuildRoot(const char *filepath) {
    char *dirpath;
    const char *slash = strrchr(filepath, '/');
    if (!slash) {
        dirpath = malloc(2); dirpath[0] = '.'; dirpath[1] = '\0';
    } else if (slash == filepath) {
        dirpath = malloc(2); dirpath[0] = '/'; dirpath[1] = '\0';
    } else {
        size_t n = slash - filepath;
        dirpath = malloc(n+1); memcpy(dirpath, filepath, n); dirpath[n] = '\0';
    }
    ftnode *root = calloc(1, sizeof(ftnode));
    root->path = dirpath;
    const char *rs = strrchr(dirpath, '/');
    const char *base = (rs && rs[1]) ? rs+1 : dirpath;
    /* A file opened by a bare name lives in ".": label it with the name of
     * the working directory rather than a lone dot. */
    char cwd[4096];
    if (!slash && getcwd(cwd, sizeof(cwd))) {
        char *cs = strrchr(cwd, '/');
        base = (cs && cs[1]) ? cs+1 : cwd;
    }
    size_t blen = strlen(base);
    root->name = malloc(blen+1); memcpy(root->name, base, blen+1);
    root->is_dir = 1;
    root->depth = 0;
    root->expanded = 1;
    ftReadDir(root);
    E.ft_root = root;
}

/* Append one node pointer to the flattened visible list. */
static void ftAppendVis(ftnode *n) {
    E.ft_vis = growBuf(E.ft_vis, &E.ft_vcap,
                       (E.ft_vcount+1) * (int)sizeof(ftnode *));
    E.ft_vis[E.ft_vcount++] = n;
}

static void ftFlattenNode(ftnode *n) {
    ftAppendVis(n);
    if (n->is_dir && n->expanded)
        for (int i = 0; i < n->nchild; i++) ftFlattenNode(n->child[i]);
}

/* Rebuild ft_vis from the tree, honouring each directory's expanded flag. */
static void ftFlatten(void) {
    E.ft_vcount = 0;
    if (E.ft_root) ftFlattenNode(E.ft_root);
    if (E.ft_sel >= E.ft_vcount) E.ft_sel = E.ft_vcount ? E.ft_vcount-1 : 0;
    if (E.ft_sel < 0) E.ft_sel = 0;
}

/* Draw a blank sidebar cell (spaces + divider) of exactly ft_width columns. */
static void ftBlankCell(struct abuf *ab) {
    char buf[FT_WIDTH];
    int i;
    for (i = 0; i < E.ft_width-1; i++) buf[i] = ' ';
    buf[E.ft_width-1] = '|';
    abAppend(ab, buf, E.ft_width);
}

/* Draw the sidebar cell for screen row 'y' into the append buffer. */
static void ftDrawSidebarRow(struct abuf *ab, int y) {
    int W = E.ft_width - 1;   /* Text columns; last column is the divider. */
    char line[FT_WIDTH];
    int n = 0;
    int idx = E.ft_scroll + y;
    int has = (idx >= 0 && idx < E.ft_vcount);
    ftnode *node = has ? E.ft_vis[idx] : NULL;

    if (has) {
        int indent = node->depth * 2;
        if (indent > W-3) indent = W-3;
        if (indent < 0) indent = 0;
        while (n < indent && n < W) line[n++] = ' ';
        if (n < W) line[n++] = node->is_dir ? (node->expanded ? '-' : '+') : ' ';
        if (n < W) line[n++] = ' ';
        const char *p = node->name;
        while (*p && n < W) line[n++] = *p++;
        if (*p && W > 0) line[W-1] = '~'; /* name was truncated */
    }
    while (n < W) line[n++] = ' ';
    editorSanitize(line,W); /* File names may hold anything. */

    int selected = (has && idx == E.ft_sel);
    if (selected)               abAppendLit(ab,VT_REVERSE);
    else if (has && node->is_dir) abAppendLit(ab,VT_BOLD_BLUE);
    abAppend(ab, line, W);
    if (selected || (has && node->is_dir)) abAppendLit(ab,VT_RESET);
    abAppendLit(ab,"|");
}

/* Move the selection by 'delta' rows, keeping it inside the visible pane. */
static void ftMoveSel(int delta) {
    if (!E.ft_vcount) return;
    E.ft_sel += delta;
    if (E.ft_sel < 0) E.ft_sel = 0;
    if (E.ft_sel >= E.ft_vcount) E.ft_sel = E.ft_vcount-1;
    if (E.ft_sel < E.ft_scroll) E.ft_scroll = E.ft_sel;
    if (E.ft_sel >= E.ft_scroll + E.screenrows)
        E.ft_scroll = E.ft_sel - E.screenrows + 1;
}

/* Enter / right: expand-or-collapse a directory, or open a file. */
static void ftActivate(void) {
    if (E.ft_sel < 0 || E.ft_sel >= E.ft_vcount) return;
    ftnode *n = E.ft_vis[E.ft_sel];
    if (n->is_dir) {
        n->expanded = !n->expanded;
        if (n->expanded && !n->loaded) ftReadDir(n);
        ftFlatten();
    } else {
        editorLoadFile(n->path);
    }
}

/* Left: collapse an expanded directory, else jump to the parent entry. */
static void ftCollapse(void) {
    if (E.ft_sel < 0 || E.ft_sel >= E.ft_vcount) return;
    ftnode *n = E.ft_vis[E.ft_sel];
    if (n->is_dir && n->expanded) {
        n->expanded = 0;
        ftFlatten();
        return;
    }
    for (int i = E.ft_sel-1; i >= 0; i--) {
        if (E.ft_vis[i]->depth < n->depth) { E.ft_sel = i; break; }
    }
    if (E.ft_sel < E.ft_scroll) E.ft_scroll = E.ft_sel;
}

/* Show or hide the sidebar, recomputing the text-area geometry. */
static void ftToggle(void) {
    if (!E.ft_open) {
        if (E.termcols < FT_WIDTH + FT_MIN_TEXT) {
            editorSetStatusMessage("Terminal too narrow for the file tree");
            return;
        }
        if (!E.ft_root) ftBuildRoot(E.filename);
        E.ft_width  = FT_WIDTH;
        E.ft_open   = 1;
        E.ft_focus  = 1;
        E.ft_scroll = 0;
        ftFlatten();
    } else {
        E.ft_open  = 0;
        E.ft_focus = 0;
        E.ft_width = 0;
    }
    editorUpdateGeometry();
}

/* Handle a key while the sidebar has focus. Returns 1 if the key was consumed. */
static int ftHandleKey(int c) {
    switch (c) {
    case ARROW_UP:   ftMoveSel(-1); return 1;
    case ARROW_DOWN: ftMoveSel(1);  return 1;
    case PAGE_UP:    ftMoveSel(-E.screenrows); return 1;
    case PAGE_DOWN:  ftMoveSel(E.screenrows);  return 1;
    case SCROLL_UP:   ftMoveSel(-3); return 1;
    case SCROLL_DOWN: ftMoveSel(3);  return 1;
    case ENTER:
    case ARROW_RIGHT: ftActivate(); return 1;
    case ARROW_LEFT:  ftCollapse(); return 1;
    case ESC: E.ft_focus = 0; return 1;
    default: return 0;
    }
}

/* Replace the current buffer with the file at 'path'. If the buffer has
 * unsaved changes, ask for confirmation first (a small modal, in the style of
 * editorFind). Returns 0 on success, 1 if the load was declined or failed. */
int editorLoadFile(const char *path) {
    /* Check readability before anything is discarded: editorOpen treats an
     * unreadable file as fatal, which is right at startup but not here. */
    FILE *fp = fopen(path, "r");
    if (!fp) {
        editorSetStatusMessage("Can't open %s: %s", path, strerror(errno));
        return 1;
    }
    fclose(fp);

    if (E.dirty) {
        editorSetStatusMessage("Discard unsaved changes? (y/N)");
        editorRefreshScreen();
        int c = editorReadKey(STDIN_FILENO);
        if (c != 'y' && c != 'Y') {
            editorSetStatusMessage("");
            return 1;
        }
    }

    /* Tear down the current document and its undo history. */
    editorFreeAllRows();
    for (int i = 0; i < undo_count; i++) {
        free(undo_hist[i].old);
        free(undo_hist[i].new);
    }
    undo_cur = -1;
    undo_count = 0;
    undo_saved = -1;
    undo_last_insert = 0;
    free(undo_base);
    undo_base = NULL;
    undo_base_len = 0;

    E.cx = E.cy = E.rowoff = E.coloff = 0;
    E.sel_active = 0;

    char *p = strdup(path); /* editorOpen/SelectSyntax take char*. */
    editorSelectSyntaxHighlight(p);
    editorOpen(p);
    free(p);

    /* Seed the undo baseline, exactly as main() does at startup. */
    editorPushSnapshot();
    undo_saved = undo_cur;
    E.dirty = 0;

    E.ft_focus = 0; /* Hand focus to the editor so the user can type. */
    editorSetStatusMessage("");
    return 0;
}

/* ========================================================================= */

/* Process events arriving from the standard input, which is, the user
 * is typing stuff on the terminal. */
#define QUIP_QUIT_TIMES 3
#define QUIP_HSCROLL_COLS 4 /* Columns moved per horizontal wheel step. */
void editorProcessKeypress(int fd) {
    /* When the file is modified, requires Ctrl-q to be pressed N times
     * before actually quitting. */
    static int quit_times = QUIP_QUIT_TIMES;

    int c = editorReadKey(fd);

    /* While the sidebar has focus, navigation keys drive the tree. Ctrl-E
     * closes it and Ctrl-Q still quits; everything else is consumed. */
    if (E.ft_open && E.ft_focus) {
        if (c == CTRL_E) { ftToggle(); return; }
        if (c != CTRL_Q && c != SCROLL_LEFT && c != SCROLL_RIGHT) {
            ftHandleKey(c);
            return;
        }
    }

    /* After a sideways scroll the cursor may be parked past the end of a
     * short line. Snap it back to the line end before a key acts on it, so
     * edits and selections never see a column beyond the line. Vertical
     * motion, scrolling, mouse events and view-neutral commands keep the
     * view where the user put it. */
    if (editorCursorPastEnd()) {
        switch (c) {
        case KEY_NULL: case CTRL_Q: case CTRL_S: case CTRL_E: case CTRL_F:
        case CTRL_L:
        case ARROW_UP: case ARROW_DOWN: case PAGE_UP: case PAGE_DOWN:
        case ALT_ARROW_UP: case ALT_ARROW_DOWN:
        case SCROLL_UP: case SCROLL_DOWN: case SCROLL_LEFT: case SCROLL_RIGHT:
        case MOUSE_CLICK: case MOUSE_DRAG: case MOUSE_RELEASE:
            break;
        default: {
            int filerow = E.rowoff + E.cy;
            editorSetCol(filerow < E.numrows ? E.row[filerow].size : 0);
            break;
        }
        }
    }

    switch(c) {
    case CTRL_E:        /* Ctrl-e — toggle / focus the file tree */
        if (E.ft_open) E.ft_focus = 1;
        else ftToggle();
        break;
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
        ftFreeNode(E.ft_root);
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
        if (!editorCanToggleComment(E.rowoff + E.cy)) break;
        editorPushSnapshot();
        undo_last_insert = 0;
        editorSelClear();
        editorToggleLineComment(E.rowoff + E.cy);
        break;
    case SHIFT_TAB:     /* Shift-Tab — outdent the current line */
        if (!editorCanOutdent(E.rowoff + E.cy)) break;
        editorPushSnapshot();
        undo_last_insert = 0;
        editorOutdentLine();
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
        if (E.ft_open && E.mouse_cx < E.ft_width) {
            /* Click inside the sidebar: focus it and act on the row. */
            E.click_count = 0;
            E.ft_focus = 1;
            int idx = E.ft_scroll + E.mouse_cy;
            if (idx >= 0 && idx < E.ft_vcount) { E.ft_sel = idx; ftActivate(); }
            break;
        }
        E.ft_focus = 0;
        if (editorCountClick() >= 2) {
            /* Double-click selects the word, triple-click the line. */
            editorMouseSelectUnit(E.click_count);
            break;
        }
        editorMouseSetCursor();
        E.sel_active     = 1;
        E.sel_anchor_row = E.rowoff + E.cy;
        E.sel_anchor_col = E.coloff + E.cx;
        break;
    case MOUSE_DRAG:
        if (!E.sel_active) break;
        if (E.click_count >= 2) editorMouseExtendUnits(E.click_count);
        else editorMouseSetCursor();
        break;
    case MOUSE_RELEASE:
        if (E.ft_open && E.mouse_cx < E.ft_width) break;
        if (E.click_count >= 2) break; /* keep the word/line selection */
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
        editorPlaceCursorInView(E.coloff + E.cx);
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
        editorPlaceCursorInView(E.coloff + E.cx);
        break;
    }
    case SCROLL_LEFT:
        undo_last_insert = 0;
        editorScrollCols(-QUIP_HSCROLL_COLS);
        break;
    case SCROLL_RIGHT:
        undo_last_insert = 0;
        editorScrollCols(QUIP_HSCROLL_COLS);
        break;
    case KEY_NULL:
        break;
    case PAGE_UP:
    case PAGE_DOWN:
        undo_last_insert = 0;
        editorSelClear();
        if (c == PAGE_UP && E.cy != 0)
            E.cy = 0;
        else if (c == PAGE_DOWN && E.cy != E.screenrows-1)
            E.cy = E.screenrows-1;
        /* Not below the line after the last one, where Down stops too: a
         * short file does not fill the screen. */
        if (E.rowoff+E.cy > E.numrows) E.cy = E.numrows-E.rowoff;
        if (E.cy < 0) E.cy = 0;
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
    case HOME_KEY:
        undo_last_insert = 0;
        editorSelClear(); editorMoveLineStart();              break;
    case CTRL_ARROW_RIGHT:
    case END_KEY:
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
        /* Soft key codes are not text; never insert one as a byte. */
        if (c >= ARROW_LEFT) break;
        if (!undo_last_insert || E.sel_active) {
            editorPushSnapshot();
            undo_last_insert = 1;
        }
        if (E.sel_active) editorDeleteSelection();
        if (c == TAB) editorInsertTab();
        else editorInsertChar(c);
        break;
    }

    quit_times = QUIP_QUIT_TIMES; /* Reset it to the original value. */
}

int editorFileWasModified(void) {
    return E.dirty;
}

void updateWindowSize(void) {
    if (getWindowSize(STDIN_FILENO,STDOUT_FILENO,
                      &E.screenrows,&E.termcols) == -1) {
        editorDie("Unable to query the screen for size (columns / rows)",
                  errno);
    }
    E.screenrows -= 2; /* Get room for status bar. */
    editorUpdateGeometry(); /* Reserve columns for the sidebar and gutter. */
}

void handleSigWinCh(int unused __attribute__((unused))) {
    updateWindowSize();
    if (E.cy >= E.screenrows) {
        E.rowoff += E.cy - (E.screenrows-1);
        E.cy = E.screenrows - 1;
    }
    if (E.ft_scroll > E.ft_vcount) E.ft_scroll = E.ft_vcount ? E.ft_vcount-1 : 0;
    if (E.coloff < 0) E.coloff = 0;
    editorRefreshScreen();
}

void initEditor(void) {
    E.cx = 0;
    E.cy = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.numrows = 0;
    E.rowcap = 0;
    E.row = NULL;
    E.dirty = 0;
    E.filename = NULL;
    E.syntax = NULL;
    E.sel_active = 0;
    E.click_count = 0;
    E.ft_open = 0;
    E.ft_width = 0;
    E.ln_width = 0;
    E.ft_focus = 0;
    E.ft_root = NULL;
    E.ft_vis = NULL;
    E.ft_vcount = 0;
    E.ft_vcap = 0;
    E.ft_sel = 0;
    E.ft_scroll = 0;
    updateWindowSize();
    signal(SIGWINCH, handleSigWinCh);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr,"Usage: quip <filename>\n");
        exit(1);
    }

    initEditor();
    editorLoadLanguages(argv[0]);
    editorSelectSyntaxHighlight(argv[1]);
    editorOpen(argv[1]);
    editorPushSnapshot();
    undo_saved = undo_cur;
    E.dirty = 0;
    enableRawMode(STDIN_FILENO);
    if (hl_load_errors > 1)
        editorSetStatusMessage("%s (+%d more)",hl_load_error,hl_load_errors-1);
    else
        editorSetStatusMessage("%s",hl_load_error);
    while(1) {
        editorRefreshScreen();
        editorProcessKeypress(STDIN_FILENO);
    }
    return 0;
}
