/* The file tree sidebar (Ctrl-E). */
#include "quip.h"

/* A node in the file-tree sidebar. Directories load their children lazily the
 * first time they are expanded, so a huge project is never walked eagerly. */
struct ftnode {
    char *name; /* Basename shown in the sidebar. */
    char *path; /* Full path, used to open / descend. */
    int is_dir;
    int depth;             /* Indentation level (root = 0). */
    int expanded;          /* Directories only. */
    int loaded;            /* Children already read? */
    struct ftnode **child; /* Child node pointers. */
    int nchild;
};

#define FT_WIDTH    30 /* Sidebar width in columns (including the divider). */
#define FT_MIN_TEXT 20 /* Minimum text columns; refuse to open below this. */

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
        c->name = malloc(nlen + 1);
        memcpy(c->name, de->d_name, nlen + 1);
        c->path = malloc(plen + (rootslash ? 0 : 1) + nlen + 1);
        memcpy(c->path, dir->path, plen);
        size_t off = plen;
        if (!rootslash) c->path[off++] = '/';
        memcpy(c->path + off, de->d_name, nlen + 1);
        c->depth = dir->depth + 1;
        struct stat st;
        c->is_dir = (stat(c->path, &st) == 0 && S_ISDIR(st.st_mode));
        dir->child = growBuf(dir->child, &cap, (dir->nchild + 1) * (int)sizeof(ftnode *));
        dir->child[dir->nchild++] = c;
    }
    closedir(d);
    if (dir->nchild) qsort(dir->child, dir->nchild, sizeof(ftnode *), ftCmp);
}

/* Recursively free a node and its subtree. */
void ftFreeNode(ftnode *n) {
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
        dirpath = malloc(2);
        dirpath[0] = '.';
        dirpath[1] = '\0';
    } else if (slash == filepath) {
        dirpath = malloc(2);
        dirpath[0] = '/';
        dirpath[1] = '\0';
    } else {
        size_t n = slash - filepath;
        dirpath = malloc(n + 1);
        memcpy(dirpath, filepath, n);
        dirpath[n] = '\0';
    }
    ftnode *root = calloc(1, sizeof(ftnode));
    root->path = dirpath;
    const char *rs = strrchr(dirpath, '/');
    const char *base = (rs && rs[1]) ? rs + 1 : dirpath;
    /* A file opened by a bare name lives in ".": label it with the name of
     * the working directory rather than a lone dot. */
    char cwd[4096];
    if (!slash && getcwd(cwd, sizeof(cwd))) {
        char *cs = strrchr(cwd, '/');
        base = (cs && cs[1]) ? cs + 1 : cwd;
    }
    size_t blen = strlen(base);
    root->name = malloc(blen + 1);
    memcpy(root->name, base, blen + 1);
    root->is_dir = 1;
    root->depth = 0;
    root->expanded = 1;
    ftReadDir(root);
    E.ft_root = root;
}

/* Append one node pointer to the flattened visible list. */
static void ftAppendVis(ftnode *n) {
    E.ft_vis = growBuf(E.ft_vis, &E.ft_vcap, (E.ft_vcount + 1) * (int)sizeof(ftnode *));
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
    if (E.ft_sel >= E.ft_vcount) E.ft_sel = E.ft_vcount ? E.ft_vcount - 1 : 0;
    if (E.ft_sel < 0) E.ft_sel = 0;
}

/* Draw a blank sidebar cell (spaces + divider) of exactly ft_width columns. */
void ftBlankCell(struct abuf *ab) {
    char buf[FT_WIDTH];
    int i;
    for (i = 0; i < E.ft_width - 1; i++) buf[i] = ' ';
    buf[E.ft_width - 1] = '|';
    abAppend(ab, buf, E.ft_width);
}

/* Draw the sidebar cell for screen row 'y' into the append buffer. */
void ftDrawSidebarRow(struct abuf *ab, int y) {
    int W = E.ft_width - 1; /* Text columns; last column is the divider. */
    char line[FT_WIDTH];
    int n = 0;
    int idx = E.ft_scroll + y;
    int has = (idx >= 0 && idx < E.ft_vcount);
    ftnode *node = has ? E.ft_vis[idx] : NULL;

    if (has) {
        int indent = node->depth * 2;
        if (indent > W - 3) indent = W - 3;
        if (indent < 0) indent = 0;
        while (n < indent && n < W) line[n++] = ' ';
        if (n < W) line[n++] = node->is_dir ? (node->expanded ? '-' : '+') : ' ';
        if (n < W) line[n++] = ' ';
        const char *p = node->name;
        while (*p && n < W) line[n++] = *p++;
        if (*p && W > 0) line[W - 1] = '~'; /* name was truncated */
    }
    while (n < W) line[n++] = ' ';
    editorSanitize(line, W); /* File names may hold anything. */

    int selected = (has && idx == E.ft_sel);
    if (selected)
        abAppendLit(ab, VT_REVERSE);
    else if (has && node->is_dir)
        abAppendLit(ab, VT_BOLD_BLUE);
    abAppend(ab, line, W);
    if (selected || (has && node->is_dir)) abAppendLit(ab, VT_RESET);
    abAppendLit(ab, "|");
}

/* Move the selection by 'delta' rows, keeping it inside the visible pane. */
static void ftMoveSel(int delta) {
    if (!E.ft_vcount) return;
    E.ft_sel += delta;
    if (E.ft_sel < 0) E.ft_sel = 0;
    if (E.ft_sel >= E.ft_vcount) E.ft_sel = E.ft_vcount - 1;
    if (E.ft_sel < E.ft_scroll) E.ft_scroll = E.ft_sel;
    if (E.ft_sel >= E.ft_scroll + E.screenrows) E.ft_scroll = E.ft_sel - E.screenrows + 1;
}

/* Enter / right: expand-or-collapse a directory, or open a file. */
void ftActivate(void) {
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
    for (int i = E.ft_sel - 1; i >= 0; i--) {
        if (E.ft_vis[i]->depth < n->depth) {
            E.ft_sel = i;
            break;
        }
    }
    if (E.ft_sel < E.ft_scroll) E.ft_scroll = E.ft_sel;
}

/* Show or hide the sidebar, recomputing the text-area geometry. */
void ftToggle(void) {
    if (!E.ft_open) {
        if (E.termcols < FT_WIDTH + FT_MIN_TEXT) {
            editorSetStatusMessage("Terminal too narrow for the file tree");
            return;
        }
        if (!E.ft_root) ftBuildRoot(E.filename);
        E.ft_width = FT_WIDTH;
        E.ft_open = 1;
        E.ft_focus = 1;
        E.ft_scroll = 0;
        ftFlatten();
    } else {
        E.ft_open = 0;
        E.ft_focus = 0;
        E.ft_width = 0;
    }
    editorUpdateGeometry();
}

/* Handle a key while the sidebar has focus. Returns 1 if the key was consumed. */
int ftHandleKey(int c) {
    switch (c) {
    case ARROW_UP: ftMoveSel(-1); return 1;
    case ARROW_DOWN: ftMoveSel(1); return 1;
    case PAGE_UP: ftMoveSel(-E.screenrows); return 1;
    case PAGE_DOWN: ftMoveSel(E.screenrows); return 1;
    case SCROLL_UP: ftMoveSel(-3); return 1;
    case SCROLL_DOWN: ftMoveSel(3); return 1;
    case ENTER:
    case ARROW_RIGHT: ftActivate(); return 1;
    case ARROW_LEFT: ftCollapse(); return 1;
    case ESC: E.ft_focus = 0; return 1;
    default: return 0;
    }
}
