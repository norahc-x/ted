/* editor.c — rendering, input dispatch, search, prompts. */
#include "editor.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- frame buffer -------------------------------------------------------- */

static void abGrow(struct abuf *ab, size_t need) {
    if (ab->cap >= need) return;
    ab->cap = ab->cap ? ab->cap * 2 : 1024;
    if (ab->cap < need) ab->cap = need;
    ab->b = xrealloc(ab->b, ab->cap);
}

static void abAppend(struct abuf *ab, const char *s, size_t n) {
    abGrow(ab, ab->len + n);
    memcpy(ab->b + ab->len, s, n);
    ab->len += n;
}

static void abPrintf(struct abuf *ab, const char *fmt, ...) {
    va_list ap;
    char buf[256];
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof(buf)) n = sizeof(buf) - 1;
    abAppend(ab, buf, (size_t)n);
}

static void abFree(struct abuf *ab) {
    free(ab->b);
    ab->b = NULL;
    ab->len = ab->cap = 0;
}

/* ---- layout -------------------------------------------------------------- */

/* Tree pane width: fits the widest visible entry, capped at 40% of the
 * terminal so the editor always keeps room. */
static int treeWidth(void) {
    int w = E.treew ? E.treew : TREE_MIN;
    int maxw = (E.screencols * 2) / 5;
    if (w > maxw) w = maxw;
    if (w < TREE_MIN) w = TREE_MIN;
    return w;
}

int treeVisible(void) {
    return E.show_tree && E.screencols >= treeWidth() + 41;
}

int editorCols(void) {
    return treeVisible() ? E.screencols - treeWidth() - 1 : E.screencols;
}

static int gutterWidth(struct ebuf *b) {
    int n, d = 1;
    if (!b) return 0;
    for (n = b->numrows; n >= 10; n /= 10) d++;
    if (d < 3) d = 3;
    return d + 2; /* right-aligned number plus one space each side */
}

static int textCols(struct ebuf *b) {
    int tc = editorCols() - gutterWidth(b);
    return tc < 1 ? 1 : tc;
}

/* Screen lines this row occupies (1 unless wrapping). */
static int rowHeight(struct ebuf *b, struct erow *row) {
    int tc = textCols(b);
    if (!b->wrap || row->rsize == 0) return 1;
    return (row->rsize + tc - 1) / tc;
}

void updateWindowSize(void) {
    if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
    if (E.screenrows < 3) E.screenrows = 3;
    E.textrows = E.screenrows - 2;
}

void editorSetStatusMessage(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}

/* ---- selection ------------------------------------------------------------ */

static void selAnchor(struct ebuf *b) {
    if (!b->sel_active) {
        b->sel_active = 1;
        b->sel_ay = b->cy;
        b->sel_ax = b->cx;
    }
}

/* Normalized, clamped selection in char coords; 0 if none or empty. */
static int selGet(struct ebuf *b, int *sy, int *sx, int *ey, int *ex) {
    int ay = b->sel_ay, ax = b->sel_ax, cy = b->cy, cx = b->cx;
    if (!b->sel_active || b->numrows == 0) return 0;
    if (ay >= b->numrows) { ay = b->numrows - 1; ax = b->rows[ay].size; }
    if (cy >= b->numrows) { cy = b->numrows - 1; cx = b->rows[cy].size; }
    if (ax > b->rows[ay].size) ax = b->rows[ay].size;
    if (cx > b->rows[cy].size) cx = b->rows[cy].size;
    if (ay == cy && ax == cx) return 0;
    if (ay < cy || (ay == cy && ax < cx)) {
        *sy = ay; *sx = ax; *ey = cy; *ex = cx;
    } else {
        *sy = cy; *sx = cx; *ey = ay; *ex = ax;
    }
    return 1;
}

/* Delete the selection if there is one; returns 1 if it did. */
static int selDelete(struct ebuf *b) {
    int sy, sx, ey, ex;
    if (!selGet(b, &sy, &sx, &ey, &ex)) {
        b->sel_active = 0;
        return 0;
    }
    bufDeleteRange(b, sy, sx, ey, ex);
    b->sel_active = 0;
    return 1;
}

/* Chain guards: make delete-selection + insertion one undo group without
 * disturbing an already-open chain (e.g. bracketed paste). */
static int chainBegin(struct ebuf *b) {
    if (b->chain) return 1;
    b->chain = 1;
    return 0;
}

static void chainEnd(struct ebuf *b, int wasOpen) {
    if (!wasOpen) b->chain = 0;
}

/* Offer the copied text to the terminal's system clipboard (OSC 52). */
static void oscCopy(const char *s, size_t n) {
    static const char b64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *out;
    size_t i, o;
    unsigned v;
    if (n == 0 || n > 65536) return; /* many terminals cap OSC 52 payloads */
    out = xmalloc(7 + 4 * ((n + 2) / 3) + 2);
    memcpy(out, "\x1b]52;c;", 7);
    o = 7;
    for (i = 0; i + 3 <= n; i += 3) {
        v = ((unsigned)(unsigned char)s[i] << 16) |
            ((unsigned)(unsigned char)s[i + 1] << 8) |
            (unsigned char)s[i + 2];
        out[o++] = b64[(v >> 18) & 63];
        out[o++] = b64[(v >> 12) & 63];
        out[o++] = b64[(v >> 6) & 63];
        out[o++] = b64[v & 63];
    }
    if (i < n) {
        v = (unsigned)(unsigned char)s[i] << 16;
        if (i + 1 < n) v |= (unsigned)(unsigned char)s[i + 1] << 8;
        out[o++] = b64[(v >> 18) & 63];
        out[o++] = b64[(v >> 12) & 63];
        out[o++] = (i + 1 < n) ? b64[(v >> 6) & 63] : '=';
        out[o++] = '=';
    }
    out[o++] = '\a';
    termWrite(out, o);
    free(out);
}

