/* main.c — initialization and the main loop. */
#include "editor.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>

struct editorConfig E;

int main(int argc, char *argv[]) {
    const char *root = ".";
    struct stat st;
    int i;

    E.curidx = -1;
    E.show_tree = 1;
    E.focus = FOCUS_EDITOR;
    updateWindowSize(); /* before treeInit: scroll clamps need textrows */

    /* A directory argument roots the tree; file arguments are opened. */
    for (i = 1; i < argc; i++)
        if (stat(argv[i], &st) == 0 && S_ISDIR(st.st_mode)) root = argv[i];
    treeInit(root);
    for (i = 1; i < argc; i++) {
        if (stat(argv[i], &st) == 0 && S_ISDIR(st.st_mode)) continue;
        if (!bufListOpen(argv[i]))
            editorSetStatusMessage("cannot open %s: %s",
                                   argv[i], strerror(errno));
    }
    if (!E.nbufs) E.focus = FOCUS_TREE;

    enableRawMode();
    if (!E.statusmsg[0])
        editorSetStatusMessage("Ctrl-S save | Ctrl-F find | Ctrl-T tree | "
                               "Ctrl-N/P buffers | Ctrl-W close | "
                               "Ctrl-X/Q quit");
    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }
}
