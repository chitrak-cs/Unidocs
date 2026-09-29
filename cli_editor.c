/*
 * minipad.c - A notepad-like, full-screen text editor for the terminal.
 *
 * System Programming Assignment 3, Question 4:
 *   "Design a Text Editor which supports basic editing and file related
 *    operations (e.g., notepad)"
 *
 * The editor talks to the terminal directly: it switches the tty to raw mode
 * (termios), reads key presses byte by byte, decodes escape sequences for the
 * special keys and repaints the screen with VT100/ANSI escape codes.
 *
 * Buffer model : an array of lines (Row). Each row keeps its raw characters
 *                (chars) and a display copy with tabs expanded (render).
 * Undo / redo  : two bounded stacks of whole-buffer snapshots.
 * Clipboard    : an internal buffer shared by cut / copy / paste.
 *
 * Build : cc -Wall -Wextra -o minipad minipad.c      (or just `make`)
 * Run   : ./minipad [file]
 */
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define MINIPAD_VERSION "1.0"
#define TAB_STOP 4
#define UNDO_LIMIT 100
#define MSG_TIMEOUT 5
#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
    BACKSPACE = 127,
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN
};

/* what the previous key press did; consecutive typing is undone as one step */
enum editKind { EDIT_NONE, EDIT_TYPE, EDIT_ERASE };

typedef struct {
    int size;       /* bytes in chars (without the '\0') */
    int rsize;      /* bytes in render */
    char *chars;    /* the line as stored in the file */
    char *render;   /* the line as drawn on screen (tabs expanded) */
} Row;

typedef struct {
    char *text;     /* whole buffer, lines joined with '\n' */
    int len;
    int cx, cy;     /* cursor at the time of the snapshot */
} Snapshot;

typedef struct {
    Snapshot items[UNDO_LIMIT];
    int count;
} History;

struct Editor {
    int cx, cy;                 /* cursor: column in chars, line number */
    int rx;                     /* cursor column in render */
    int rowoff, coloff;         /* first visible line / column */
    int screenrows, screencols; /* text area height, terminal width */
    int gutter, textcols;       /* line-number column width, text width */
    int numrows;
    Row *row;
    int dirty;                  /* unsaved changes? */
    char *filename;
    char statusmsg[160];
    time_t statusmsg_time;
    int mark_active, mx, my;    /* selection anchor */
    char *clip;                 /* clipboard */
    int cliplen;
    History undo, redo;
    enum editKind last_edit;
    int hl_row, hl_start, hl_end; /* search match to highlight (render cols) */
    int find_sx, find_sy;       /* cursor before a search started */
    struct termios orig_termios;
};

static struct Editor E;
static volatile sig_atomic_t winch_pending = 0;

struct abuf {
    char *b;
    int len;
};

#define ABUF_INIT {NULL, 0}

static void refreshScreen(void);
static void setStatus(const char *fmt, ...);
static char *prompt(const char *fmt, void (*callback)(char *, int), const char *init, int allowEmpty);

/* ============================== append buffer ============================== */

static void abAppend(struct abuf *ab, const char *s, int len) {
    char *n = realloc(ab->b, ab->len + len + 1);
    if (n == NULL) return;
    memcpy(n + ab->len, s, len);
    ab->b = n;
    ab->len += len;
    ab->b[ab->len] = '\0';
}

static void abFree(struct abuf *ab) { free(ab->b); }

/* ================================ terminal ================================= */

static void writeOut(const char *s, int len) {
    if (write(STDOUT_FILENO, s, len) == -1) { /* nothing sensible to do */ }
}

static void disableRawMode(void) {
    writeOut("\x1b[?1049l", 8); /* leave the alternate screen */
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios);
}

static void die(const char *s) {
    disableRawMode();
    perror(s);
    exit(1);
}

static void enableRawMode(void) {
    if (!isatty(STDIN_FILENO)) {
        fprintf(stderr, "minipad: standard input is not a terminal\n");
        exit(1);
    }
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
    atexit(disableRawMode);

    struct termios raw = E.orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON); /* ^S/^Q, CR->NL off */
    raw.c_oflag &= ~(OPOST);                                  /* no "\n" -> "\r\n" */
    raw.c_cflag |= CS8;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);          /* ^C/^Z/^V are ours */
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;                                      /* read() times out after 100 ms */
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
    writeOut("\x1b[?1049h", 8); /* alternate screen: the shell is restored on exit */
}

static void handleWinch(int sig) {
    (void)sig;
    winch_pending = 1;
}

/* Read one key press, translating escape sequences into editorKey values. */
static int readKey(void) {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN && errno != EINTR) die("read");
        if (winch_pending) {
            winch_pending = 0;
            refreshScreen();
        }
    }

    if (c != '\x1b') return (unsigned char)c;

    char seq[4];
    if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

    if (seq[0] == '[') {
        if (seq[1] >= '0' && seq[1] <= '9') {
            if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
            if (seq[2] == '~') {
                switch (seq[1]) {
                case '1': case '7': return HOME_KEY;
                case '3': return DEL_KEY;
                case '4': case '8': return END_KEY;
                case '5': return PAGE_UP;
                case '6': return PAGE_DOWN;
                }
            } else if (seq[2] == ';') { /* modified key, e.g. ESC [ 1 ; 5 C */
                if (read(STDIN_FILENO, &seq[3], 1) != 1) return '\x1b';
                if (read(STDIN_FILENO, &seq[3], 1) != 1) return '\x1b';
                switch (seq[3]) {
                case 'A': return ARROW_UP;
                case 'B': return ARROW_DOWN;
                case 'C': return ARROW_RIGHT;
                case 'D': return ARROW_LEFT;
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
                }
            }
        } else {
            switch (seq[1]) {
            case 'A': return ARROW_UP;
            case 'B': return ARROW_DOWN;
            case 'C': return ARROW_RIGHT;
            case 'D': return ARROW_LEFT;
            case 'H': return HOME_KEY;
            case 'F': return END_KEY;
            }
        }
    } else if (seq[0] == 'O') {
        switch (seq[1]) {
        case 'H': return HOME_KEY;
        case 'F': return END_KEY;
        }
    }
    return '\x1b';
}

