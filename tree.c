/* tree.c — lazy-loading file tree pane. */
#include "editor.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

/* Full path of a node (caller frees). Root's name is the root path itself. */
static char *treePath(struct tnode *n) {
    char *pp, *s;
    size_t len;
    if (!n->parent) return xstrdup(n->name);
    pp = treePath(n->parent);
    len = strlen(pp) + 1 + strlen(n->name) + 1;
    s = xmalloc(len);
    snprintf(s, len, "%s/%s", pp, n->name);
    free(pp);
    return s;
}

static int nodeCmp(const void *a, const void *b) {
    const struct tnode *na = *(struct tnode *const *)a;
    const struct tnode *nb = *(struct tnode *const *)b;
    if (na->isdir != nb->isdir) return nb->isdir - na->isdir; /* dirs first */
    return strcasecmp(na->name, nb->name);
}

static void treeFreeNode(struct tnode *n) {
    int i;
    for (i = 0; i < n->nchild; i++) treeFreeNode(n->child[i]);
    free(n->child);
    free(n->name);
    free(n);
}

static void treeLoadNode(struct tnode *n) {
    char *path, *cp;
    DIR *d;
    struct dirent *de;
    struct stat st;
    struct tnode *c;
    size_t cplen;
    int cap = 0;

    if (n->loaded) return;
    path = treePath(n);
    d = opendir(path);
    if (!d) {
        editorSetStatusMessage("cannot open %s: %s", path, strerror(errno));
        free(path);
        return;
    }
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        c = xmalloc(sizeof(*c));
        memset(c, 0, sizeof(*c));
        c->name = xstrdup(de->d_name);
        c->parent = n;
        cplen = strlen(path) + 1 + strlen(de->d_name) + 1;
        cp = xmalloc(cplen);
        snprintf(cp, cplen, "%s/%s", path, de->d_name);
        c->isdir = (stat(cp, &st) == 0 && S_ISDIR(st.st_mode));
        free(cp);
        if (n->nchild == cap) {
            cap = cap ? cap * 2 : 8;
            n->child = xrealloc(n->child, sizeof(*n->child) * (size_t)cap);
        }
        n->child[n->nchild++] = c;
    }
    closedir(d);
    free(path);
    if (n->nchild) /* qsort with a NULL base is UB even for 0 items */
        qsort(n->child, (size_t)n->nchild, sizeof(*n->child), nodeCmp);
    n->loaded = 1;
}

/* ---- flattened view ------------------------------------------------------ */

static void visPush(struct tnode *n, int depth) {
    if (E.nvis == E.viscap) {
        E.viscap = E.viscap ? E.viscap * 2 : 64;
        E.vis = xrealloc(E.vis, sizeof(*E.vis) * (size_t)E.viscap);
    }
    E.vis[E.nvis].node = n;
    E.vis[E.nvis].depth = depth;
    E.nvis++;
}

static void flattenRec(struct tnode *n, int depth) {
    int i;
    visPush(n, depth);
    if (!n->isdir || !n->expanded) return;
    for (i = 0; i < n->nchild; i++) {
        if (!E.show_hidden && n->child[i]->name[0] == '.') continue;
        flattenRec(n->child[i], depth + 1);
    }
}

static void treeEnsureVisible(void) {
    if (E.treesel >= E.nvis) E.treesel = E.nvis - 1;
    if (E.treesel < 0) E.treesel = 0;
    if (E.treesel < E.treeoff) E.treeoff = E.treesel;
    if (E.treesel >= E.treeoff + E.textrows)
        E.treeoff = E.treesel - E.textrows + 1;
    if (E.treeoff > E.nvis - 1) E.treeoff = E.nvis - 1;
    if (E.treeoff < 0) E.treeoff = 0;
}

void treeFlatten(void) {
    E.nvis = 0;
    if (E.root) flattenRec(E.root, 0);
    treeEnsureVisible();
}

/* ---- operations ---------------------------------------------------------- */

void treeInit(const char *root) {
    char *r = xstrdup(root);
    size_t l = strlen(r);
    while (l > 1 && r[l - 1] == '/') r[--l] = '\0';
    E.root = xmalloc(sizeof(*E.root));
    memset(E.root, 0, sizeof(*E.root));
    E.root->name = r;
    E.root->isdir = 1;
    E.root->expanded = 1;
    treeLoadNode(E.root);
    treeFlatten();
}

void treeFreeAll(void) {
    if (E.root) treeFreeNode(E.root);
    E.root = NULL;
}

static void treeOpenSelected(void) {
    struct tnode *n;
    char *path;
    if (E.nvis == 0) return;
    n = E.vis[E.treesel].node;
    if (n->isdir) {
        if (!n->expanded) treeLoadNode(n);
        n->expanded = !n->expanded;
        treeFlatten();
    } else {
        path = treePath(n);
        if (bufListOpen(path))
            E.focus = FOCUS_EDITOR;
        else
            editorSetStatusMessage("cannot open %s: %s", path, strerror(errno));
        free(path);
    }
}

static void nodeReload(struct tnode *n) {
    int i;
    for (i = 0; i < n->nchild; i++) treeFreeNode(n->child[i]);
    free(n->child);
    n->child = NULL;
    n->nchild = 0;
    n->loaded = 0;
    if (n->expanded) treeLoadNode(n);
}

