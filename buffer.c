/* buffer.c — text buffers: rows, editing operations, file I/O, buffer list. */
#include "editor.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- checked allocation ------------------------------------------------ */

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) die("malloc");
    return p;
}

void *xrealloc(void *p, size_t n) {
    p = realloc(p, n ? n : 1);
    if (!p) die("realloc");
    return p;
}

char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

/* ---- rows -------------------------------------------------------------- */

int editorRowCxToRx(struct erow *row, int cx) {
    int rx = 0, j;
    for (j = 0; j < cx; j++) {
        if (row->chars[j] == '\t') rx += (TAB_WIDTH - 1) - (rx % TAB_WIDTH);
        rx++;
    }
    return rx;
}

int editorRowRxToCx(struct erow *row, int rx) {
    int cur = 0, cx;
    for (cx = 0; cx < row->size; cx++) {
        if (row->chars[cx] == '\t') cur += (TAB_WIDTH - 1) - (cur % TAB_WIDTH);
        cur++;
        if (cur > rx) return cx;
    }
    return cx;
}

static void editorUpdateRow(struct ebuf *b, struct erow *row) {
    int tabs = 0, j, at = 0;
    for (j = 0; j < row->size; j++)
        if (row->chars[j] == '\t') tabs++;
    free(row->render);
    row->render = xmalloc((size_t)row->size + (size_t)tabs * (TAB_WIDTH - 1) + 1);
    for (j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            row->render[at++] = ' ';
            while (at % TAB_WIDTH != 0) row->render[at++] = ' ';
        } else {
            row->render[at++] = row->chars[j];
        }
    }
    row->render[at] = '\0';
    row->rsize = at;
    editorUpdateSyntax(b, row);
}

static void bufInsertRow(struct ebuf *b, int at, const char *s, int len) {
    struct erow *row;
    int j;
    if (at < 0 || at > b->numrows) return;
    b->rows = xrealloc(b->rows, sizeof(struct erow) * ((size_t)b->numrows + 1));
    memmove(&b->rows[at + 1], &b->rows[at],
            sizeof(struct erow) * (size_t)(b->numrows - at));
    b->numrows++;
    row = &b->rows[at];
    row->size = len;
    row->chars = xmalloc((size_t)len + 1);
    memcpy(row->chars, s, (size_t)len);
    row->chars[len] = '\0';
    row->render = NULL;
    row->hl = NULL;
    row->rsize = 0;
    row->hl_open_comment = 0;
    for (j = at; j < b->numrows; j++) b->rows[j].idx = j;
    editorUpdateRow(b, row);
    b->dirty++;
}

static void editorFreeRow(struct erow *row) {
    free(row->chars);
    free(row->render);
    free(row->hl);
}

static void bufDelRow(struct ebuf *b, int at) {
    int j;
    if (at < 0 || at >= b->numrows) return;
    editorFreeRow(&b->rows[at]);
    memmove(&b->rows[at], &b->rows[at + 1],
            sizeof(struct erow) * (size_t)(b->numrows - at - 1));
    b->numrows--;
    for (j = at; j < b->numrows; j++) b->rows[j].idx = j;
    if (at < b->numrows) editorUpdateSyntax(b, &b->rows[at]);
    b->dirty++;
}

static void rowInsertChar(struct ebuf *b, struct erow *row, int at, int c) {
    if (at < 0 || at > row->size) at = row->size;
    row->chars = xrealloc(row->chars, (size_t)row->size + 2);
    memmove(&row->chars[at + 1], &row->chars[at], (size_t)(row->size - at) + 1);
    row->size++;
    row->chars[at] = (char)c;
    editorUpdateRow(b, row);
    b->dirty++;
}

static void rowAppendString(struct ebuf *b, struct erow *row,
                            const char *s, int len) {
    row->chars = xrealloc(row->chars, (size_t)row->size + (size_t)len + 1);
    memcpy(&row->chars[row->size], s, (size_t)len);
    row->size += len;
    row->chars[row->size] = '\0';
    editorUpdateRow(b, row);
    b->dirty++;
}