static void getWindowSize(int *rows, int *cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        *rows = 24;
        *cols = 80;
    } else {
        *rows = ws.ws_row;
        *cols = ws.ws_col;
    }
}

static void updateWindowSize(void) {
    int rows, cols;
    getWindowSize(&rows, &cols);
    E.screenrows = rows > 3 ? rows - 2 : 1; /* status bar + message bar */
    E.screencols = cols;

    int digits = 1;
    for (int n = E.numrows; n >= 10; n /= 10) digits++;
    if (digits < 3) digits = 3;
    E.gutter = digits + 1;
    E.textcols = cols - E.gutter > 1 ? cols - E.gutter : 1;
}

/* ============================== row operations ============================= */

static int cxToRx(const Row *row, int cx) {
    int rx = 0;
    for (int j = 0; j < cx && j < row->size; j++) {
        if (row->chars[j] == '\t') rx += (TAB_STOP - 1) - (rx % TAB_STOP);
        rx++;
    }
    return rx;
}

static void updateRow(Row *row) {
    int tabs = 0;
    for (int j = 0; j < row->size; j++)
        if (row->chars[j] == '\t') tabs++;

    free(row->render);
    row->render = malloc(row->size + tabs * (TAB_STOP - 1) + 1);

    int idx = 0;
    for (int j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            row->render[idx++] = ' ';
            while (idx % TAB_STOP != 0) row->render[idx++] = ' ';
        } else {
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;
}

static void insertRow(int at, const char *s, size_t len) {
    if (at < 0 || at > E.numrows) return;
    E.row = realloc(E.row, sizeof(Row) * (E.numrows + 1));
    memmove(&E.row[at + 1], &E.row[at], sizeof(Row) * (E.numrows - at));

    Row *r = &E.row[at];
    r->size = (int)len;
    r->chars = malloc(len + 1);
    memcpy(r->chars, s, len);
    r->chars[len] = '\0';
    r->rsize = 0;
    r->render = NULL;
    updateRow(r);

    E.numrows++;
    E.dirty++;
}

static void freeRow(Row *row) {
    free(row->render);
    free(row->chars);
}

static void deleteRow(int at) {
    if (at < 0 || at >= E.numrows) return;
    freeRow(&E.row[at]);
    memmove(&E.row[at], &E.row[at + 1], sizeof(Row) * (E.numrows - at - 1));
    E.numrows--;
    E.dirty++;
}

static void rowInsertChar(Row *row, int at, int c) {
    if (at < 0 || at > row->size) at = row->size;
    row->chars = realloc(row->chars, row->size + 2);
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = (char)c;
    updateRow(row);
    E.dirty++;
}

static void rowAppendString(Row *row, const char *s, size_t len) {
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += (int)len;
    row->chars[row->size] = '\0';
    updateRow(row);
    E.dirty++;
}

static void rowDeleteChar(Row *row, int at) {
    if (at < 0 || at >= row->size) return;
    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    updateRow(row);
    E.dirty++;
}

static int rowLen(int y) { return (y >= 0 && y < E.numrows) ? E.row[y].size : 0; }

/* ======================= whole-buffer <-> text helpers ===================== */

static char *rowsToString(int *buflen) {
    int total = 0;
    for (int j = 0; j < E.numrows; j++) total += E.row[j].size + 1;

    char *buf = malloc(total + 1);
    char *p = buf;
    for (int j = 0; j < E.numrows; j++) {
        memcpy(p, E.row[j].chars, E.row[j].size);
        p += E.row[j].size;
        *p++ = '\n';
    }
    *p = '\0';
    *buflen = total;
    return buf;
}

static void freeRows(void) {
    for (int j = 0; j < E.numrows; j++) freeRow(&E.row[j]);
    free(E.row);
    E.row = NULL;
    E.numrows = 0;
}

/* Replace the buffer with `text`, splitting it into lines (CRLF accepted). */
static void loadText(const char *text, int len) {
    freeRows();
    int start = 0;
    for (int i = 0; i <= len; i++) {
        if (i == len && start == len) break;
        if (i == len || text[i] == '\n') {
            int l = i - start;
            if (l > 0 && text[start + l - 1] == '\r') l--;
            insertRow(E.numrows, text + start, l);
            start = i + 1;
        }
    }
}

static void clampCursor(void) {
    if (E.cy > E.numrows) E.cy = E.numrows;
    if (E.cy < 0) E.cy = 0;
    if (E.cx > rowLen(E.cy)) E.cx = rowLen(E.cy);
    if (E.cx < 0) E.cx = 0;
}

/* ================================ undo / redo ============================== */

static void historyPush(History *h, char *text, int len, int cx, int cy) {
    if (h->count == UNDO_LIMIT) { /* drop the oldest snapshot */
        free(h->items[0].text);
        memmove(&h->items[0], &h->items[1], sizeof(Snapshot) * (UNDO_LIMIT - 1));
        h->count--;
    }
    h->items[h->count].text = text;
    h->items[h->count].len = len;
    h->items[h->count].cx = cx;
    h->items[h->count].cy = cy;
    h->count++;
}

static void historyClear(History *h) {
    for (int i = 0; i < h->count; i++) free(h->items[i].text);
    h->count = 0;
}

static void pushCurrent(History *h) {
    int len;
    char *text = rowsToString(&len);
    historyPush(h, text, len, E.cx, E.cy);
}

/* Call before every change to the buffer. */
static void saveUndo(void) {
    pushCurrent(&E.undo);
    historyClear(&E.redo);
}

static void restoreFrom(History *from, History *to, const char *what) {
    if (from->count == 0) {
        setStatus("Nothing to %s", what);
        return;
    }
    pushCurrent(to);
    Snapshot s = from->items[--from->count];
    loadText(s.text, s.len);
    free(s.text);
    E.cx = s.cx;
    E.cy = s.cy;
    clampCursor();
    E.mark_active = 0;
    E.dirty++;
    setStatus("%s done (%d more)", what, from->count);
}

/* ============================ primitive editing ============================ */

static void rawInsertChar(int c) {
    if (E.cy == E.numrows) insertRow(E.numrows, "", 0);
    rowInsertChar(&E.row[E.cy], E.cx, c);
    E.cx++;
}

static void rawInsertNewline(void) {
    if (E.cx == 0) {
        insertRow(E.cy, "", 0);
    } else {
        Row *r = &E.row[E.cy];
        insertRow(E.cy + 1, &r->chars[E.cx], r->size - E.cx);
        r = &E.row[E.cy]; /* insertRow may have moved the array */
        r->size = E.cx;
        r->chars[r->size] = '\0';
        updateRow(r);
    }
    E.cy++;
    E.cx = 0;
}

static void rawBackspace(void) {
    if (E.cy == E.numrows) { /* virtual line after the end: just step back */
        if (E.cy > 0) {
            E.cy--;
            E.cx = E.row[E.cy].size;
        }
        return;
    }
    if (E.cx == 0 && E.cy == 0) return;

    Row *r = &E.row[E.cy];
    if (E.cx > 0) {
        rowDeleteChar(r, E.cx - 1);
        E.cx--;
    } else { /* join with the previous line */
        E.cx = E.row[E.cy - 1].size;
        rowAppendString(&E.row[E.cy - 1], r->chars, r->size);
        deleteRow(E.cy);
        E.cy--;
    }
}

static void rawDeleteForward(void) {
    if (E.cy == E.numrows) return;
    Row *r = &E.row[E.cy];
    if (E.cx < r->size) {
        rowDeleteChar(r, E.cx);
    } else if (E.cy + 1 < E.numrows) {
        rowAppendString(r, E.row[E.cy + 1].chars, E.row[E.cy + 1].size);
        deleteRow(E.cy + 1);
    }
}

static void insertText(const char *s, int len) {
    for (int i = 0; i < len; i++) {
        if (s[i] == '\n') rawInsertNewline();
        else if (s[i] != '\r') rawInsertChar(s[i]);
    }
}

/* ================================ selection ================================ */

/* Ordered selection bounds; returns 0 when nothing is selected. */
static int getSelection(int *y1, int *x1, int *y2, int *x2) {
    if (!E.mark_active) return 0;
    int my = E.my, mx = E.mx;
    if (my > E.numrows) my = E.numrows;
    if (mx > rowLen(my)) mx = rowLen(my);

    if (my < E.cy || (my == E.cy && mx < E.cx)) {
        *y1 = my; *x1 = mx; *y2 = E.cy; *x2 = E.cx;
    } else {
        *y1 = E.cy; *x1 = E.cx; *y2 = my; *x2 = mx;
    }
    return !(*y1 == *y2 && *x1 == *x2);
}

static char *extractRegion(int y1, int x1, int y2, int x2, int *outlen) {
    struct abuf ab = ABUF_INIT;
    for (int y = y1; y <= y2; y++) {
        int from = (y == y1) ? x1 : 0;
        int to = (y == y2) ? x2 : rowLen(y);
        if (y < E.numrows && to > from) abAppend(&ab, &E.row[y].chars[from], to - from);
        if (y < y2) abAppend(&ab, "\n", 1);
    }
    if (ab.b == NULL) ab.b = calloc(1, 1);
    *outlen = ab.len;
    return ab.b;
}

static void deleteRegion(int y1, int x1, int y2, int x2) {
    if (y1 < E.numrows) {
        Row *a = &E.row[y1];
        if (y1 == y2) {
            memmove(&a->chars[x1], &a->chars[x2], a->size - x2 + 1);
            a->size -= x2 - x1;
            updateRow(a);
        } else {
            const char *tail = "";
            int taillen = 0;
            if (y2 < E.numrows) {
                tail = &E.row[y2].chars[x2];
                taillen = E.row[y2].size - x2;
            }
            a->size = x1;
            a->chars[x1] = '\0';
            rowAppendString(a, tail, taillen);
            int last = y2 < E.numrows ? y2 : E.numrows - 1;
            for (int y = last; y > y1; y--) deleteRow(y);
        }
        E.dirty++;
    }
    E.cy = y1;
    E.cx = x1;
}

static int deleteSelectionIfAny(void) {
    int y1, x1, y2, x2;
    int had = getSelection(&y1, &x1, &y2, &x2);
    if (had) deleteRegion(y1, x1, y2, x2);
    E.mark_active = 0;
    return had;
}

/* ================================ clipboard ================================ */

static void setClip(char *s, int len) {
    free(E.clip);
    E.clip = s;
    E.cliplen = len;
}

/* Copy (or cut) the selection; without a selection, the whole current line. */
static void copyCmd(int cut) {
    int y1, x1, y2, x2, len = 0;
    if (getSelection(&y1, &x1, &y2, &x2)) {
        setClip(extractRegion(y1, x1, y2, x2, &len), len);
        if (cut) {
            saveUndo();
            deleteRegion(y1, x1, y2, x2);
        }
        E.mark_active = 0;
        setStatus("%s %d character(s)", cut ? "Cut" : "Copied", len);
        return;
    }
    if (E.cy >= E.numrows) {
        setStatus("Nothing to %s", cut ? "cut" : "copy");
        return;
    }
    Row *r = &E.row[E.cy];
    char *line = malloc(r->size + 2);
    memcpy(line, r->chars, r->size);
    line[r->size] = '\n';
    line[r->size + 1] = '\0';
    setClip(line, r->size + 1);
    if (cut) {
        saveUndo();
        deleteRow(E.cy);
        E.cx = 0;
        clampCursor();
    }
    setStatus("%s line %d", cut ? "Cut" : "Copied", E.cy + 1);
}

static void pasteCmd(void) {
    if (E.clip == NULL) {
        setStatus("Clipboard is empty");
        return;
    }
    saveUndo();
    deleteSelectionIfAny();
    insertText(E.clip, E.cliplen);
    setStatus("Pasted %d character(s)", E.cliplen);
}

static void selectAll(void) {
    E.mark_active = 1;
    E.my = 0;
    E.mx = 0;
    E.cy = E.numrows > 0 ? E.numrows - 1 : 0;
    E.cx = rowLen(E.cy);
    setStatus("Selected everything");
}

/* ============================== file operations ============================ */

static void resetBuffer(void) {
    E.cx = E.cy = E.rx = 0;
    E.rowoff = E.coloff = 0;
    E.mark_active = 0;
    E.hl_row = -1;
    E.last_edit = EDIT_NONE;
    historyClear(&E.undo);
    historyClear(&E.redo);
}

static int loadFile(const char *name) {
    struct stat st;
    if (stat(name, &st) == 0 && S_ISDIR(st.st_mode)) {
        setStatus("'%s' is a directory", name);
        return 0;
    }

    FILE *fp = fopen(name, "rb");
    if (fp == NULL) {
        if (errno != ENOENT) {
            setStatus("Cannot open '%s': %s", name, strerror(errno));
            return 0;
        }
        char *copy = strdup(name); /* `name` may be E.filename itself */
        freeRows();
        free(E.filename);
        E.filename = copy;
        resetBuffer();
        E.dirty = 0;
        setStatus("New file: %s (will be created on save)", E.filename);
        return 1;
    }

    struct abuf ab = ABUF_INIT;
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), fp)) > 0) abAppend(&ab, chunk, (int)n);
    int failed = ferror(fp);
    fclose(fp);
    if (failed) {
        abFree(&ab);
        setStatus("Error while reading '%s'", name);
        return 0;
    }

    char *copy = strdup(name);
    loadText(ab.b ? ab.b : "", ab.len);
    abFree(&ab);
    free(E.filename);
    E.filename = copy;
    resetBuffer();
    E.dirty = 0;
    setStatus("Opened '%s' (%d lines)", E.filename, E.numrows);
    return 1;
}