/* Re-scan the selected directory (or the selected file's parent). */
static void treeRefresh(void) {
    struct tnode *n, *target;
    int i;
    if (E.nvis == 0) return;
    n = E.vis[E.treesel].node;
    target = n->isdir ? n : (n->parent ? n->parent : E.root);
    nodeReload(target);
    treeFlatten();
    for (i = 0; i < E.nvis; i++)
        if (E.vis[i].node == target) { E.treesel = i; break; }
    treeEnsureVisible();
}

/* Prompt for a name and create a file (buffer only; disk on first save)
 * or a directory (mkdir now) inside the selected directory. */
static void treeCreate(int isdir) {
    struct tnode *sel, *dir;
    char *name, *dpath, *full;
    size_t len;
    int i;

    if (E.nvis == 0) return;
    sel = E.vis[E.treesel].node;
    dir = sel->isdir ? sel : (sel->parent ? sel->parent : E.root);
    name = editorPrompt(isdir ? "New directory: %s (ESC cancel)"
                              : "New file: %s (ESC cancel)", NULL);
    if (!name) return;
    if (!name[0] || strchr(name, '/') ||
        !strcmp(name, ".") || !strcmp(name, "..")) {
        editorSetStatusMessage("invalid name: %s", name);
        free(name);
        return;
    }
    dpath = treePath(dir);
    len = strlen(dpath) + 1 + strlen(name) + 1;
    full = xmalloc(len);
    snprintf(full, len, "%s/%s", dpath, name);
    if (isdir) {
        if (mkdir(full, 0755) == -1) {
            editorSetStatusMessage("mkdir %s: %s", full, strerror(errno));
        } else {
            dir->expanded = 1;
            nodeReload(dir);
            treeFlatten();
            for (i = 0; i < E.nvis; i++)
                if (E.vis[i].node->parent == dir &&
                    !strcmp(E.vis[i].node->name, name)) {
                    E.treesel = i;
                    break;
                }
            treeEnsureVisible();
            editorSetStatusMessage("created %s", full);
        }
    } else if (bufListOpen(full)) {
        E.focus = FOCUS_EDITOR;
        editorSetStatusMessage("new file: %s (Ctrl-S writes it to disk)", full);
    } else {
        editorSetStatusMessage("cannot open %s: %s", full, strerror(errno));
    }
    free(dpath);
    free(full);
    free(name);
}

static struct tnode *findLoadedDir(struct tnode *n, const char *dpath) {
    struct tnode *r;
    char *p;
    int i, match;
    if (!n->isdir || !n->loaded) return NULL;
    p = treePath(n);
    match = strcmp(p, dpath) == 0;
    free(p);
    if (match) return n;
    for (i = 0; i < n->nchild; i++)
        if ((r = findLoadedDir(n->child[i], dpath)) != NULL) return r;
    return NULL;
}

/* A new file reached the disk: re-scan its directory if we show it. */
void treeNotifySaved(const char *path) {
    struct tnode *n;
    char *dpath;
    const char *slash = strrchr(path, '/');

    if (!E.root) return;
    if (slash) {
        size_t dl = (size_t)(slash - path);
        dpath = xmalloc(dl + 1);
        memcpy(dpath, path, dl);
        dpath[dl] = '\0';
    } else {
        dpath = xstrdup(E.root->name);
    }
    n = findLoadedDir(E.root, dpath);
    free(dpath);
    if (!n) return;
    nodeReload(n);
    treeFlatten();
}

void treeProcessKey(int c) {
    struct tnode *n = E.nvis ? E.vis[E.treesel].node : NULL;
    int i;
    switch (c) {
    case ARROW_UP:   E.treesel--; break;
    case ARROW_DOWN: E.treesel++; break;
    case PAGE_UP:    E.treesel -= E.textrows; break;
    case PAGE_DOWN:  E.treesel += E.textrows; break;
    case HOME_KEY:   E.treesel = 0; break;
    case END_KEY:    E.treesel = E.nvis - 1; break;
    case '\r':       treeOpenSelected(); break;
    case ARROW_RIGHT:
        if (n && n->isdir && !n->expanded) treeOpenSelected();
        break;
    case ARROW_LEFT:
        if (n && n->isdir && n->expanded) {
            n->expanded = 0;
            treeFlatten();
        } else if (n && n->parent) {
            for (i = 0; i < E.nvis; i++)
                if (E.vis[i].node == n->parent) { E.treesel = i; break; }
        }
        break;
    case 'r': case 'R': treeRefresh(); break;
    case 'n': treeCreate(0); break;
    case 'N': treeCreate(1); break;
    case '.':
        E.show_hidden = !E.show_hidden;
        treeFlatten();
        break;
    case '\x1b': E.focus = FOCUS_EDITOR; return;
    default: return;
    }
    treeEnsureVisible();
}

void treeClick(int y) { /* y: 1-based screen row */
    int ti = E.treeoff + y - 1;
    E.focus = FOCUS_TREE;
    if (ti < 0 || ti >= E.nvis) return;
    E.treesel = ti;
    treeOpenSelected();
    treeEnsureVisible();
}

void treeScroll(int delta) {
    int max = E.nvis - E.textrows;
    if (max < 0) max = 0;
    E.treeoff += delta;
    if (E.treeoff > max) E.treeoff = max;
    if (E.treeoff < 0) E.treeoff = 0;
}
