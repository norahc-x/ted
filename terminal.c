/* terminal.c — raw mode, window size, key and mouse decoding. */
#include "editor.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

static volatile sig_atomic_t resize_pending = 0;

static void sigwinchHandler(int sig) { (void)sig; resize_pending = 1; }

void termWrite(const char *s, size_t n) {
    while (n) {
        ssize_t w = write(STDOUT_FILENO, s, n);
        if (w == -1 && errno == EINTR) continue;
        if (w <= 0) return;
        s += w;
        n -= (size_t)w;
    }
}

void die(const char *s) {
    termWrite("\x1b[2J\x1b[H", 7);
    perror(s);
    exit(1);
}

void disableRawMode(void) {
    if (!E.rawmode) return;
    termWrite("\x1b[?1002;1006;2004l\x1b[?25h", 24); /* mouse off, cursor on */
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios);
    E.rawmode = 0;
}

void enableRawMode(void) {
    struct termios raw;
    struct sigaction sa;

    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
    atexit(disableRawMode);

    raw = E.orig_termios;
    raw.c_iflag &= ~(tcflag_t)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(tcflag_t)OPOST;
    raw.c_cflag |= CS8;
    raw.c_lflag &= ~(tcflag_t)(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;   /* read() polls: returns after ... */
    raw.c_cc[VTIME] = 1;  /* ...at most 100ms with no input  */
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
    E.rawmode = 1;
    termWrite("\x1b[?1002;1006;2004h", 18); /* SGR mouse+drag, bracketed paste */

    sa.sa_handler = sigwinchHandler;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGWINCH, &sa, NULL);
}

int getWindowSize(int *rows, int *cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        *rows = 24; /* no reliable size (e.g. detached pty): sane default */
        *cols = 80;
        return 0;
    }
    *rows = ws.ws_row;
    *cols = ws.ws_col;
    return 0;
}

static int pushback = -1; /* byte wrongly consumed after a bare ESC */

/* Read one byte, honoring the poll timeout so resizes are noticed. */
static int readByte(char *c) {
    ssize_t n;
    if (pushback != -1) {
        *c = (char)pushback;
        pushback = -1;
        return 1;
    }
    while ((n = read(STDIN_FILENO, c, 1)) != 1) {
        if (n == -1 && errno != EAGAIN && errno != EINTR) die("read");
        if (resize_pending) return 0;
    }
    return 1;
}

/* Decode one keypress; fills *m and returns KEY_MOUSE for mouse input. */
int editorReadKey(struct mev *m) {
    char c, seq[2];
    int p[3] = {0, 0, 0}, pi = 0, ctrl, shift, alt, key = 0;
    char fin = 0;

    if (!readByte(&c)) { resize_pending = 0; return KEY_RESIZE; }
    if (c != '\x1b') return (unsigned char)c;

    /* Escape sequence: further bytes must arrive within the poll timeout. */
    if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
    if (seq[0] == 'O') {
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';
        if (seq[1] == 'H') return HOME_KEY;
        if (seq[1] == 'F') return END_KEY;
        return '\x1b';
    }
    if (seq[0] != '[') { /* bare ESC then a normal key: keep the key */
        pushback = (unsigned char)seq[0];
        return '\x1b';
    }
    if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

    if (seq[1] == '<') { /* SGR mouse: \x1b[<btn;x;y then M (press) / m */
        while (read(STDIN_FILENO, &c, 1) == 1) {
            if (c >= '0' && c <= '9') p[pi] = p[pi] * 10 + (c - '0');
            else if (c == ';') { if (++pi > 2) return '\x1b'; }
            else break;
        }
        if ((c == 'M' || c == 'm') && m) {
            m->btn = p[0]; m->x = p[1]; m->y = p[2];
            m->press = (c == 'M');
            return KEY_MOUSE;
        }
        return '\x1b';
    }

    /* Generic CSI: optional params, then a final byte. */
    if (seq[1] >= '0' && seq[1] <= '9') p[0] = seq[1] - '0';
    else if (seq[1] != ';') fin = seq[1];
    while (!fin) {
        if (read(STDIN_FILENO, &c, 1) != 1) return '\x1b';
        if (c >= '0' && c <= '9') { if (pi < 2) p[pi] = p[pi] * 10 + (c - '0'); }
        else if (c == ';') { if (pi < 2) pi++; }
        else fin = c;
    }
    ctrl = p[1] >= 2 && ((p[1] - 1) & 4); /* xterm modifier: 1 + bitmask */
    shift = p[1] >= 2 && ((p[1] - 1) & 1);
    alt = p[1] >= 2 && ((p[1] - 1) & 2);
    switch (fin) {
    case 'A': key = ctrl ? CTRL_ARROW_UP : alt ? ALT_ARROW_UP : ARROW_UP; break;
    case 'B': key = ctrl ? CTRL_ARROW_DOWN : alt ? ALT_ARROW_DOWN : ARROW_DOWN; break;
    case 'C': key = ctrl ? CTRL_ARROW_RIGHT : ARROW_RIGHT; break;
    case 'D': key = ctrl ? CTRL_ARROW_LEFT : ARROW_LEFT; break;
    case 'H': key = HOME_KEY; break;
    case 'F': key = END_KEY; break;
    case '~':
        switch (p[0]) {
        case 1: case 7: key = HOME_KEY; break;
        case 4: case 8: key = END_KEY; break;
        case 3: key = ctrl ? CTRL_DEL_KEY : DEL_KEY; break;
        case 5: key = PAGE_UP; break;
        case 6: key = PAGE_DOWN; break;
        case 200: return KEY_PASTE_BEGIN;
        case 201: return KEY_PASTE_END;
        }
        break;
    }
    if (!key) return '\x1b';
    return shift ? (key | KEY_SHIFT) : key;
}