static void rowDelChar(struct ebuf *b, struct erow *row, int at) {
    if (at < 0 || at >= row->size) return;
    memmove(&row->chars[at], &row->chars[at + 1], (size_t)(row->size - at));
    row->size--;
    editorUpdateRow(b, row);
    b->dirty++;
}

static void rowInsSpan(struct ebuf *b, struct erow *row, int at,
                       const char *s, int n) {
    if (at < 0 || at > row->size || n <= 0) return;
    row->chars = xrealloc(row->chars, (size_t)row->size + (size_t)n + 1);
    memmove(&row->chars[at + n], &row->chars[at], (size_t)(row->size - at) + 1);
    memcpy(&row->chars[at], s, (size_t)n);
    row->size += n;
    editorUpdateRow(b, row);
    b->dirty++;
}

static void rowDelSpan(struct ebuf *b, struct erow *row, int at, int n) {
    if (at < 0 || n <= 0 || at + n > row->size) return;
    memmove(&row->chars[at], &row->chars[at + n],
            (size_t)(row->size - at - n) + 1);
    row->size -= n;
    editorUpdateRow(b, row);
    b->dirty++;
}

static void rowTruncate(struct ebuf *b, struct erow *row, int len) {
    if (len < 0 || len > row->size) return;
    row->size = len;
    row->chars[len] = '\0';
    editorUpdateRow(b, row);
    b->dirty++;
}

/* ---- undo ----------------------------------------------------------------
 * Editing ops push reverse operations; consecutive typing and backspacing
 * coalesce into runs, compound ops (split/join/paste) chain into one group.
 */

#define UNDO_MAX 1024   /* records kept per buffer */
#define URUN_MAX 32     /* max chars per coalesced typing/deleting run */

static char *xmemdup(const char *s, int n) {
    char *p = xmalloc((size_t)n + 1);
    memcpy(p, s, (size_t)n);
    p[n] = '\0';
    return p;
}

static struct urec *undoTop(struct ebuf *b) {
    return b->nur ? &b->ur[b->nur - 1] : NULL;
}

/* A fresh edit: history diverges — drop redo, invalidate a bypassed save. */
static void undoEdited(struct ebuf *b) {
    int i;
    if (b->saved_nur > b->nur) b->saved_nur = -1;
    for (i = 0; i < b->nrr; i++) free(b->rr[i].s);
    b->nrr = 0;
}

/* Push a reverse op; s (may be NULL) is owned by the record from now on. */
static void undoPush(struct ebuf *b, int type, int y, int x, int n,
                     char *s, int chained) {
    struct urec *r;
    undoEdited(b);
    if (b->chain == 2) chained = 1;
    else if (b->chain == 1) b->chain = 2;
    if (b->nur == b->urcap) {
        if (b->urcap < UNDO_MAX) {
            b->urcap = b->urcap ? b->urcap * 2 : 64;
            b->ur = xrealloc(b->ur, sizeof(*b->ur) * (size_t)b->urcap);
        } else { /* full: drop the oldest group */
            int i, k = 1;
            while (k < b->nur && b->ur[k].chained) k++;
            for (i = 0; i < k; i++) free(b->ur[i].s);
            memmove(b->ur, &b->ur[k], sizeof(*b->ur) * (size_t)(b->nur - k));
            b->nur -= k;
            b->saved_nur = (b->saved_nur >= k) ? b->saved_nur - k : -1;
        }
    }
    r = &b->ur[b->nur++];
    r->type = (unsigned char)type;
    r->chained = (unsigned char)chained;
    r->y = y; r->x = x; r->n = n; r->s = s;
    r->cx = b->cx; r->cy = b->cy;
}

/* Apply record r to the buffer and return the record that reverses it.
 * The types pair up: UINS<->UDEL, UROWI<->UROWD, UTRUNC<->UAPPEND. */
