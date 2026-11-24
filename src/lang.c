/* Language definitions: the .lang file loader and the built-in C. */
#include "quip.h"

/* Language files
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
static char *C_HL_extensions[] = {".c", ".h", ".cpp", ".hpp", ".cc", NULL};
static char *C_HL_filenames[] = {NULL};
// clang-format off
static char *C_HL_keywords[] = {
    /* C Keywords */
    "auto", "break", "case", "continue", "default", "do", "else", "enum",
    "extern", "for", "goto", "if", "register", "return", "sizeof", "static",
    "struct", "switch", "typedef", "union", "volatile", "while", "NULL",

    /* C++ Keywords */
    "alignas", "alignof", "and", "and_eq", "asm", "bitand", "bitor", "class",
    "compl", "constexpr", "const_cast", "decltype", "delete", "dynamic_cast",
    "explicit", "export", "false", "friend", "inline", "mutable", "namespace",
    "new", "noexcept", "not", "not_eq", "nullptr", "operator", "or", "or_eq",
    "private", "protected", "public", "reinterpret_cast", "static_assert",
    "static_cast", "template", "this", "thread_local", "throw", "true", "try",
    "typeid", "typename", "virtual", "xor", "xor_eq",

    /* C types */
    "int|", "long|", "double|", "float|", "char|", "unsigned|", "signed|",
    "void|", "short|", "auto|", "const|", "bool|", NULL
};
// clang-format on

static struct editorSyntax C_HL_builtin = {
    "c",       C_HL_extensions, C_HL_filenames, C_HL_keywords,
    {"//", 2}, {"/*", 2},       {"*/", 2},      HL_HIGHLIGHT_STRINGS | HL_HIGHLIGHT_NUMBERS,
};

/* Every known language, in match priority order (see above). */
struct editorSyntax **HLDB = NULL;
int HLDB_ENTRIES = 0;

/* Names accepted on a language file's "features" line. */
static const struct {
    const char *name;
    int flag;
} HL_FEATURES[] = {
    {"strings", HL_HIGHLIGHT_STRINGS},
    {"numbers", HL_HIGHLIGHT_NUMBERS},
    {"nested_comments", HL_NESTED_COMMENTS},
    {"multiline_strings", HL_MULTILINE_STRINGS},
    {"quoted_strings", HL_QUOTED_STRINGS},
    {"prime_quotes", HL_PRIME_QUOTES},
    {"word_start_comments", HL_WORD_START_COMMENTS},
    {"triple_quotes", HL_TRIPLE_QUOTES},
};

/* Problems found while loading language files, shown on the status line
 * once the editor is up. Only the first is spelled out. */
char hl_load_error[80];
int hl_load_errors = 0;

__attribute__((format(printf, 3, 4))) static void hlLoadError(const char *file, int line,
                                                              const char *fmt, ...) {
    if (hl_load_errors++) return;
    int len = line ? snprintf(hl_load_error, sizeof(hl_load_error), "%s:%d: ", file, line)
                   : snprintf(hl_load_error, sizeof(hl_load_error), "%s: ", file);
    if (len < 0 || len >= (int)sizeof(hl_load_error)) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(hl_load_error + len, sizeof(hl_load_error) - len, fmt, ap);
    va_end(ap);
}

