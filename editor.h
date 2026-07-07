/*
 * ted — a tiny terminal code editor with a file tree pane.
 *
 * Architecture derived from kilo by Salvatore Sanfilippo (antirez),
 * BSD-2-Clause — https://github.com/antirez/kilo
 * No dependencies: POSIX + VT100 escape sequences only.
 */
#ifndef TED_H
#define TED_H

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif

#include <stddef.h>
#include <termios.h>
#include <time.h>

#define TED_VERSION "0.1.0"
#define TAB_WIDTH   4  /* render width of \t and soft-tab size */
#define TREE_WIDTH  28 /* columns of the file tree pane */
#define QUIT_TIMES  1  /* extra Ctrl-Q/Ctrl-W presses with unsaved changes */

#define CTRL_KEY(k) ((k) & 0x1f)
#define KEY_SHIFT 0x2000 /* OR'ed onto movement keys when Shift is held */

enum ekey {
    KEY_NULL = 0,
    BACKSPACE = 127,
    ARROW_LEFT = 1000, ARROW_RIGHT, ARROW_UP, ARROW_DOWN,
    CTRL_ARROW_LEFT, CTRL_ARROW_RIGHT, CTRL_ARROW_UP, CTRL_ARROW_DOWN,
    HOME_KEY, END_KEY, PAGE_UP, PAGE_DOWN, DEL_KEY, CTRL_DEL_KEY,
    KEY_MOUSE, KEY_RESIZE, KEY_PASTE_BEGIN, KEY_PASTE_END
};

enum ehl {
    HL_NORMAL = 0, HL_COMMENT, HL_MLCOMMENT,
    HL_KEYWORD1, HL_KEYWORD2, HL_STRING, HL_NUMBER, HL_MATCH
};
#define HL_NUMBERS (1 << 0)
#define HL_STRINGS (1 << 1)

enum efocus { FOCUS_EDITOR, FOCUS_TREE };

struct esyntax {
    const char *filetype;
    const char **filematch;
    const char **keywords;
    const char *scs;        /* single-line comment start */
    const char *mcs, *mce;  /* multi-line comment delimiters */
    int flags;
};

enum utype { UINS, UDEL, UROWI, UROWD, UTRUNC, UAPPEND };

struct urec {               /* one reverse (undo) operation */
    unsigned char type;
    unsigned char chained;  /* same group as the record pushed before it */
    int y, x, n;
    char *s;                /* owned: text to restore (UDEL/UROWD/UTRUNC) */
    int cx, cy;             /* cursor before the action */
};

struct erow {
    int idx;                /* index within the buffer */
    int size, rsize;        /* length of chars / render */
    char *chars;            /* raw line content */
    char *render;           /* tabs expanded, what is drawn */
    unsigned char *hl;      /* per-render-byte highlight class */
    int hl_open_comment;    /* row ends inside a multiline comment */
};

struct ebuf {               /* one open file */
    char *filename;
    struct erow *rows;
    int numrows;
    int cx, cy, rx;         /* cursor (chars coords) and render x */
    int rowoff, coloff;     /* scroll offsets */
    int dirty;
    struct esyntax *syntax;
    struct urec *ur;        /* undo stack */
    int nur, urcap;
    struct urec *rr;        /* redo stack (cleared by any fresh edit) */
    int nrr, rrcap;
    int saved_nur;          /* stack depth at last save; -1 if unreachable */
    int chain;              /* 0 off, 1 chain from next push, 2 chaining */
    int sel_active;         /* selection spans anchor..cursor */
    int sel_ay, sel_ax;     /* selection anchor (chars coords) */
};

struct tnode {              /* file tree node */
    char *name;
    struct tnode *parent;
    struct tnode **child;
    int nchild;
    unsigned char isdir, expanded, loaded;
};

struct vitem { struct tnode *node; int depth; }; /* flattened tree entry */

struct mev { int btn, x, y, press; };            /* decoded mouse event */

struct abuf { char *b; size_t len, cap; };       /* growable frame buffer */

struct editorConfig {
    int screenrows, screencols; /* whole terminal */
    int textrows;               /* rows available for text */
    struct termios orig_termios;
    int rawmode;
    struct ebuf **bufs;         /* open buffers */
    int nbufs, curidx;          /* curidx == -1: no buffer */
    struct tnode *root;         /* file tree */
    struct vitem *vis;          /* flattened visible tree entries */
    int nvis, viscap;
    int treesel, treeoff;       /* tree selection and scroll */
    int show_tree, show_hidden;
    int focus;                  /* FOCUS_EDITOR or FOCUS_TREE */
    char statusmsg[128];
    time_t statusmsg_time;
    char *clip;                 /* internal clipboard, shared by buffers */
    size_t cliplen;
    struct abuf ab;             /* reused per-frame output buffer */
};
extern struct editorConfig E;

/* terminal.c */
void die(const char *s);
void enableRawMode(void);
void disableRawMode(void);
int  getWindowSize(int *rows, int *cols);
int  editorReadKey(struct mev *m);
void termWrite(const char *s, size_t n);

/* buffer.c */
void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
int  editorRowCxToRx(struct erow *row, int cx);
int  editorRowRxToCx(struct erow *row, int rx);
void bufInsertChar(struct ebuf *b, int c);
void bufInsertNewline(struct ebuf *b, int autoindent);
void bufDelChar(struct ebuf *b);
void bufInsertText(struct ebuf *b, const char *s, size_t n);
void bufDeleteRange(struct ebuf *b, int sy, int sx, int ey, int ex);
char *bufGetRange(struct ebuf *b, int sy, int sx, int ey, int ex, size_t *n);
int  bufUndo(struct ebuf *b);
int  bufRedo(struct ebuf *b);
long bufWrite(struct ebuf *b);
void bufFree(struct ebuf *b);
struct ebuf *bufListOpen(const char *path);
void bufListClose(int idx);
struct ebuf *curBuf(void);

/* syntax.c */
void editorUpdateSyntax(struct ebuf *b, struct erow *row);
int  editorSyntaxToColor(int hl);
void editorSelectSyntaxHighlight(struct ebuf *b);

/* tree.c */
void treeInit(const char *root);
void treeNotifySaved(const char *path);
void treeFreeAll(void);
void treeFlatten(void);
void treeProcessKey(int c);
void treeClick(int y);
void treeScroll(int delta);

/* editor.c */
char *editorPrompt(const char *prompt, void (*callback)(char *, int));
void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen(void);
void editorProcessKeypress(void);
void updateWindowSize(void);
int  treeVisible(void);
int  editorCols(void);
void editorTeardown(void);

#endif