static struct urec applyRec(struct ebuf *b, struct urec *r) {
    struct urec inv;
    struct erow *row;
    inv.y = r->y;
    inv.x = r->x;
    inv.n = r->n;
    inv.s = NULL;
    inv.chained = 0;
    inv.cx = b->cx;
    inv.cy = b->cy;
    switch (r->type) {
    case UINS: /* n chars were inserted at (y,x): delete them */
        row = &b->rows[r->y];
        inv.type = UDEL;
        inv.s = xmemdup(row->chars + r->x, r->n);
        rowDelSpan(b, row, r->x, r->n);
        break;
    case UDEL: /* text was deleted at (y,x): put it back */
        inv.type = UINS;
        rowInsSpan(b, &b->rows[r->y], r->x, r->s, r->n);
        break;
    case UROWI:
        row = &b->rows[r->y];
        inv.type = UROWD;
        inv.n = row->size;
        inv.s = xmemdup(row->chars, row->size);
        bufDelRow(b, r->y);
        break;
    case UROWD:
        inv.type = UROWI;
        bufInsertRow(b, r->y, r->s, r->n);
        break;
    case UTRUNC:
        inv.type = UAPPEND;
        rowAppendString(b, &b->rows[r->y], r->s, r->n);
        break;
    default: /* UAPPEND */
        row = &b->rows[r->y];
        inv.type = UTRUNC;
        inv.n = row->size - r->x;
        inv.s = xmemdup(row->chars + r->x, row->size - r->x);
        rowTruncate(b, row, r->x);
        break;
    }
    return inv;
}

/* Pop one whole group from (from,fn), applying each record and pushing the
 * reverses onto (to,tn,tcap); the receiving stack pops them back in the
 * original application order. Returns 0 if the source stack is empty. */
static int transferGroup(struct ebuf *b, struct urec *from, int *fn,
                         struct urec **to, int *tn, int *tcap) {
    int chained, first = 1;
    if (*fn == 0) return 0;
    do {
        struct urec *r = &from[--(*fn)];
        struct urec inv = applyRec(b, r);
        inv.chained = (unsigned char)!first;
        first = 0;
        chained = r->chained;
        b->cx = r->cx;
        b->cy = r->cy;
        free(r->s);
        if (*tn == *tcap) {
            *tcap = *tcap ? *tcap * 2 : 64;
            *to = xrealloc(*to, sizeof(**to) * (size_t)*tcap);
        }
        (*to)[(*tn)++] = inv;
    } while (chained && *fn > 0);
    b->dirty = (b->nur != b->saved_nur);
    return 1;
}

int bufUndo(struct ebuf *b) {
    return transferGroup(b, b->ur, &b->nur, &b->rr, &b->nrr, &b->rrcap);
}

int bufRedo(struct ebuf *b) {
    return transferGroup(b, b->rr, &b->nrr, &b->ur, &b->nur, &b->urcap);
}

/* ---- editing at the cursor ---------------------------------------------- */

void bufInsertChar(struct ebuf *b, int c) {
    struct urec *top = undoTop(b);
    if (b->cy == b->numrows) {
        undoPush(b, UROWI, b->numrows, 0, 0, NULL, 0);
        bufInsertRow(b, b->numrows, "", 0);
        undoPush(b, UINS, b->cy, b->cx, 1, NULL, 1);
    } else if (top && top->type == UINS && top->y == b->cy &&
               top->x + top->n == b->cx && top->n < URUN_MAX &&
               b->nur != b->saved_nur) { /* never extend across a save */
        undoEdited(b);
        top->n++; /* coalesce a typing run */
    } else {
        undoPush(b, UINS, b->cy, b->cx, 1, NULL, 0);
    }
    rowInsertChar(b, &b->rows[b->cy], b->cx, c);
    b->cx++;
}