/* Copy (cut != 0: and delete) the selection, or the whole current line. */
static void editorCopy(int cut) {
    struct ebuf *b = curBuf();
    int sy, sx, ey, ex;
    size_t n;
    if (!b) return;
    if (!selGet(b, &sy, &sx, &ey, &ex)) {
        if (b->cy >= b->numrows) return;
        sy = b->cy;
        sx = 0;
        if (sy < b->numrows - 1) { ey = sy + 1; ex = 0; }
        else { ey = sy; ex = b->rows[sy].size; }
        if (sy == ey && ex == sx) return; /* empty last line */
    }
    free(E.clip);
    E.clip = bufGetRange(b, sy, sx, ey, ex, &n);
    E.cliplen = n;
    oscCopy(E.clip, n);
    if (cut) {
        bufDeleteRange(b, sy, sx, ey, ex);
        b->sel_active = 0;
    }
    editorSetStatusMessage("%s %zu bytes", cut ? "cut" : "copied", n);
}

/* ---- drawing ------------------------------------------------------------- */

/* Wrap mode: visual lines between the top of the screen and the cursor. */
static int wrapDist(struct ebuf *b, int tc) {
    int dist = -b->segoff, r;
    for (r = b->rowoff; r < b->cy && r < b->numrows; r++)
        dist += rowHeight(b, &b->rows[r]);
    return dist + ((b->cy < b->numrows) ? b->rx / tc : 0);
}

static void editorScroll(void) {
    struct ebuf *b = curBuf();
    int tc, cseg, dist;
    if (!b) return;
    tc = textCols(b);
    b->rx = (b->cy < b->numrows)
                ? editorRowCxToRx(&b->rows[b->cy], b->cx) : 0;
    if (!b->wrap) {
        b->segoff = 0;
        if (b->cy < b->rowoff) b->rowoff = b->cy;
        if (b->cy >= b->rowoff + E.textrows)
            b->rowoff = b->cy - E.textrows + 1;
        if (b->rx < b->coloff) b->coloff = b->rx;
        if (b->rx >= b->coloff + tc) b->coloff = b->rx - tc + 1;
        return;
    }
    b->coloff = 0;
    if (b->rowoff > b->numrows) b->rowoff = b->numrows;
    if (b->rowoff == b->numrows ||
        b->segoff >= rowHeight(b, &b->rows[b->rowoff])) b->segoff = 0;
    cseg = (b->cy < b->numrows) ? b->rx / tc : 0;
    if (b->cy < b->rowoff || (b->cy == b->rowoff && cseg < b->segoff)) {
        b->rowoff = b->cy;
        b->segoff = cseg;
    }
    dist = wrapDist(b, tc);
    if (dist >= E.textrows) { /* advance the top dist-textrows+1 segments */
        int adv = dist - E.textrows + 1;
        while (adv > 0 && b->rowoff < b->numrows) {
            int rem = rowHeight(b, &b->rows[b->rowoff]) - b->segoff;
            if (rem > adv) {
                b->segoff += adv;
                adv = 0;
            } else {
                adv -= rem;
                b->rowoff++;
                b->segoff = 0;
            }
        }
    }
}

static void drawWelcome(struct abuf *ab, int y, int width) {
    static const char *lines[] = {
        "ted -- a tiny editor with a file tree",
        "version " TED_VERSION,
        "",
        "Ctrl-T focus tree | Enter open | Ctrl-Q quit",
    };
    int n = (int)(sizeof(lines) / sizeof(lines[0]));
    int start = E.textrows / 3, len, pad;
    if (y < start || y >= start + n) return;
    len = (int)strlen(lines[y - start]);
    if (len > width) len = width;
    pad = (width - len) / 2;
    if (pad > 0) abPrintf(ab, "%*s", pad, "");
    abAppend(ab, lines[y - start], (size_t)len);
}

static void drawTreeEntry(struct abuf *ab, int ti) {
    struct vitem *v = &E.vis[ti];
    struct tnode *n = v->node;
    int width = treeWidth(), cols = 0, sel = (ti == E.treesel);
    int indent = v->depth * 2, nlen, room;
    const char *mark;

    if (indent > width - 6) indent = width - 6;
    if (sel) abAppend(ab, "\x1b[7m", 4);
    if (n->isdir) abAppend(ab, "\x1b[36m", 5);
    abPrintf(ab, "%*s", indent + 1, "");
    cols += indent + 1;
    mark = n->isdir ? (n->expanded ? "\xe2\x96\xbe " : "\xe2\x96\xb8 ") : "  ";
    abAppend(ab, mark, strlen(mark)); /* ▾ / ▸: 3 bytes, 1 column */
    cols += 2;
    nlen = (int)strlen(n->name);
    room = width - cols;
    if (nlen > room) { /* clipped: make the cutoff visible */
        if (room > 1) abAppend(ab, n->name, (size_t)room - 1);
        if (room > 0) abAppend(ab, "\xe2\x80\xa6", 3); /* … */
        cols = width;
    } else if (nlen > 0) {
        abAppend(ab, n->name, (size_t)nlen);
        cols += nlen;
    }
    if (sel)
        while (cols++ < width) abAppend(ab, " ", 1);
    abAppend(ab, "\x1b[m", 3);
}