static int askKey(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
    refreshScreen();
    int c = readKey();
    setStatus("");
    return c < 128 ? tolower(c) : c;
}

static int saveFile(int saveAs) {
    if (saveAs || E.filename == NULL) {
        char *name = prompt("Save as: %s  (Enter: save, Esc: cancel)", NULL, E.filename, 0);
        if (name == NULL) {
            setStatus("Save cancelled");
            return 0;
        }
        struct stat st;
        if ((E.filename == NULL || strcmp(name, E.filename) != 0) && stat(name, &st) == 0) {
            if (askKey("'%s' already exists. Overwrite? (y/n)", name) != 'y') {
                free(name);
                setStatus("Save cancelled");
                return 0;
            }
        }
        free(E.filename);
        E.filename = name;
    }

    int len;
    char *buf = rowsToString(&len);
    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
    if (fd != -1) {
        if (ftruncate(fd, len) != -1 && write(fd, buf, len) == len) {
            close(fd);
            free(buf);
            E.dirty = 0;
            setStatus("%d bytes written to '%s'", len, E.filename);
            return 1;
        }
        close(fd);
    }
    free(buf);
    setStatus("Cannot save '%s': %s", E.filename, strerror(errno));
    return 0;
}

/* Returns 1 when it is OK to throw away the current buffer. */
static int confirmDiscard(const char *action) {
    if (!E.dirty) return 1;
    int k = askKey("Save changes to '%s' before %s? (y = save, n = discard, Esc = cancel)",
                   E.filename ? E.filename : "Untitled", action);
    if (k == 'y') return saveFile(0);
    if (k == 'n') return 1;
    setStatus("Cancelled");
    return 0;
}