void bufInsertNewline(struct ebuf *b, int autoindent) {
    if (b->cx == 0) {
        undoPush(b, UROWI, b->cy, 0, 0, NULL, 0);
        bufInsertRow(b, b->cy, "", 0);
    } else {
        struct erow *row = &b->rows[b->cy];
        int ind = 0, tail = row->size - b->cx;
        char *s;
        while (autoindent && ind < b->cx &&
               (row->chars[ind] == ' ' || row->chars[ind] == '\t'))
            ind++; /* auto-indent: new line inherits leading whitespace */
        undoPush(b, UTRUNC, b->cy, b->cx, tail,
                 xmemdup(row->chars + b->cx, tail), 0);
        undoPush(b, UROWI, b->cy + 1, 0, 0, NULL, 1);
        s = xmalloc((size_t)ind + (size_t)tail);
        memcpy(s, row->chars, (size_t)ind);
        memcpy(s + ind, row->chars + b->cx, (size_t)tail);
        bufInsertRow(b, b->cy + 1, s, ind + tail);
        free(s);
        row = &b->rows[b->cy]; /* rows may have been reallocated */
        row->size = b->cx;
        row->chars[row->size] = '\0';
        editorUpdateRow(b, row);
        b->cx = ind;
    }
    b->cy++;
}

void bufDelChar(struct ebuf *b) {
    struct erow *row;
    if (b->cy == b->numrows) return;
    if (b->cx == 0 && b->cy == 0) return;
    row = &b->rows[b->cy];
    if (b->cx > 0) {
        struct urec *top = undoTop(b);
        int d = b->cx - 1;
        char c = row->chars[d];
        if (top && top->type == UDEL && top->y == b->cy &&
            top->n < URUN_MAX && (d == top->x - 1 || d == top->x) &&
            b->nur != b->saved_nur) { /* never extend across a save */
            undoEdited(b);
            top->s = xrealloc(top->s, (size_t)top->n + 2);
            if (d == top->x - 1) { /* backspace run: prepend */
                memmove(top->s + 1, top->s, (size_t)top->n + 1);
                top->s[0] = c;
                top->x = d;
            } else {               /* forward-delete run: append */
                top->s[top->n] = c;
                top->s[top->n + 1] = '\0';
            }
            top->n++;
        } else {
            undoPush(b, UDEL, b->cy, d, 1, xmemdup(&c, 1), 0);
        }
        rowDelChar(b, row, d);
        b->cx--;
    } else {
        int prevlen = b->rows[b->cy - 1].size;
        undoPush(b, UAPPEND, b->cy - 1, prevlen, row->size, NULL, 0);
        undoPush(b, UROWD, b->cy, 0, row->size,
                 xmemdup(row->chars, row->size), 1);
        b->cx = prevlen;
        rowAppendString(b, &b->rows[b->cy - 1], row->chars, row->size);
        bufDelRow(b, b->cy);
        b->cy--;
    }
}

/* Insert a block of text at the cursor. Callers wanting the insertion (and
 * a preceding selection delete) as one undo group set b->chain around it. */
void bufInsertText(struct ebuf *b, const char *s, size_t n) {
    size_t i = 0, j;
    int span;
    while (i < n) {
        j = i;
        while (j < n && s[j] != '\n') j++;
        span = (int)(j - i);
        if (span > 0) {
            if (b->cy == b->numrows) {
                undoPush(b, UROWI, b->numrows, 0, 0, NULL, 0);
                bufInsertRow(b, b->numrows, "", 0);
            }
            undoPush(b, UINS, b->cy, b->cx, span, NULL, 0);
            rowInsSpan(b, &b->rows[b->cy], b->cx, s + i, span);
            b->cx += span;
        }
        if (j < n) bufInsertNewline(b, 0); /* the '\n' itself */
        i = j + 1;
    }
}