/* Render [start, start+len) of a row with syntax colors and selection. */
static void drawRowSlice(struct abuf *ab, struct erow *row, int start,
                         int len, int selx, int sele) {
    char *c = &row->render[start];
    unsigned char *hl = &row->hl[start];
    int j, cur = -1, insel = 0, now;
    for (j = 0; j < len; j++) {
        now = (start + j >= selx && start + j < sele);
        if (now != insel) {
            abAppend(ab, now ? "\x1b[7m" : "\x1b[27m", now ? 4 : 5);
            insel = now;
        }
        if (iscntrl((unsigned char)c[j])) {
            char sym = (c[j] <= 26) ? '@' + c[j] : '?';
            abAppend(ab, "\x1b[7m", 4);
            abAppend(ab, &sym, 1);
            abAppend(ab, "\x1b[m", 3);
            if (cur != -1) abPrintf(ab, "\x1b[%dm", cur);
            if (insel) abAppend(ab, "\x1b[7m", 4);
        } else {
            int color = editorSyntaxToColor(hl[j]);
            if (color != cur) {
                cur = color;
                abPrintf(ab, "\x1b[%dm", color);
            }
            abAppend(ab, &c[j], 1);
        }
    }
    if (insel) abAppend(ab, "\x1b[27m", 5);
    abAppend(ab, "\x1b[39m", 5);
}

static void drawRows(struct abuf *ab) {
    struct ebuf *b = curBuf();
    int ecols = editorCols(), gw = gutterWidth(b);
    int tc = b ? textCols(b) : ecols;
    int y, tv = treeVisible();
    int wr = 0, wseg = 0; /* wrap-mode walk position */
    int ssy = 0, ssx = 0, sey = 0, sex = 0;
    int hasSel = b ? selGet(b, &ssy, &ssx, &sey, &sex) : 0;

    if (b && b->wrap) {
        wr = b->rowoff;
        wseg = b->segoff;
    }
    for (y = 0; y < E.textrows; y++) {
        abPrintf(ab, "\x1b[%d;1H\x1b[K", y + 1);
        if (!b) {
            drawWelcome(ab, y, ecols);
        } else if (b->wrap) {
            if (wr >= b->numrows) {
                abAppend(ab, "\x1b[90m~\x1b[39m", 11);
            } else {
                struct erow *row = &b->rows[wr];
                int start = wseg * tc;
                int len = row->rsize - start;
                int selx = -1, sele = -1;
                if (len > tc) len = tc;
                if (hasSel && wr >= ssy && wr <= sey) {
                    selx = (wr == ssy) ? editorRowCxToRx(row, ssx) : 0;
                    sele = (wr == sey) ? editorRowCxToRx(row, sex)
                                       : row->rsize;
                }
                if (wseg == 0) /* number only on the first segment */
                    abPrintf(ab, "\x1b[90m%*d \x1b[39m", gw - 1, wr + 1);
                else
                    abPrintf(ab, "%*s", gw, "");
                if (len > 0) drawRowSlice(ab, row, start, len, selx, sele);
                if (++wseg >= rowHeight(b, row)) {
                    wseg = 0;
                    wr++;
                }
            }
        } else {
            int filerow = y + b->rowoff;
            if (filerow >= b->numrows) {
                abAppend(ab, "\x1b[90m~\x1b[39m", 11);
            } else {
                struct erow *row = &b->rows[filerow];
                int len = row->rsize - b->coloff;
                int selx = -1, sele = -1; /* selected rx span on this row */
                if (hasSel && filerow >= ssy && filerow <= sey) {
                    selx = (filerow == ssy) ? editorRowCxToRx(row, ssx) : 0;
                    sele = (filerow == sey) ? editorRowCxToRx(row, sex)
                                            : row->rsize;
                }
                abPrintf(ab, "\x1b[90m%*d \x1b[39m", gw - 1, filerow + 1);
                if (len > tc) len = tc;
                if (len > 0)
                    drawRowSlice(ab, row, b->coloff, len, selx, sele);
            }
        }
        if (tv) {
            abPrintf(ab, "\x1b[%d;%dH\x1b[90m\xe2\x94\x82\x1b[39m",
                     y + 1, ecols + 1);
            if (E.treeoff + y < E.nvis) drawTreeEntry(ab, E.treeoff + y);
        }
    }
}