static void newFileCmd(void) {
    if (!confirmDiscard("creating a new file")) return;
    freeRows();
    free(E.filename);
    E.filename = NULL;
    resetBuffer();
    E.dirty = 0;
    setStatus("New untitled document");
}

static void openCmd(void) {
    if (!confirmDiscard("opening another file")) return;
    char *name = prompt("Open file: %s  (Enter: open, Esc: cancel)", NULL, NULL, 0);
    if (name == NULL) {
        setStatus("Open cancelled");
        return;
    }
    loadFile(name);
    free(name);
}

static void statsCmd(void) {
    long chars = 0, words = 0;
    for (int y = 0; y < E.numrows; y++) {
        Row *r = &E.row[y];
        int inword = 0;
        for (int j = 0; j < r->size; j++) {
            if (isspace((unsigned char)r->chars[j])) {
                inword = 0;
            } else if (!inword) {
                inword = 1;
                words++;
            }
        }
        chars += r->size + 1;
    }

    char disk[64] = "not saved yet";
    struct stat st;
    if (E.filename && stat(E.filename, &st) == 0)
        snprintf(disk, sizeof(disk), "%lld bytes on disk", (long long)st.st_size);

    setStatus("%s | Lines: %d  Words: %ld  Chars: %ld | %s%s",
              E.filename ? E.filename : "Untitled", E.numrows, words, chars, disk,
              E.dirty ? " (modified)" : "");
}