/* Delete [sy,sx)..(ey,ex) (normalized, valid coords) as one undo group. */
void bufDeleteRange(struct ebuf *b, int sy, int sx, int ey, int ex) {
    struct erow *row;
    int i, nmid;
    if (sy == ey) {
        if (ex <= sx) return;
        row = &b->rows[sy];
        undoPush(b, UDEL, sy, sx, ex - sx, xmemdup(row->chars + sx, ex - sx), 0);
        rowDelSpan(b, row, sx, ex - sx);
    } else {
        row = &b->rows[sy];
        undoPush(b, UTRUNC, sy, sx, row->size - sx,
                 xmemdup(row->chars + sx, row->size - sx), 0);
        rowTruncate(b, row, sx);
        nmid = ey - sy - 1;
        for (i = 0; i < nmid; i++) { /* whole rows between the endpoints */
            row = &b->rows[sy + 1];
            undoPush(b, UROWD, sy + 1, 0, row->size,
                     xmemdup(row->chars, row->size), 1);
            bufDelRow(b, sy + 1);
        }
        row = &b->rows[sy + 1];
        if (ex > 0) { /* the selected prefix of the last row */
            undoPush(b, UDEL, sy + 1, 0, ex, xmemdup(row->chars, ex), 1);
            rowDelSpan(b, row, 0, ex);
        }
        undoPush(b, UAPPEND, sy, sx, row->size, NULL, 1);
        undoPush(b, UROWD, sy + 1, 0, row->size,
                 xmemdup(row->chars, row->size), 1);
        rowAppendString(b, &b->rows[sy], row->chars, row->size);
        bufDelRow(b, sy + 1);
    }
    b->cx = sx;
    b->cy = sy;
}

/* The text in [sy,sx)..(ey,ex), '\n'-joined; caller frees. */
char *bufGetRange(struct ebuf *b, int sy, int sx, int ey, int ex, size_t *n) {
    size_t len;
    char *s, *p;
    int y;
    if (sy == ey) {
        len = (size_t)(ex - sx);
        s = xmalloc(len + 1);
        memcpy(s, b->rows[sy].chars + sx, len);
    } else {
        len = (size_t)(b->rows[sy].size - sx) + 1 + (size_t)ex;
        for (y = sy + 1; y < ey; y++) len += (size_t)b->rows[y].size + 1;
        s = p = xmalloc(len + 1);
        memcpy(p, b->rows[sy].chars + sx, (size_t)(b->rows[sy].size - sx));
        p += b->rows[sy].size - sx;
        *p++ = '\n';
        for (y = sy + 1; y < ey; y++) {
            memcpy(p, b->rows[y].chars, (size_t)b->rows[y].size);
            p += b->rows[y].size;
            *p++ = '\n';
        }
        memcpy(p, b->rows[ey].chars, (size_t)ex);
    }
    s[len] = '\0';
    *n = len;
    return s;
}

/* ---- file I/O ----------------------------------------------------------- */

/* Load path into a fresh buffer. A missing file is an empty new buffer;
 * any other error returns NULL with errno set. */
static struct ebuf *bufOpen(const char *path) {
    struct ebuf *b;
    struct stat st;
    FILE *fp;
    char *line = NULL;
    size_t linecap = 0;
    ssize_t len;

    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        errno = EISDIR;
        return NULL;
    }
    fp = fopen(path, "r");
    if (!fp && errno != ENOENT) return NULL;

    b = xmalloc(sizeof(*b));
    memset(b, 0, sizeof(*b));
    b->filename = xstrdup(path);
    if (fp) {
        while ((len = getline(&line, &linecap, fp)) != -1) {
            while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
                len--;
            bufInsertRow(b, b->numrows, line, (int)len);
        }
        free(line);
        fclose(fp);
    }
    editorSelectSyntaxHighlight(b);
    b->wrap = (b->syntax == NULL); /* soft-wrap prose, scroll code */
    b->dirty = 0;
    return b;
}

static int writeAll(int fd, const char *buf, size_t len) {
    while (len) {
        ssize_t w = write(fd, buf, len);
        if (w == -1) {
            if (errno == EINTR) continue;
            return -1;
        }
        buf += w;
        len -= (size_t)w;
    }
    return 0;
}