static void drawStatusBar(struct abuf *ab) {
    struct ebuf *b = curBuf();
    char status[96], rstatus[48];
    int len, rlen;

    abPrintf(ab, "\x1b[%d;1H\x1b[7m", E.textrows + 1);
    if (b) {
        const char *name = b->filename ? strrchr(b->filename, '/') : NULL;
        name = name ? name + 1 : (b->filename ? b->filename : "[no name]");
        len = snprintf(status, sizeof(status), " %.40s%s  %d/%d",
                       name, b->dirty ? " [+]" : "", E.curidx + 1, E.nbufs);
        rlen = snprintf(rstatus, sizeof(rstatus), "%s  %d:%d ",
                        b->syntax ? b->syntax->filetype : "text",
                        b->cy + 1, b->rx + 1);
    } else {
        len = snprintf(status, sizeof(status), " ted v" TED_VERSION);
        rlen = snprintf(rstatus, sizeof(rstatus), "Ctrl-T: tree ");
    }
    if (len > (int)sizeof(status) - 1) len = sizeof(status) - 1;
    if (rlen > (int)sizeof(rstatus) - 1) rlen = sizeof(rstatus) - 1;
    if (len > E.screencols) len = E.screencols;
    abAppend(ab, status, (size_t)len);
    while (len < E.screencols) {
        if (E.screencols - len == rlen) {
            abAppend(ab, rstatus, (size_t)rlen);
            break;
        }
        abAppend(ab, " ", 1);
        len++;
    }
    abAppend(ab, "\x1b[m", 3);
}

static void drawMessageBar(struct abuf *ab) {
    int len = (int)strlen(E.statusmsg);
    abPrintf(ab, "\x1b[%d;1H\x1b[K", E.textrows + 2);
    if (len > E.screencols) len = E.screencols;
    if (len && time(NULL) - E.statusmsg_time < 5)
        abAppend(ab, E.statusmsg, (size_t)len);
}

void editorRefreshScreen(void) {
    struct ebuf *b = curBuf();
    struct abuf *ab = &E.ab;

    editorScroll();
    ab->len = 0;
    abAppend(ab, "\x1b[?25l", 6);
    drawRows(ab);
    drawStatusBar(ab);
    drawMessageBar(ab);
    if (E.focus == FOCUS_EDITOR && b) {
        int gw = gutterWidth(b);
        if (b->wrap) {
            int tc = textCols(b);
            abPrintf(ab, "\x1b[%d;%dH\x1b[?25h", wrapDist(b, tc) + 1,
                     b->rx % tc + gw + 1);
        } else {
            abPrintf(ab, "\x1b[%d;%dH\x1b[?25h", b->cy - b->rowoff + 1,
                     b->rx - b->coloff + gw + 1);
        }
    }
    termWrite(ab->b, ab->len);
}

/* ---- prompt and search ---------------------------------------------------- */

char *editorPrompt(const char *prompt, void (*callback)(char *, int)) {
    size_t bufsize = 128, buflen = 0;
    char *buf = xmalloc(bufsize);
    struct mev m;

    buf[0] = '\0';
    while (1) {
        int c;
        editorSetStatusMessage(prompt, buf);
        editorRefreshScreen();
        c = editorReadKey(&m);
        if (c == KEY_MOUSE) continue;
        if (c == KEY_RESIZE) { updateWindowSize(); continue; }
        if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
            if (buflen) buf[--buflen] = '\0';
        } else if (c == '\x1b') {
            editorSetStatusMessage("");
            if (callback) callback(buf, c);
            free(buf);
            return NULL;
        } else if (c == '\r') { /* may return "": callers decide */
            editorSetStatusMessage("");
            if (callback) callback(buf, c);
            return buf;
        } else if (c >= 32 && c < 127) {
            if (buflen == bufsize - 1) {
                bufsize *= 2;
                buf = xrealloc(buf, bufsize);
            }
            buf[buflen++] = (char)c;
            buf[buflen] = '\0';
        }
        if (callback) callback(buf, c);
    }
}

static void editorFindCallback(char *query, int key) {
    static int last_match = -1, direction = 1;
    static int saved_hl_line;
    static char *saved_hl = NULL;
    struct ebuf *b = curBuf();
    int i, current;

    if (saved_hl) {
        memcpy(b->rows[saved_hl_line].hl, saved_hl,
               (size_t)b->rows[saved_hl_line].rsize);
        free(saved_hl);
        saved_hl = NULL;
    }
    if (key == '\r' || key == '\x1b' || !query[0]) {
        last_match = -1;
        direction = 1;
        return;
    }
    if (key == ARROW_RIGHT || key == ARROW_DOWN) direction = 1;
    else if (key == ARROW_LEFT || key == ARROW_UP) direction = -1;
    else { last_match = -1; direction = 1; }

    if (last_match == -1) direction = 1;
    current = last_match;
    for (i = 0; i < b->numrows; i++) {
        struct erow *row;
        char *match;
        current += direction;
        if (current == -1) current = b->numrows - 1;
        else if (current == b->numrows) current = 0;
        row = &b->rows[current];
        match = strstr(row->render, query);
        if (match) {
            last_match = current;
            b->cy = current;
            b->cx = editorRowRxToCx(row, (int)(match - row->render));
            b->rowoff = b->numrows; /* editorScroll centers the match */
            saved_hl_line = current;
            saved_hl = xmalloc((size_t)(row->rsize ? row->rsize : 1));
            memcpy(saved_hl, row->hl, (size_t)row->rsize);
            memset(&row->hl[match - row->render], HL_MATCH, strlen(query));
            break;
        }
    }
}

static void editorFind(void) {
    struct ebuf *b = curBuf();
    int cx = b->cx, cy = b->cy, coloff = b->coloff, rowoff = b->rowoff;
    b->sel_active = 0;
    char *query = editorPrompt("Search: %s (ESC cancel, arrows next/prev)",
                               editorFindCallback);
    if (query) {
        free(query);
    } else {
        b->cx = cx; b->cy = cy;
        b->coloff = coloff; b->rowoff = rowoff;
    }
}