/* Append 's' to the NULL terminated, malloc'd list '*list'. */
static void hlListAdd(char ***list, const char *s) {
    int n = 0;
    while (*list && (*list)[n]) n++;
    *list = realloc(*list, sizeof(char *) * (n + 2));
    (*list)[n] = strdup(s);
    (*list)[n + 1] = NULL;
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
        if (n == max) return max + 1;
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
    FILE *fp = fopen(path, "r");
    if (!fp) {
        hlLoadError(file, 0, "%s", strerror(errno));
        return NULL;
    }

    struct editorSyntax *s = calloc(1, sizeof(*s));
    char *line = NULL;
    size_t linecap = 0;
    int lineno = 0, have_version = 0, ok = 1;
    char key[16] = ""; /* The key an indented line continues, if any. */
    unsigned seen = 0; /* Bit per key in 'keys', to catch repeats. */
    static const char *keys[] = {"extensions", "filenames", "line_comment", "block_comment",
                                 "features",   "keywords",  "types"};
    enum { K_EXT, K_FILES, K_LINE, K_BLOCK, K_FEAT, K_KW, K_TYPES, K_N };

    while (ok && getline(&line, &linecap, fp) != -1) {
        lineno++;
        char *tok[64];
        int cont = isspace((unsigned char)line[0]);
        char *first = line;
        while (*first && isspace((unsigned char)*first)) first++;
        if (*first == '\0' || *first == '#') continue;

        int ntok = hlTokenize(line, tok, 64);
        if (ntok > 64) {
            hlLoadError(file, lineno, "more than 64 words on a line");
            ok = 0;
            break;
        }

        if (!have_version) {
            if (cont || ntok != 2 || strcmp(tok[0], "quip-lang")) {
                hlLoadError(file, lineno, "must start with \"quip-lang 1\"");
                ok = 0;
            } else if (strcmp(tok[1], "1")) {
                hlLoadError(file, lineno, "unsupported version %.10s", tok[1]);
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
            snprintf(key, sizeof(key), "%s", tok[0]);
            args++;
            nargs--;
        } else if (!key[0]) {
            hlLoadError(file, lineno, "indented line continues no key");
            ok = 0;
            break;
        }

        int k;
        for (k = 0; k < K_N; k++)
            if (!strcmp(key, keys[k])) break;
        if (k == K_N) {
            hlLoadError(file, lineno, "unknown key \"%.20s\"", key);
            ok = 0;
            break;
        }
        if (!cont) {
            if (seen & (1u << k)) {
                hlLoadError(file, lineno, "\"%s\" given twice", key);
                ok = 0;
                break;
            }
            seen |= 1u << k;
        }
        if (cont && (k == K_LINE || k == K_BLOCK)) {
            hlLoadError(file, lineno, "\"%s\" takes one line", key);
            ok = 0;
            break;
        }

        switch (k) {
        case K_EXT:
            for (int i = 0; i < nargs && ok; i++) {
                if (args[i][0] != '.' || args[i][1] == '\0') {
                    hlLoadError(file, lineno, "extension \"%.20s\" must start with '.'", args[i]);
                    ok = 0;
                } else {
                    hlListAdd(&s->extensions, args[i]);
                }
            }
            break;
        case K_FILES:
            for (int i = 0; i < nargs; i++) hlListAdd(&s->filenames, args[i]);
            break;
        case K_LINE:
            if (nargs != 1) {
                hlLoadError(file, lineno, "line_comment takes one delimiter");
                ok = 0;
            } else {
                hlSetDelim(&s->line_comment, args[0]);
            }
            break;
        case K_BLOCK:
            if (nargs != 2) {
                hlLoadError(file, lineno, "block_comment takes a start and an end");
                ok = 0;
            } else {
                hlSetDelim(&s->block_start, args[0]);
                hlSetDelim(&s->block_end, args[1]);
            }
            break;
        case K_FEAT:
            for (int i = 0; i < nargs && ok; i++) {
                unsigned f;
                for (f = 0; f < sizeof(HL_FEATURES) / sizeof(HL_FEATURES[0]); f++)
                    if (!strcmp(args[i], HL_FEATURES[f].name)) break;
                if (f == sizeof(HL_FEATURES) / sizeof(HL_FEATURES[0])) {
                    hlLoadError(file, lineno, "unknown feature \"%.20s\"", args[i]);
                    ok = 0;
                } else {
                    s->flags |= HL_FEATURES[f].flag;
                }
            }
            break;
        case K_KW:
            for (int i = 0; i < nargs; i++) hlListAdd(&s->keywords, args[i]);
            break;
        case K_TYPES:
            for (int i = 0; i < nargs; i++) {
                size_t len = strlen(args[i]);
                char *type = malloc(len + 2);
                memcpy(type, args[i], len);
                memcpy(type + len, "|", 2);
                hlListAdd(&s->keywords, type);
                free(type);
            }
            break;
        }
    }
    free(line);
    fclose(fp);

    if (ok && !have_version) {
        hlLoadError(file, 0, "empty, must start with \"quip-lang 1\"");
        ok = 0;
    }
    if (ok && !s->extensions && !s->filenames) {
        hlLoadError(file, 0, "needs \"extensions\" or \"filenames\"");
        ok = 0;
    }
    if (!ok) {
        hlFreeSyntax(s);
        return NULL;
    }

    /* Empty lists and delimiters rather than NULLs keep the users simple. */
    if (!s->extensions) s->extensions = calloc(1, sizeof(char *));
    if (!s->filenames) s->filenames = calloc(1, sizeof(char *));
    if (!s->keywords) s->keywords = calloc(1, sizeof(char *));
    if (!s->line_comment.s) hlSetDelim(&s->line_comment, "");
    if (!s->block_start.s) {
        hlSetDelim(&s->block_start, "");
        hlSetDelim(&s->block_end, "");
    }
    s->id = strndup(file, strlen(file) - 5); /* Drop ".lang". */
    return s;
}

/* Return malloc'd 'a' (its first 'alen' bytes, or all of it if alen is -1)
 * followed by 'b' and 'c'. Paths are built this way, at their exact length,
 * so that a long one is never cut short into some other path. */
static char *hlPath(const char *a, int alen, const char *b, const char *c) {
    size_t la = alen < 0 ? strlen(a) : (size_t)alen;
    size_t lb = strlen(b), lc = strlen(c);
    char *p = malloc(la + lb + lc + 1);
    memcpy(p, a, la);
    memcpy(p + la, b, lb);
    memcpy(p + la + lb, c, lc + 1);
    return p;
}

static int hlKnown(const char *id) {
    for (int i = 0; i < HLDB_ENTRIES; i++)
        if (!strcmp(HLDB[i]->id, id)) return 1;
    return 0;
}

static void hlAdd(struct editorSyntax *s) {
    HLDB = realloc(HLDB, sizeof(*HLDB) * (HLDB_ENTRIES + 1));
    HLDB[HLDB_ENTRIES++] = s;
}

static int hlCmpNames(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Load every *.lang file in 'dir' whose name was not loaded already, in
 * name order so that extension clashes resolve the same way every time. */
static void hlLoadDir(const char *dir) {
    /* The same directory can be named twice (QUIP_LANGDIR and an XDG dir);
     * scan it once so its problems are not reported twice. */
    static struct {
        dev_t dev;
        ino_t ino;
    } scanned[16];
    static int nscanned = 0;
    struct stat st;
    if (stat(dir, &st) == -1 || !S_ISDIR(st.st_mode)) return; /* Missing directories are normal. */
    for (int i = 0; i < nscanned; i++)
        if (scanned[i].dev == st.st_dev && scanned[i].ino == st.st_ino) return;
    if (nscanned < (int)(sizeof(scanned) / sizeof(scanned[0]))) {
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
        if (len > 5 && !strcmp(de->d_name + len - 5, ".lang")) {
            names = realloc(names, sizeof(char *) * (n + 1));
            names[n++] = strdup(de->d_name);
        }
    }
    closedir(d);
    if (n) qsort(names, n, sizeof(char *), hlCmpNames);

    for (int i = 0; i < n; i++) {
        char *id = strndup(names[i], strlen(names[i]) - 5); /* Drop ".lang". */
        if (!hlKnown(id)) {
            char *path = hlPath(dir, -1, "/", names[i]);
            struct editorSyntax *s = hlLoadFile(path, names[i]);
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
    char *dir = hlPath(base, len, sub, "");
    hlLoadDir(dir);
    free(dir);
}

/* Return the malloc'd directory holding the running binary, found from
 * argv[0] as the shell did: a name with a slash is a path, a bare name was
 * looked up in $PATH. Returns NULL if it cannot be found. */
static char *hlExeDir(const char *argv0) {
    const char *slash = strrchr(argv0, '/');
    if (slash == argv0) return strdup("/"); /* "/quip" */
    if (slash) return strndup(argv0, slash - argv0);
    const char *path = getenv("PATH");
    while (path && *path) {
        int len = strcspn(path, ":");
        /* An empty entry means the current directory. */
        char *dir = len ? strndup(path, len) : strdup(".");
        char *exe = hlPath(dir, -1, "/", argv0);
        int found = access(exe, X_OK) == 0;
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
        hlLoadSubdir(env, -1, "/quip/langs");
    else if (home)
        hlLoadSubdir(home, -1, "/.config/quip/langs");

    if ((env = getenv("QUIP_LANGDIR")) && env[0]) hlLoadDir(env);

    char *exedir = hlExeDir(argv0);
    if (exedir) {
        hlLoadSubdir(exedir, -1, "/langs");
        hlLoadSubdir(exedir, -1, "/../share/quip/langs");
        free(exedir);
    }

    if ((env = getenv("XDG_DATA_HOME")) && env[0])
        hlLoadSubdir(env, -1, "/quip/langs");
    else if (home)
        hlLoadSubdir(home, -1, "/.local/share/quip/langs");

    env = getenv("XDG_DATA_DIRS");
    if (!env || !env[0]) env = "/usr/local/share:/usr/share";
    while (*env) {
        int len = strcspn(env, ":");
        if (len) hlLoadSubdir(env, len, "/quip/langs");
        env += len;
        if (*env == ':') env++;
    }

    if (!hlKnown("c")) hlAdd(&C_HL_builtin);
}