/* ============================ search and replace =========================== */

/*
 * Find `q` starting at (*r, *c). Forward searches match at or after the
 * position; backward searches match strictly before it. Wraps around.
 */
static int findMatch(const char *q, int *r, int *c, int dir) {
    int n = E.numrows;
    if (n == 0 || q[0] == '\0') return 0;
    int row = *r, col = *c;

    for (int i = 0; i <= n; i++) {
        Row *rw = &E.row[row];
        if (dir > 0) {
            char *p = col <= rw->size ? strstr(rw->chars + col, q) : NULL;
            if (p) {
                *r = row;
                *c = (int)(p - rw->chars);
                return 1;
            }
            row = (row + 1) % n;
            col = 0;
        } else {
            int best = -1;
            for (char *p = rw->chars; (p = strstr(p, q)) != NULL && p - rw->chars < col; p++)
                best = (int)(p - rw->chars);
            if (best >= 0) {
                *r = row;
                *c = best;
                return 1;
            }
            row = (row - 1 + n) % n;
            col = E.row[row].size + 1;
        }
    }
    return 0;
}

static void findCallback(char *q, int key) {
    static int mr = -1, mc = -1;

    if (key == '\r' || key == '\x1b') {
        mr = -1;
        if (key == '\x1b') E.hl_row = -1;
        return;
    }

    int dir = 1;
    if (key == ARROW_RIGHT || key == ARROW_DOWN) dir = 1;
    else if (key == ARROW_LEFT || key == ARROW_UP) dir = -1;
    else mr = -1; /* query changed: search again from where we started */

    int r, c;
    if (mr == -1) {
        r = E.find_sy;
        c = E.find_sx;
        dir = 1;
    } else {
        r = mr;
        c = dir > 0 ? mc + 1 : mc;
    }
    if (r >= E.numrows) {
        r = 0;
        c = 0;
    }

    E.hl_row = -1;
    if (findMatch(q, &r, &c, dir)) {
        mr = r;
        mc = c;
        E.cy = r;
        E.cx = c;
        Row *row = &E.row[r];
        E.hl_row = r;
        E.hl_start = cxToRx(row, c);
        E.hl_end = cxToRx(row, c + (int)strlen(q));
    }
}

static void findCmd(void) {
    int saved_cx = E.cx, saved_cy = E.cy, saved_rowoff = E.rowoff, saved_coloff = E.coloff;
    E.find_sx = E.cx;
    E.find_sy = E.cy;
    E.mark_active = 0;

    char *q = prompt("Find: %s  (Arrows: next/prev, Enter: stop, Esc: cancel)", findCallback, NULL, 0);
    if (q == NULL) {
        E.cx = saved_cx;
        E.cy = saved_cy;
        E.rowoff = saved_rowoff;
        E.coloff = saved_coloff;
        return;
    }
    if (E.hl_row == -1) setStatus("'%s' not found", q);
    free(q);
}

static void replaceCmd(void) {
    char *q = prompt("Replace - find what: %s  (Esc: cancel)", NULL, NULL, 0);
    if (q == NULL) return;
    char *w = prompt("Replace with: %s  (Enter: replace all, Esc: cancel)", NULL, NULL, 1);
    if (w == NULL) {
        free(q);
        return;
    }

    int qlen = (int)strlen(q), wlen = (int)strlen(w), count = 0;
    for (int y = 0; y < E.numrows; y++)
        for (char *p = E.row[y].chars; (p = strstr(p, q)) != NULL; p += qlen) count++;

    if (count > 0) {
        saveUndo();
        for (int y = 0; y < E.numrows; y++) {
            Row *r = &E.row[y];
            if (strstr(r->chars, q) == NULL) continue;
            struct abuf ab = ABUF_INIT;
            char *p = r->chars, *m;
            while ((m = strstr(p, q)) != NULL) {
                abAppend(&ab, p, (int)(m - p));
                abAppend(&ab, w, wlen);
                p = m + qlen;
            }
            abAppend(&ab, p, (int)strlen(p));
            if (ab.b == NULL) ab.b = calloc(1, 1);
            free(r->chars);
            r->chars = ab.b;
            r->size = ab.len;
            updateRow(r);
        }
        E.dirty++;
        E.mark_active = 0;
        clampCursor();
    }
    setStatus("Replaced %d occurrence(s) of '%s'", count, q);
    free(q);
    free(w);
}