/* Interactive search & replace over the whole buffer, top to bottom.
 * Each replacement is one undo group; matches preview as a selection. */
static void editorReplace(void) {
    struct ebuf *b = curBuf();
    char *query, *rep, *hit;
    struct mev m;
    int qlen, replen, y = 0, x = 0, all = 0, count = 0, k, h;

    if (!b || b->numrows == 0) return;
    query = editorPrompt("Replace: %s (ESC cancel)", NULL);
    if (!query || !query[0]) {
        free(query);
        return;
    }
    rep = editorPrompt("Replace with: %s", NULL);
    if (!rep) {
        free(query);
        return;
    }
    qlen = (int)strlen(query);
    replen = (int)strlen(rep);
    while (y < b->numrows) {
        hit = strstr(b->rows[y].chars + x, query);
        if (!hit) {
            y++;
            x = 0;
            continue;
        }
        x = (int)(hit - b->rows[y].chars);
        if (!all) {
            b->sel_ay = y;
            b->sel_ax = x;
            b->cy = y;
            b->cx = x + qlen;
            b->sel_active = 1;
            editorSetStatusMessage("Replace? (y)es (n)o (a)ll (q)uit");
            editorRefreshScreen();
            do {
                k = editorReadKey(&m);
                if (k == KEY_RESIZE) updateWindowSize();
            } while (k == KEY_MOUSE || k == KEY_RESIZE);
            if (k == 'q' || k == '\x1b' || k == CTRL_KEY('c') ||
                k == CTRL_KEY('q'))
                break;
            if (k == 'a') all = 1;
            else if (k != 'y') { x++; continue; }
        }
        b->sel_active = 0;
        h = chainBegin(b);
        bufDeleteRange(b, y, x, y, x + qlen);
        bufInsertText(b, rep, (size_t)replen);
        chainEnd(b, h);
        count++;
        y = b->cy; /* resume after the replacement (rep may contain query) */
        x = b->cx;
    }
    b->sel_active = 0;
    editorSetStatusMessage("%d replaced", count);
    free(query);
    free(rep);
}

/* ---- save, quit ------------------------------------------------------------ */

static void editorSave(void) {
    struct ebuf *b = curBuf();
    long n;
    int isnew;
    if (!b) return;
    if (!b->filename) {
        char *fn = editorPrompt("Save as: %s (ESC cancel)", NULL);
        if (!fn || !fn[0]) {
            free(fn);
            editorSetStatusMessage("Save aborted");
            return;
        }
        b->filename = fn;
        editorSelectSyntaxHighlight(b);
    }
    isnew = access(b->filename, F_OK) != 0;
    n = bufWrite(b);
    if (n >= 0) {
        if (isnew) treeNotifySaved(b->filename); /* show it in the tree */
        editorSetStatusMessage("%ld bytes written to %s", n, b->filename);
    } else {
        editorSetStatusMessage("Save failed: %s", strerror(errno));
    }
}

void editorTeardown(void) {
    int i;
    for (i = 0; i < E.nbufs; i++) bufFree(E.bufs[i]);
    free(E.bufs);
    treeFreeAll();
    free(E.vis);
    free(E.clip);
    abFree(&E.ab);
}

static int anyDirty(void) {
    int i;
    for (i = 0; i < E.nbufs; i++)
        if (E.bufs[i]->dirty) return 1;
    return 0;
}

/* ---- cursor movement -------------------------------------------------------- */

static void editorMoveCursor(int key) {
    struct ebuf *b = curBuf();
    struct erow *row = (b->cy >= b->numrows) ? NULL : &b->rows[b->cy];
    int rowlen;

    switch (key) {
    case ARROW_LEFT:
        if (b->cx > 0) b->cx--;
        else if (b->cy > 0) { b->cy--; b->cx = b->rows[b->cy].size; }
        break;
    case ARROW_RIGHT:
        if (row && b->cx < row->size) b->cx++;
        else if (row && b->cx == row->size) { b->cy++; b->cx = 0; }
        break;
    case ARROW_UP:
        if (b->wrap && row) { /* move one visual line within a wrapped row */
            int tc = textCols(b), rx = editorRowCxToRx(row, b->cx);
            if (rx >= tc) {
                b->cx = editorRowRxToCx(row, rx - tc);
                break;
            }
        }
        if (b->cy > 0) b->cy--;
        break;
    case ARROW_DOWN:
        if (b->wrap && row) {
            int tc = textCols(b), rx = editorRowCxToRx(row, b->cx);
            if (rx / tc + 1 < rowHeight(b, row)) {
                int t = rx + tc;
                if (t > row->rsize) t = row->rsize;
                b->cx = editorRowRxToCx(row, t);
                break;
            }
        }
        if (b->cy < b->numrows) b->cy++;
        break;
    }
    row = (b->cy >= b->numrows) ? NULL : &b->rows[b->cy];
    rowlen = row ? row->size : 0;
    if (b->cx > rowlen) b->cx = rowlen;
}

static int isWordChar(int c) {
    return isalnum((unsigned char)c) || c == '_';
}