/* Write the buffer atomically: temp file in the same directory, fsync,
 * rename over the target (symlinks are resolved first so they stay intact).
 * Returns bytes written or -1 with errno set. */
long bufWrite(struct ebuf *b) {
    size_t total = 0, dirlen;
    char *buf, *p, *target, *tmp;
    const char *slash;
    struct stat st;
    mode_t mode, um;
    int j, fd, ok, saved;

    for (j = 0; j < b->numrows; j++) total += (size_t)b->rows[j].size + 1;
    buf = xmalloc(total);
    p = buf;
    for (j = 0; j < b->numrows; j++) {
        memcpy(p, b->rows[j].chars, (size_t)b->rows[j].size);
        p += b->rows[j].size;
        *p++ = '\n';
    }
    target = realpath(b->filename, NULL);
    if (!target) {
        if (errno != ENOENT) { free(buf); return -1; }
        target = xstrdup(b->filename); /* new file */
    }
    um = umask(0);
    umask(um);
    mode = (stat(target, &st) == 0) ? (st.st_mode & 07777)
                                    : (mode_t)(0666 & ~um);
    slash = strrchr(target, '/');
    dirlen = slash ? (size_t)(slash - target) + 1 : 0;
    tmp = xmalloc(dirlen + sizeof(".ted.XXXXXX"));
    memcpy(tmp, target, dirlen);
    memcpy(tmp + dirlen, ".ted.XXXXXX", sizeof(".ted.XXXXXX"));
    fd = mkstemp(tmp);
    if (fd == -1) {
        saved = errno;
        free(buf); free(target); free(tmp);
        errno = saved;
        return -1;
    }
    (void)fchmod(fd, mode); /* best effort: some filesystems refuse */
    ok = writeAll(fd, buf, total) == 0 && fsync(fd) == 0;
    if (close(fd) == -1) ok = 0;
    if (ok && rename(tmp, target) == -1) ok = 0;
    saved = errno;
    if (!ok) unlink(tmp);
    free(buf); free(target); free(tmp);
    if (!ok) {
        errno = saved;
        return -1;
    }
    b->dirty = 0;
    b->saved_nur = b->nur; /* undoing back to here means "unmodified" */
    return (long)total;
}

/* ---- buffer list --------------------------------------------------------- */

struct ebuf *curBuf(void) {
    return E.curidx >= 0 ? E.bufs[E.curidx] : NULL;
}

void bufFree(struct ebuf *b) {
    int j;
    for (j = 0; j < b->numrows; j++) editorFreeRow(&b->rows[j]);
    for (j = 0; j < b->nur; j++) free(b->ur[j].s);
    for (j = 0; j < b->nrr; j++) free(b->rr[j].s);
    free(b->ur);
    free(b->rr);
    free(b->rows);
    free(b->filename);
    free(b);
}

/* Open path (deduplicated) and make it current; NULL on failure. */
struct ebuf *bufListOpen(const char *path) {
    struct ebuf *b;
    int i;
    while (path[0] == '.' && path[1] == '/') path += 2;
    for (i = 0; i < E.nbufs; i++) {
        if (E.bufs[i]->filename && strcmp(E.bufs[i]->filename, path) == 0) {
            E.curidx = i;
            return E.bufs[i];
        }
    }
    b = bufOpen(path);
    if (!b) return NULL;
    E.bufs = xrealloc(E.bufs, sizeof(*E.bufs) * ((size_t)E.nbufs + 1));
    E.bufs[E.nbufs] = b;
    E.curidx = E.nbufs++;
    return b;
}

void bufListClose(int idx) {
    if (idx < 0 || idx >= E.nbufs) return;
    bufFree(E.bufs[idx]);
    memmove(&E.bufs[idx], &E.bufs[idx + 1],
            sizeof(*E.bufs) * (size_t)(E.nbufs - idx - 1));
    E.nbufs--;
    if (E.curidx > idx) E.curidx--;
    if (E.curidx >= E.nbufs) E.curidx = E.nbufs - 1; /* -1 when list empty */
}