static void gotoCmd(void) {
    char fmt[64];
    snprintf(fmt, sizeof(fmt), "Go to line (1-%d): %%s", E.numrows > 0 ? E.numrows : 1);
    char *s = prompt(fmt, NULL, NULL, 0);
    if (s == NULL) return;
    int n = atoi(s);
    free(s);
    if (n < 1) {
        setStatus("Invalid line number");
        return;
    }
    if (n > E.numrows) n = E.numrows > 0 ? E.numrows : 1;
    E.cy = n - 1;
    E.cx = 0;
    E.mark_active = 0;
    clampCursor();
}

static void runCmd(void) {
    if (!E.filename) {
        setStatus("Save the file first before running!");
        return;
    }
    if (E.dirty) saveFile(0);

    char cmd[1024] = {0};
    char *ext = strrchr(E.filename, '.');
    if (ext && strcmp(ext, ".py") == 0) {
        snprintf(cmd, sizeof(cmd), "python3 %s", E.filename);
    } else if (ext && (strcmp(ext, ".c") == 0 || strcmp(ext, ".cpp") == 0)) {
        snprintf(cmd, sizeof(cmd), "gcc %s -o temp_cli_run && ./temp_cli_run", E.filename);
    } else if (ext && strcmp(ext, ".js") == 0) {
        snprintf(cmd, sizeof(cmd), "node %s", E.filename);
    } else if (ext && strcmp(ext, ".java") == 0) {
        snprintf(cmd, sizeof(cmd), "javac %s && java %s", E.filename, E.filename);
    } else {
        setStatus("Run not supported for this file type!");
        return;
    }

    disableRawMode();
    writeOut("\x1b[2J\x1b[H", 7);
    printf("Running: %s\n", cmd);
    printf("========================================\n");
    int ret = system(cmd);
    printf("\n========================================\n");
    printf("Process exited with code %d\n", ret);
    printf("Press ENTER to return to the editor...\n");
    while(getchar() != '\n');
    enableRawMode();
    refreshScreen();
    setStatus("Finished running %s", E.filename);
}

/* ================================== output ================================= */

static void scroll(void) {
    E.rx = E.cy < E.numrows ? cxToRx(&E.row[E.cy], E.cx) : 0;

    if (E.cy < E.rowoff) E.rowoff = E.cy;
    if (E.cy >= E.rowoff + E.screenrows) E.rowoff = E.cy - E.screenrows + 1;
    if (E.rx < E.coloff) E.coloff = E.rx;
    if (E.rx >= E.coloff + E.textcols) E.coloff = E.rx - E.textcols + 1;
}

static void drawRows(struct abuf *ab) {
    int y1 = 0, x1 = 0, y2 = -1, x2 = 0;
    int hasSel = getSelection(&y1, &x1, &y2, &x2);

    for (int y = 0; y < E.screenrows; y++) {
        int filerow = y + E.rowoff;
        if (filerow >= E.numrows) {
            if (E.numrows == 0 && y == E.screenrows / 3) {
                char welcome[80];
                int wl = snprintf(welcome, sizeof(welcome),
                                  "MiniPad %s  -  press Ctrl-T for help", MINIPAD_VERSION);
                if (wl > E.screencols) wl = E.screencols;
                int pad = (E.screencols - wl) / 2;
                abAppend(ab, "\x1b[90m~\x1b[0m", 10);
                while (--pad > 0) abAppend(ab, " ", 1);
                abAppend(ab, welcome, wl);
            } else {
                abAppend(ab, "\x1b[90m~\x1b[0m", 10);
            }
        } else {
            char num[32];
            int nl = snprintf(num, sizeof(num), "\x1b[90m%*d \x1b[0m", E.gutter - 1, filerow + 1);
            abAppend(ab, num, nl);

            Row *r = &E.row[filerow];
            int ss = -1, se = -1;
            if (hasSel && filerow >= y1 && filerow <= y2) {
                ss = filerow == y1 ? cxToRx(r, x1) : 0;
                se = filerow == y2 ? cxToRx(r, x2) : r->rsize;
            }

            int len = r->rsize - E.coloff;
            if (len < 0) len = 0;
            if (len > E.textcols) len = E.textcols;

            int inverse = 0;
            for (int j = 0; j < len; j++) {
                int rx = j + E.coloff;
                int want = (rx >= ss && rx < se) ||
                           (filerow == E.hl_row && rx >= E.hl_start && rx < E.hl_end);
                if (want != inverse) {
                    abAppend(ab, want ? "\x1b[7m" : "\x1b[27m", want ? 4 : 5);
                    inverse = want;
                }
                char ch = r->render[rx];
                if (iscntrl((unsigned char)ch)) abAppend(ab, "?", 1);
                else abAppend(ab, &ch, 1);
            }
            if (inverse) abAppend(ab, "\x1b[27m", 5);

            /* a selected line break is shown as one highlighted cell */
            if (ss >= 0 && filerow < y2 && r->rsize >= E.coloff && r->rsize - E.coloff < E.textcols)
                abAppend(ab, "\x1b[7m \x1b[27m", 10);
        }
        abAppend(ab, "\x1b[K\r\n", 5);
    }
}