/* Move to the next word end (dir > 0) or previous word start (dir < 0). */
static void editorMoveWord(struct ebuf *b, int dir) {
    struct erow *row = (b->cy < b->numrows) ? &b->rows[b->cy] : NULL;
    if (dir > 0) {
        if (!row) return;
        if (b->cx >= row->size) { /* at line end: cross to the next line */
            if (b->cy < b->numrows) { b->cy++; b->cx = 0; }
            return;
        }
        while (b->cx < row->size && !isWordChar(row->chars[b->cx])) b->cx++;
        while (b->cx < row->size && isWordChar(row->chars[b->cx])) b->cx++;
    } else {
        if (b->cx == 0) {
            if (b->cy > 0) { b->cy--; b->cx = b->rows[b->cy].size; }
            return;
        }
        if (!row) { b->cx = 0; return; } /* unreachable, but stay safe */
        while (b->cx > 0 && !isWordChar(row->chars[b->cx - 1])) b->cx--;
        while (b->cx > 0 && isWordChar(row->chars[b->cx - 1])) b->cx--;
    }
}

/* ---- mouse -------------------------------------------------------------------- */

static void mouseSetCursor(struct ebuf *b, int x, int y) {
    int gw = gutterWidth(b), rx;
    if (y < 1) y = 1;
    if (y > E.textrows) y = E.textrows;
    if (treeVisible() && x > editorCols()) x = editorCols();
    if (b->wrap) { /* walk from the top to the clicked visual line */
        int tc = textCols(b), r = b->rowoff, seg = b->segoff, steps = y - 1;
        while (steps-- > 0 && r < b->numrows) {
            if (seg + 1 < rowHeight(b, &b->rows[r])) seg++;
            else { r++; seg = 0; }
        }
        b->cy = r;
        if (b->cy < b->numrows) {
            rx = x - 1 - gw;
            if (rx < 0) rx = 0;
            if (rx > tc - 1) rx = tc - 1;
            b->cx = editorRowRxToCx(&b->rows[b->cy], seg * tc + rx);
        } else {
            b->cx = 0;
        }
        return;
    }
    b->cy = b->rowoff + y - 1;
    if (b->cy > b->numrows) b->cy = b->numrows;
    if (b->cy < b->numrows) {
        rx = b->coloff + x - 1 - gw;
        if (rx < 0) rx = 0;
        b->cx = editorRowRxToCx(&b->rows[b->cy], rx);
    } else {
        b->cx = 0;
    }
}

/* After the wrap-mode top moved, pull the cursor into the visible range. */
static void wrapClampCursor(struct ebuf *b, int tc) {
    int r = b->rowoff, seg = b->segoff, last = b->rowoff, lseg = b->segoff, y;
    struct erow *row;
    for (y = 0; y < E.textrows && r < b->numrows; y++) {
        last = r;
        lseg = seg;
        if (seg + 1 < rowHeight(b, &b->rows[r])) seg++;
        else { r++; seg = 0; }
    }
    if (b->cy < b->rowoff) b->cy = b->rowoff;
    if (b->cy > last) b->cy = last;
    row = (b->cy < b->numrows) ? &b->rows[b->cy] : NULL;
    if (b->cx > (row ? row->size : 0)) b->cx = row ? row->size : 0;
    if (row && b->cy == b->rowoff) { /* keep the cursor's segment on screen */
        int rx = editorRowCxToRx(row, b->cx);
        if (rx / tc < b->segoff)
            b->cx = editorRowRxToCx(row, b->segoff * tc);
    }
    if (row && b->cy == last) {
        int rx = editorRowCxToRx(row, b->cx);
        if (rx / tc > lseg) {
            int t = lseg * tc + tc - 1;
            if (t > row->rsize) t = row->rsize;
            b->cx = editorRowRxToCx(row, t);
        }
    }
}

static void handleMouse(struct mev *m) {
    static int dragging = 0;
    struct ebuf *b = curBuf();
    int overTree = treeVisible() && m->x > editorCols();

    if (m->btn == 64 || m->btn == 65) { /* wheel */
        int d = (m->btn == 64) ? -3 : 3;
        if (!m->press) return;
        if (overTree) {
            treeScroll(d);
        } else if (b && b->wrap) { /* scroll by visual lines */
            int i, tc = textCols(b);
            for (i = 0; i < 3; i++) {
                if (d > 0) {
                    if (b->rowoff >= b->numrows) break;
                    if (b->segoff + 1 < rowHeight(b, &b->rows[b->rowoff]))
                        b->segoff++;
                    else { b->rowoff++; b->segoff = 0; }
                } else {
                    if (b->segoff > 0) b->segoff--;
                    else if (b->rowoff > 0) {
                        b->rowoff--;
                        b->segoff = rowHeight(b, &b->rows[b->rowoff]) - 1;
                    }
                }
            }
            wrapClampCursor(b, tc);
        } else if (b) {
            b->rowoff += d;
            if (b->rowoff > b->numrows - 1) b->rowoff = b->numrows - 1;
            if (b->rowoff < 0) b->rowoff = 0;
            if (b->cy < b->rowoff) b->cy = b->rowoff;
            if (b->cy >= b->rowoff + E.textrows)
                b->cy = b->rowoff + E.textrows - 1;
            if (b->cy > b->numrows) b->cy = b->numrows;
            editorMoveCursor(ARROW_LEFT); /* snap cx via the shared clamp */
            editorMoveCursor(ARROW_RIGHT);
        }
        return;
    }
    if (m->btn & 32) { /* drag: extend the selection to the pointer */
        if (dragging && b) {
            mouseSetCursor(b, m->x, m->y);
            b->sel_active = 1;
        }
        return;
    }
    if (!m->press) { /* release: a no-move drag is just a click */
        int sy, sx, ey, ex;
        if (dragging && b && !selGet(b, &sy, &sx, &ey, &ex))
            b->sel_active = 0;
        dragging = 0;
        return;
    }
    if ((m->btn & 0x43) != 0) return; /* only plain left press */
    if (m->y > E.textrows) return;
    if (overTree) {
        dragging = 0;
        treeClick(m->y);
    } else if (b) {
        E.focus = FOCUS_EDITOR;
        mouseSetCursor(b, m->x, m->y);
        b->sel_active = 0;
        b->sel_ay = b->cy;
        b->sel_ax = b->cx;
        dragging = 1;
    }
}

