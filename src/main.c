/* Startup, the main loop, and opening a file from the tree. */
#include "quip.h"

struct editorConfig E;

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
            editorClearStatusMessage();
            return 1;
        }
    }

    /* Tear down the current document and its undo history. */
    editorFreeAllRows();
    editorUndoReset();

    E.cx = E.cy = E.rowoff = E.coloff = 0;
    E.sel_active = 0;

    char *p = strdup(path); /* editorOpen/SelectSyntax take char*. */
    editorSelectSyntaxHighlight(p);
    editorOpen(p);
    free(p);

    /* Seed the undo baseline, exactly as main() does at startup. */
    editorPushSnapshot();
    editorUndoMarkSaved();
    E.dirty = 0;

    E.ft_focus = 0; /* Hand focus to the editor so the user can type. */
    editorClearStatusMessage();
    return 0;
}

static void initEditor(void) {
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
    /* sigaction, not signal: on Linux, signal() under -std=c99 resets the
     * handler after one use. No SA_RESTART, so a resize wakes the pending
     * read at once instead of after its 0.1s timeout. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handleSigWinCh;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGWINCH, &sa, NULL);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: quip <filename>\n");
        exit(1);
    }

    initEditor();
    editorLoadLanguages(argv[0]);
    editorSelectSyntaxHighlight(argv[1]);
    editorOpen(argv[1]);
    editorPushSnapshot();
    editorUndoMarkSaved();
    E.dirty = 0;
    enableRawMode(STDIN_FILENO);
    if (hl_load_errors > 1)
        editorSetStatusMessage("%s (+%d more)", hl_load_error, hl_load_errors - 1);
    else
        editorSetStatusMessage("%s", hl_load_error);
    while (1) {
        editorRefreshScreen();
        editorProcessKeypress(STDIN_FILENO);
    }
    return 0;
}
