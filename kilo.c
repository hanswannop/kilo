/*** includes ***/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

/*** defines ***/
#define KILO_VERSION "0.0.1"

#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
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

/*** data ***/

typedef struct row {
  int size;
  char *chars;
} row;

struct editorState{
    int cursorX, cursorY;
    int rowOffset;
    int screenRows;
    int screenColumns;
    int totalRows;
    row *rows;
    struct termios originalTermios;
};

struct editorState E;

/*** terminal ***/

void die(const char *s) {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    perror(s);
    exit(1);
}

void disableRawMode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.originalTermios) == -1)
        die("tcsetattr");
}

void enableRawMode() {
    if (tcgetattr(STDIN_FILENO, &E.originalTermios) == -1) die("tcgetattr");
    atexit(disableRawMode);

    struct termios raw = E.originalTermios;
    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

int editorReadKey() {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno !=EAGAIN) die("read");
    }

    if (c == '\x1b'){
        char seq[3];

        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1': return HOME_KEY;
                        case '3': return DEL_KEY;
                        case '4': return END_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
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
    } else {
        return c;
    }
}

int getCursorPosition(int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;


    while (i < sizeof(buf) - 1){
        if(read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if(buf[i] == 'R') break;
        i++;
    }
    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[') return-1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

    return 0;
}

int getWindowSize(int *rows, int *cols) {
    struct winsize ws;

    if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0){
        if(write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
        return getCursorPosition(rows,cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*** row operations ***/

void editorAppendRow(char *s, size_t length) {
    E.rows = realloc(E.rows, sizeof(row) * (E.totalRows + 1));

    int index = E.totalRows;
    E.rows[index].size = length;
    E.rows[index].chars =  malloc(length + 1);
    memcpy(E.rows[index].chars, s, length);
    E.rows[index].chars[length] = '\0';
    E.totalRows++;
}

/*** file i/o ***/

void editorOpen(char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp) die("fopen");

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    while((linelen = getline(&line, &linecap, fp)) != -1) {
        while (linelen > 0 && (line[linelen - 1] == '\n' ||
                               line[linelen - 1] == '\r'))
            linelen--;
        editorAppendRow(line, linelen);
    }
    free(line);
    fclose(fp);
}

/*** Append buffer ***/
struct buffer {
    char *b;
    int len;
};

void appendToBuffer(struct buffer *buf, const char *s, int len) {
    char *new = realloc(buf->b, buf->len + len);

    if (new == NULL) return;
    memcpy(&new[buf->len], s, len);
    buf->b = new;
    buf->len += len;
}

void freeBuffer(struct buffer *buf) {
    free(buf->b);
}

#define BUFFER_INIT {NULL, 0}

/*** Output ***/

void editorScroll() {
    if (E.cursorY < E.rowOffset){ // Cursor above visible window
        E.rowOffset = E.cursorY;
    }
    if (E.cursorY >= E.rowOffset + E.screenRows) { // Cursor below visible window
        E.rowOffset = E.cursorY - E.screenRows + 1;
    }
}

void editorDrawRows(struct buffer *buf) {
    int y;
    for (y = 0; y < E.screenRows; y++) {
        int fileRow = y + E.rowOffset;
        if (fileRow >= E.totalRows) {
            if (E.totalRows == 0 && y == E.screenRows / 3) { // Show only if is new file
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome), "kilo editor -- version %s", KILO_VERSION);
                if (welcomelen > E.screenColumns) welcomelen = E.screenColumns;
                int padding = (E.screenColumns - welcomelen) / 2;
                if (padding) {
                    appendToBuffer(buf, "~", 1);
                    padding--;
                }
                while (padding--) appendToBuffer(buf, " ", 1);
                appendToBuffer(buf, welcome, welcomelen);
            } else {
                appendToBuffer(buf, "~", 1);
            }
        } else {
            int len = E.rows[fileRow].size;
            if (len > E.screenColumns) len = E.screenColumns;
            appendToBuffer(buf, E.rows[fileRow].chars, len);
        }

        appendToBuffer(buf, "\x1b[K", 3); // Clear remaining characters on the line
        if (y < E.screenRows -1) {
            appendToBuffer(buf, "\r\n", 2);
        }
    }
}

void editorRefreshScreen() {
    editorScroll();

    struct buffer buf = BUFFER_INIT; // Create new buffer

    appendToBuffer(&buf, "\x1b[?25l", 6); //Hide cursor
    appendToBuffer(&buf, "\x1b[H", 3); // Zero cursor

    editorDrawRows(&buf); // Draw tildes

    char cursorPosition[32];
    snprintf(cursorPosition, sizeof(cursorPosition), "\x1b[%d;%dH", (E.cursorY - E.rowOffset) + 1, E.cursorX + 1);
    appendToBuffer(&buf, cursorPosition, strlen(cursorPosition)); //Set cursor position

    appendToBuffer(&buf, "\x1b[?25h", 6); //Show cursor

    write(STDOUT_FILENO, buf.b, buf.len); // Write out buffer
    freeBuffer(&buf); // Free buffer
}

/*** Input ***/
void editorMoveCursor(int key) {
    switch(key) {
        case ARROW_LEFT:
            if (E.cursorX != 0){
                E.cursorX--;
            }
            break;
        case ARROW_RIGHT:
            if (E.cursorX != E.screenColumns - 1) {
                E.cursorX++;
            }
            break;
        case ARROW_UP:
            if (E.cursorY != 0) {
                E.cursorY--;
            }
            break;
        case ARROW_DOWN:
            if (E.cursorY != E.totalRows) {
                E.cursorY++;
            }
            break;
    }
}

void editorProcessKeypress() {
    int c = editorReadKey();

    switch (c) {
        case CTRL_KEY('q'):
            editorRefreshScreen();
            exit(0);
            break;

        case HOME_KEY:
            E.cursorX = 0;
            break;

        case END_KEY:
            E.cursorX = E.screenColumns - 1;
            break;

        case PAGE_UP:
        case PAGE_DOWN:
            {
                int times = E.screenRows;
                while (times--)
                    editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
            }
            break;

        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editorMoveCursor(c);
            break;
    }
}

/*** init ***/

void initEditor() {
    E.cursorX = 0;
    E.cursorY = 0;
    E.rowOffset = 0;
    E.totalRows = 0;
    E.rows = NULL;

    if(getWindowSize(&E.screenRows,&E.screenColumns) == -1) die("getWindowSize");
}

int main(int argc, char *argv[]) {
    enableRawMode();
    initEditor();
    if(argc >= 2) {
        editorOpen(argv[1]);
    }

    while(1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}