static void drawStatusBar(struct abuf *ab) {
    char left[160], right[64];
    int ll = snprintf(left, sizeof(left), " %.40s%s  |  %d lines%s",
                      E.filename ? E.filename : "[Untitled]", E.dirty ? " [modified]" : "",
                      E.numrows, E.mark_active ? "  |  SELECTING" : "");
    int rl = snprintf(right, sizeof(right), "Ln %d, Col %d ", E.cy + 1, E.rx + 1);
    if (ll > E.screencols) ll = E.screencols;

    abAppend(ab, "\x1b[7m", 4);
    abAppend(ab, left, ll);
    while (ll < E.screencols) {
        if (E.screencols - ll == rl) {
            abAppend(ab, right, rl);
            break;
        }
        abAppend(ab, " ", 1);
        ll++;
    }
    abAppend(ab, "\x1b[m\r\n", 5);
}

static void drawMessageBar(struct abuf *ab) {
    abAppend(ab, "\x1b[K", 3);
    int msglen = (int)strlen(E.statusmsg);
    if (msglen && time(NULL) - E.statusmsg_time < MSG_TIMEOUT) {
        if (msglen > E.screencols) msglen = E.screencols;
        abAppend(ab, E.statusmsg, msglen);
    } else {
        const char *hint = " ^T Help  ^S Save  ^O Open  ^F Find  ^Z Undo  ^Q Quit";
        int hl = (int)strlen(hint);
        if (hl > E.screencols) hl = E.screencols;
        abAppend(ab, "\x1b[90m", 5);
        abAppend(ab, hint, hl);
        abAppend(ab, "\x1b[0m", 4);
    }
}

static void refreshScreen(void) {
    updateWindowSize();
    scroll();

    struct abuf ab = ABUF_INIT;
    abAppend(&ab, "\x1b[?25l\x1b[H", 9); /* hide cursor, go home */
    drawRows(&ab);
    drawStatusBar(&ab);
    drawMessageBar(&ab);

    char buf[32];
    int bl = snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1,
                      (E.rx - E.coloff) + 1 + E.gutter);
    abAppend(&ab, buf, bl);
    abAppend(&ab, "\x1b[?25h", 6);

    writeOut(ab.b, ab.len);
    abFree(&ab);
}

static void setStatus(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}

static void helpScreen(void) {
    static const char *lines[] = {
        "MiniPad " MINIPAD_VERSION " - keyboard reference",
        "",
        "FILE     Ctrl-N  New           Ctrl-O  Open          Ctrl-S  Save",
        "         Ctrl-W  Save As       Ctrl-E  File info     Ctrl-Q  Quit",
        "         Ctrl-P  Run (Python, C/C++, Java, JS)",
        "",
        "EDIT     Ctrl-Z  Undo          Ctrl-Y  Redo",
        "         Ctrl-B  Start/stop selection (then move the cursor)",
        "         Ctrl-A  Select all    Esc     Clear selection",
        "         Ctrl-X  Cut           Ctrl-C  Copy          Ctrl-V  Paste",
        "                 (with no selection, cut/copy work on the whole line)",
        "         Backspace / Delete, Enter, Tab, any printable key",
        "",
        "SEARCH   Ctrl-F  Find (arrows jump to next / previous match)",
        "         Ctrl-R  Replace all   Ctrl-G  Go to line",
        "",
        "MOVE     Arrow keys, Home / End, Page Up / Page Down",
        "",
        "Press any key to return to the document.",
    };
    int n = (int)(sizeof(lines) / sizeof(lines[0]));

    updateWindowSize();
    struct abuf ab = ABUF_INIT;
    abAppend(&ab, "\x1b[?25l\x1b[2J\x1b[H", 13);
    for (int i = 0; i < n && i < E.screenrows + 2; i++) {
        int l = (int)strlen(lines[i]);
        if (l > E.screencols - 2) l = E.screencols - 2 > 0 ? E.screencols - 2 : 0;
        abAppend(&ab, "  ", 2);
        if (i == 0) abAppend(&ab, "\x1b[1m", 4);
        abAppend(&ab, lines[i], l);
        if (i == 0) abAppend(&ab, "\x1b[0m", 4);
        abAppend(&ab, "\r\n", 2);
    }
    writeOut(ab.b, ab.len);
    abFree(&ab);
    readKey();
}

/* ================================== input ================================== */

/*
 * Ask for a line of text in the message bar. `fmt` must contain one %s,
 * which shows what has been typed. Returns a malloc'd string, or NULL on Esc.
 * `callback` (optional) is called after every key, e.g. for incremental search.
 */
static char *prompt(const char *fmt, void (*callback)(char *, int), const char *init, int allowEmpty) {
    size_t len = init ? strlen(init) : 0;
    size_t cap = len + 128;
    char *buf = malloc(cap);
    memcpy(buf, init ? init : "", len + 1);

    while (1) {
        setStatus(fmt, buf);
        refreshScreen();

        int c = readKey();
        if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
            if (len != 0) buf[--len] = '\0';
        } else if (c == '\x1b') {
            setStatus("");
            if (callback) callback(buf, c);
            free(buf);
            return NULL;
        } else if (c == '\r') {
            if (len != 0 || allowEmpty) {
                setStatus("");
                if (callback) callback(buf, c);
                return buf;
            }
        } else if (c < 128 && !iscntrl(c)) {
            if (len == cap - 1) {
                cap *= 2;
                buf = realloc(buf, cap);
            }
            buf[len++] = (char)c;
            buf[len] = '\0';
        }
        if (callback) callback(buf, c);
    }
}