/* ---- key dispatch ---------------------------------------------------------------- */

void editorProcessKeypress(void) {
    static int quit_times = QUIT_TIMES, close_times = QUIT_TIMES;
    static int pasting = 0;
    struct mev m;
    struct ebuf *b;
    int c = editorReadKey(&m), shift;

    switch (c) { /* keys that work regardless of focus */
    case KEY_RESIZE:
        updateWindowSize();
        return;
    case KEY_MOUSE:
        handleMouse(&m);
        return;
    case KEY_PASTE_BEGIN:
        pasting = 1;
        if ((b = curBuf()) != NULL) b->chain = 1; /* paste = one undo group */
        return;
    case KEY_PASTE_END:
        pasting = 0;
        if ((b = curBuf()) != NULL) b->chain = 0;
        return;
    case CTRL_KEY('q'):
    case CTRL_KEY('x'): /* nano habit */
        if (anyDirty() && quit_times > 0) {
            editorSetStatusMessage("Unsaved changes! Press %s again to quit.",
                                   c == CTRL_KEY('x') ? "Ctrl-X" : "Ctrl-Q");
            quit_times--;
            return;
        }
        termWrite("\x1b[2J\x1b[H", 7);
        editorTeardown();
        exit(0);
    case CTRL_KEY('b'):
        E.show_tree = !E.show_tree;
        if (!E.show_tree && E.focus == FOCUS_TREE) E.focus = FOCUS_EDITOR;
        return;
    case CTRL_KEY('t'):
        if (E.focus == FOCUS_EDITOR) {
            E.show_tree = 1;
            E.focus = FOCUS_TREE;
        } else {
            E.focus = FOCUS_EDITOR;
        }
        return;
    }
    quit_times = QUIT_TIMES;
    shift = (c & KEY_SHIFT) != 0;
    c &= ~KEY_SHIFT;

    if (E.focus == FOCUS_TREE) {
        if (!pasting) treeProcessKey(c); /* pasted text must not navigate */
        return;
    }
    b = curBuf();
    switch (c) {
    case '\r':
        if (b) {
            int h = chainBegin(b);
            selDelete(b);
            bufInsertNewline(b, !pasting);
            chainEnd(b, h);
        }
        break;
    case CTRL_KEY('s'):
        editorSave();
        break;
    case CTRL_KEY('f'):
        if (b) editorFind();
        break;
    case CTRL_KEY('n'):
        if (E.nbufs) E.curidx = (E.curidx + 1) % E.nbufs;
        break;
    case CTRL_KEY('p'):
        if (E.nbufs) E.curidx = (E.curidx + E.nbufs - 1) % E.nbufs;
        break;
    case CTRL_KEY('w'):
        if (b && b->dirty && close_times > 0) {
            editorSetStatusMessage("Unsaved changes! "
                                   "Press Ctrl-W again to close.");
            close_times--;
            return;
        }
        if (b) bufListClose(E.curidx);
        break;
    case CTRL_KEY('z'):
        if (b) {
            b->sel_active = 0;
            if (!bufUndo(b)) editorSetStatusMessage("Nothing to undo");
        }
        break;
    case CTRL_KEY('y'):
        if (b) {
            b->sel_active = 0;
            if (!bufRedo(b)) editorSetStatusMessage("Nothing to redo");
        }
        break;
    case CTRL_KEY('g'):
        if (b) {
            char *s = editorPrompt("Go to line: %s (ESC cancel)", NULL);
            if (s && s[0]) {
                int line = atoi(s);
                if (line >= 1 && line <= b->numrows) {
                    b->sel_active = 0;
                    b->cy = line - 1;
                    b->cx = 0;
                } else {
                    editorSetStatusMessage("no such line: %s", s);
                }
            }
            free(s);
        }
        break;
    case CTRL_KEY('r'):
        editorReplace();
        break;
    case CTRL_KEY('e'): /* toggle soft wrap */
        if (b) {
            b->wrap = !b->wrap;
            b->segoff = 0;
            if (b->wrap) b->coloff = 0;
            editorSetStatusMessage("line wrap %s", b->wrap ? "on" : "off");
        }
        break;
    case CTRL_KEY('c'):
        editorCopy(0);
        break;
    case CTRL_KEY('k'): /* cut; nano's cut-line when nothing is selected */
        editorCopy(1);
        break;
    case CTRL_KEY('v'):
    case CTRL_KEY('u'): /* nano's uncut */
        if (b && E.clip) {
            int h = chainBegin(b);
            selDelete(b);
            bufInsertText(b, E.clip, E.cliplen);
            chainEnd(b, h);
        }
        break;
    case CTRL_KEY('a'): /* select all */
        if (b && b->numrows) {
            b->sel_ay = 0;
            b->sel_ax = 0;
            b->sel_active = 1;
            b->cy = b->numrows - 1;
            b->cx = b->rows[b->cy].size;
        }
        break;
    case CTRL_KEY('l'): /* select current line; repeat to extend */
        if (b && b->cy < b->numrows) {
            if (!b->sel_active) {
                b->sel_ay = b->cy;
                b->sel_ax = 0;
                b->sel_active = 1;
            }
            if (b->cy < b->numrows - 1) { b->cy++; b->cx = 0; }
            else b->cx = b->rows[b->cy].size;
        }
        break;
    case BACKSPACE:
        if (b && !selDelete(b)) bufDelChar(b);
        break;
    case CTRL_KEY('h'): /* Ctrl+Backspace: delete word before cursor */
        if (b) {
            if (selDelete(b)) break;
            if (b->cx == 0) {
                bufDelChar(b); /* joins with the previous line */
            } else {
                struct erow *row = &b->rows[b->cy];
                int start = b->cx;
                while (start > 0 && !isWordChar(row->chars[start - 1]))
                    start--;
                while (start > 0 && isWordChar(row->chars[start - 1]))
                    start--;
                b->chain = 1; /* whole word = one undo group */
                while (b->cx > start) bufDelChar(b);
                b->chain = 0;
            }
        }
        break;
    case DEL_KEY:
        if (b && !selDelete(b)) {
            editorMoveCursor(ARROW_RIGHT);
            bufDelChar(b);
        }
        break;
    case CTRL_DEL_KEY: /* delete word after cursor */
        if (b && selDelete(b)) break;
        if (b && b->cy < b->numrows) {
            struct erow *row = &b->rows[b->cy];
            if (b->cx >= row->size) { /* at line end: pull next line up */
                if (b->cy < b->numrows - 1) {
                    editorMoveCursor(ARROW_RIGHT);
                    bufDelChar(b);
                }
            } else {
                int end = b->cx, n;
                while (end < row->size && !isWordChar(row->chars[end])) end++;
                while (end < row->size && isWordChar(row->chars[end])) end++;
                n = end - b->cx;
                b->chain = 1;
                while (n--) {
                    editorMoveCursor(ARROW_RIGHT);
                    bufDelChar(b);
                }
                b->chain = 0;
            }
        }
        break;
    case CTRL_ARROW_LEFT:
    case CTRL_ARROW_RIGHT:
        if (b) {
            if (shift) selAnchor(b); else b->sel_active = 0;
            editorMoveWord(b, c == CTRL_ARROW_RIGHT ? 1 : -1);
        }
        break;
    case CTRL_ARROW_UP: /* jump to previous/next blank line */
        if (b && b->cy > 0) {
            if (shift) selAnchor(b); else b->sel_active = 0;
            do b->cy--; while (b->cy > 0 && b->rows[b->cy].size != 0);
            b->cx = 0;
        }
        break;
    case CTRL_ARROW_DOWN:
        if (b && b->cy < b->numrows) {
            if (shift) selAnchor(b); else b->sel_active = 0;
            do b->cy++; while (b->cy < b->numrows && b->rows[b->cy].size != 0);
            b->cx = 0;
        }
        break;
    case '\t':
        if (b) {
            int h = chainBegin(b);
            selDelete(b);
            if (pasting) { /* keep pasted tabs verbatim */
                bufInsertChar(b, '\t');
            } else {
                int n = TAB_WIDTH - (b->cx % TAB_WIDTH);
                while (n--) bufInsertChar(b, ' ');
            }
            chainEnd(b, h);
        }
        break;
    case HOME_KEY:
        if (b) {
            if (shift) selAnchor(b); else b->sel_active = 0;
            b->cx = 0;
        }
        break;
    case END_KEY:
        if (b) {
            if (shift) selAnchor(b); else b->sel_active = 0;
            if (b->cy < b->numrows) b->cx = b->rows[b->cy].size;
        }
        break;
    case PAGE_UP:
    case PAGE_DOWN:
        if (b) {
            int times = E.textrows;
            if (shift) selAnchor(b); else b->sel_active = 0;
            if (!b->wrap) { /* wrap: visual arrows already page correctly */
                if (c == PAGE_UP) {
                    b->cy = b->rowoff;
                } else {
                    b->cy = b->rowoff + E.textrows - 1;
                    if (b->cy > b->numrows) b->cy = b->numrows;
                }
            }
            while (times--)
                editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
        }
        break;
    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
        if (b) {
            if (shift) selAnchor(b); else b->sel_active = 0;
            editorMoveCursor(c);
        }
        break;
    case '\x1b':
        if (b) b->sel_active = 0;
        break;
    default:
        if (b && c >= 32 && c < 256 && c != 127) {
            int h = chainBegin(b);
            selDelete(b);
            bufInsertChar(b, c);
            chainEnd(b, h);
        }
        break;
    }
    close_times = QUIT_TIMES;
}