static void moveCursor(int key) {
    Row *row = E.cy < E.numrows ? &E.row[E.cy] : NULL;

    switch (key) {
    case ARROW_LEFT:
        if (E.cx != 0) {
            E.cx--;
        } else if (E.cy > 0) {
            E.cy--;
            E.cx = E.row[E.cy].size;
        }
        break;
    case ARROW_RIGHT:
        if (row && E.cx < row->size) {
            E.cx++;
        } else if (row && E.cy < E.numrows) {
            E.cy++;
            E.cx = 0;
        }
        break;
    case ARROW_UP:
        if (E.cy != 0) E.cy--;
        break;
    case ARROW_DOWN:
        if (E.cy < E.numrows) E.cy++;
        break;
    }
    clampCursor();
}

static void processKey(void) {
    int c = readKey();
    enum editKind kind = EDIT_NONE;
    E.hl_row = -1;

    switch (c) {
    /* ---- file ---- */
    case CTRL_KEY('q'):
        if (!confirmDiscard("quitting")) break;
        writeOut("\x1b[2J\x1b[H", 7);
        exit(0);
        break;
    case CTRL_KEY('s'): saveFile(0); break;
    case CTRL_KEY('w'): saveFile(1); break;
    case CTRL_KEY('n'): newFileCmd(); break;
    case CTRL_KEY('o'): openCmd(); break;
    case CTRL_KEY('e'): statsCmd(); break;
    case CTRL_KEY('t'): helpScreen(); break;
    case CTRL_KEY('p'): runCmd(); break;

    /* ---- search ---- */
    case CTRL_KEY('f'): findCmd(); break;
    case CTRL_KEY('r'): replaceCmd(); break;
    case CTRL_KEY('g'): gotoCmd(); break;

    /* ---- clipboard / selection / history ---- */
    case CTRL_KEY('b'):
        if (E.mark_active) {
            E.mark_active = 0;
            setStatus("Selection cleared");
        } else {
            E.mark_active = 1;
            E.my = E.cy;
            E.mx = E.cx;
            setStatus("Selection started - move the cursor, then ^C / ^X");
        }
        break;
    case CTRL_KEY('a'): selectAll(); break;
    case CTRL_KEY('c'): copyCmd(0); break;
    case CTRL_KEY('x'): copyCmd(1); break;
    case CTRL_KEY('v'): pasteCmd(); break;
    case CTRL_KEY('z'): restoreFrom(&E.undo, &E.redo, "Undo"); break;
    case CTRL_KEY('y'): restoreFrom(&E.redo, &E.undo, "Redo"); break;
    case '\x1b': E.mark_active = 0; break;

    /* ---- editing ---- */
    case '\r':
        saveUndo();
        deleteSelectionIfAny();
        rawInsertNewline();
        break;
    case BACKSPACE:
    case CTRL_KEY('h'):
    case DEL_KEY: {
        int y1, x1, y2, x2;
        if (getSelection(&y1, &x1, &y2, &x2)) {
            saveUndo();
            deleteSelectionIfAny();
            break;
        }
        E.mark_active = 0;
        kind = EDIT_ERASE;
        if (E.last_edit != EDIT_ERASE) saveUndo();
        if (c == DEL_KEY) rawDeleteForward();
        else rawBackspace();
        break;
    }

    /* ---- movement ---- */
    case HOME_KEY: E.cx = 0; break;
    case END_KEY: E.cx = rowLen(E.cy); break;
    case PAGE_UP:
    case PAGE_DOWN:
        if (c == PAGE_UP) {
            E.cy = E.rowoff;
        } else {
            E.cy = E.rowoff + E.screenrows - 1;
            if (E.cy > E.numrows) E.cy = E.numrows;
        }
        for (int times = E.screenrows; times--;) moveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
        break;
    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
        moveCursor(c);
        break;

    default:
        if (c == '\t' || (c >= 32 && c < 127)) {
            int y1, x1, y2, x2;
            int sel = getSelection(&y1, &x1, &y2, &x2);
            /* one undo step per word: a new snapshot at each word boundary */
            if (sel || E.last_edit != EDIT_TYPE || c == ' ' || c == '\t') saveUndo();
            deleteSelectionIfAny();
            rawInsertChar(c);
            kind = EDIT_TYPE;
        }
        break;
    }
    E.last_edit = kind;
}

/* =================================== main ================================== */

static void initEditor(void) {
    memset(&E, 0, sizeof(E));
    E.hl_row = -1;
    updateWindowSize();
}

int main(int argc, char *argv[]) {
    if (argc > 1 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        printf("usage: %s [file]\nA notepad-like terminal text editor. Press Ctrl-T inside for help.\n", argv[0]);
        return 0;
    }

    initEditor();
    enableRawMode();

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handleWinch;
    sigaction(SIGWINCH, &sa, NULL);

    if (argc > 1) loadFile(argv[1]);
    else setStatus("Welcome to MiniPad!  Ctrl-T shows all commands.");

    while (1) {
        refreshScreen();
        processKey();
    }
    return 0;
}
