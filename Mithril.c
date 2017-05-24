//
// Created by syvon on 5/23/17.
//
#include <stdlib.h>
#include <stdio.h>
#include <termios.h>
#include <unistd.h>
#include <asm/errno.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <memory.h>
#include <fcntl.h>

#define VERSION "1.0.0"


// allows to get the keycode of a ctrl+something key
#define CTRL_KEY(k) ((k) & 0x1f)


// TODO : Delete a line is broken
// TODO : adding a line is weird

enum EditorKey {
    BACKSPACE = 127,
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PG_UP,
    PG_DOWN
};

void fatal(char *message) {
    write(STDOUT_FILENO, "\x1b[2J", 4); //send an escape sequence to erase the screen
    write(STDOUT_FILENO, "\x1b[H", 3); //resends the cursor at the top of the screen
    perror(message);
    exit(1);
}

/**
 * A text row
 * We use rawSize and rawContent
 * to be able to render some
 * characters differently (like tabs)
 */
struct Row {
    int rawSize;
    char *rawContent;

    int realSize;
    char *realContent;
};

struct Tab {
    char *fileName;
    int numRows;
    struct Row *rows;
};

/**
 * A struct that
 * contains the environnement
 * settings like the available rows
 */
struct Environment {
    int screenRows;
    int screenCols;
    /*** The user's terminal settings ***/
    struct termios orig_termios;
};

struct Environment env;

/**
 * A struct to store
 * the current session
 * of editing
 */
struct Session {
    /*** Cursor positioning ***/
    int colOffset;
    int rowOffset;
    int cursorRow;
    int cursorCol;
    /*** tabs that the user can open ***/
    int currentTabIdx;
    int numTabs;
    struct Tab *tabs;
};

struct Session currentSession;


/**
 *
 * @return the character read from the input
 */
int readKey() {
    int lenRead;
    char cRead;
    while ((lenRead = read(STDIN_FILENO, &cRead, 1) != 1)) {
        if (lenRead == -1 && errno != EAGAIN) {
            fatal("read");
        }
    }

    if (cRead == '\x1b') {
        char seq[3];

        if (read(STDIN_FILENO, &seq[0], 1) != 1) {
            return '\x1b';
        }

        if (read(STDIN_FILENO, &seq[1], 1) != 1) {
            return '\x1b';
        }

        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {

                if (read(STDIN_FILENO, &seq[2], 1) != 1) {
                    return '\x1b';
                }

                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1':
                            return HOME_KEY;
                        case '2':
                            return END_KEY;
                        case '3':
                            return DEL_KEY;
                        case '5':
                            return PG_UP;
                        case '6':
                            return PG_DOWN;
                        case '7':
                            return HOME_KEY;
                        case '8':
                            return END_KEY;
                        default:
                            return '\x1b';
                    }
                }

            } else {
                switch (seq[1]) {
                    case 'A':
                        return ARROW_UP;
                    case 'B':
                        return ARROW_DOWN;
                    case 'C':
                        return ARROW_RIGHT;
                    case 'D':
                        return ARROW_LEFT;
                    case 'H':
                        return HOME_KEY;
                    case 'F':
                        return END_KEY;
                    default:
                        return '\x1b';
                }
            }
        }

        return '\x1b';

    }

    return cRead;
}


/**
 * Get the current tab being edited
 * @return a pointer on the tab or NULL if none
 */
struct Tab *getCurrentTab() {

    struct Tab *tab;

    if ((currentSession.numTabs) > 0 && (currentSession.currentTabIdx < currentSession.numTabs)) {
        tab = &currentSession.tabs[currentSession.currentTabIdx];
    } else {
        tab = NULL;
    }

    return tab;
}

/**
 * Gets the current row being edited
 * @return a pointer on the row or NULL if none
 */
struct Row *getCurrentRow() {
    struct Row *row;

    struct Tab *currentTab = getCurrentTab();

    if (currentTab) {
        int realRowIdx = (currentSession.cursorRow + currentSession.rowOffset);

        if ((realRowIdx > -1) && (realRowIdx < currentTab->numRows)) {
            row = &currentTab->rows[realRowIdx];
        } else {
            row = NULL;
        }
    } else {
        row = NULL;
    }

    return row;
}

/**
 * Gets the position of the cursor
 * @param rows an int pointer towards the var we want to fill with the number of rows
 * @param cols an int pointer towards the var we want to fill with the number of cols
 * @return -1 on failure, 0 on success
 */
int getCursorPosition(int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) {
        return -1;
    }

    while (i < (sizeof(buf) - 1)) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) {
            break;
        }

        if (buf[i] == 'R') {
            break;
        }
        ++i;
    }
    buf[i] = '\0';
    if (buf[0] != '\x1b' || buf[1] != '[') { return -1; }
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) { return -1; }

    return 0;
}

/**
 * Gets the window size of the terminal of the user
 * @param rows an int pointer
 * @param cols an int pointer
 * @return -1 on failure, 0 on success
 */
int getWindowSize(int *rows, int *cols) {
    struct winsize ws;
    //pass a reference to the ws struct to ioctrl (& is dereferencing)
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {

        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) {
            return -1;
        }

        return getCursorPosition(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}


/*** row operations ***/

void editorUpdateRow(struct Row *row) {
    int tabs = 0;
    int j;

    for (j = 0; j < row->rawSize; ++j) {
        if (row->rawContent[j] == '\t') {
            ++tabs;
        }
    }

    if (row->realContent) free(row->realContent); // clear since length might not be good

    row->realContent = malloc((size_t) ((row->rawSize + 1) + (tabs * 7) + 1));

    int idx = 0;

    for (j = 0; j < row->rawSize; ++j) {

        if (row->rawContent[j] == '\t') {
            row->realContent[idx++] = ' ';// add one because the tab takes at least one space
            while (idx % 8 != 0) { // go to a tab stop (each 8 char is a tab col)
                row->realContent[idx++] = ' ';
            }
        } else {
            row->realContent[idx++] = row->rawContent[j];
        }
    }

    row->realContent[idx] = '\0';
    row->realSize = idx;
}


void editorRowInsertChar(struct Row *row, int at, int c) {

    if (!row) {
        fatal("Missing row (editorInsertChar)");
        return;
    }

    if (at < 0 || at > row->rawSize) {
        at = row->rawSize;
    }

    row->rawContent = realloc(row->rawContent, (size_t) row->rawSize + 2);

    memmove(&row->rawContent[at + 1], &row->rawContent[at], (size_t) (row->rawSize - at + 1));
    ++row->rawSize;
    row->rawContent[at] = (char) c;

    editorUpdateRow(row);
}

void editorAppendRow(char *s, size_t len) {

    struct Tab *currentTab = getCurrentTab();

    //add more room for the rows
    size_t rowSize = sizeof(struct Row);
    currentTab->rows = realloc(currentTab->rows, rowSize * (currentTab->numRows + 1));

    int at = currentTab->numRows;

    currentTab->rows[at].rawSize = (int) len;
    currentTab->rows[at].rawContent = malloc(len + 1);
    currentTab->rows[at].realSize = 0;
    currentTab->rows[at].realContent = NULL;
    //fill the content
    memcpy(currentTab->rows[at].rawContent, s, len);
    currentTab->rows[at].rawContent[len] = '\0';

    editorUpdateRow(&currentTab->rows[at]);

    ++currentTab->numRows;
}

/*** Editor operation ***/

void editorInsertChar(int c) {
    struct Tab *currentTab = getCurrentTab();

    if (!currentTab) {
        fatal("Missing tab (editorInsertChar)");
        return;
    }

    if (currentSession.cursorRow == currentTab->numRows) {
        editorAppendRow("", 0);
    }

    editorRowInsertChar(getCurrentRow(), currentSession.cursorCol, c);
    ++currentSession.cursorCol;
}

void editorBackspace() {
    struct Row *row = getCurrentRow();

    if (!row) {
        fatal("Missing row (backspace)");
        return;
    }

    int pos = currentSession.cursorCol + currentSession.colOffset;

    if (pos == 0 || pos > row->rawSize) {
        return;
    }

    //We delete the char before
    memmove(&row->rawContent[pos - 1], &row->rawContent[pos], (size_t) (row->rawSize - (pos - 1) - 1));

    --(row->rawSize);
    --(currentSession.cursorCol);
    editorUpdateRow(row);
}

/*** file i/o ***/

char *editorRowsToString(int *bufLen) {

    struct Tab *tab = getCurrentTab();

    if (!tab) {
        fatal("Missing tab (editorRowsToString)");
        return NULL;
    }


    int totalLen = 0;
    int j;
    for (j = 0; j < (tab->numRows); ++j) {
        totalLen += tab->rows[j].rawSize + 1;
    }

    *bufLen = totalLen;

    char *buf = malloc((size_t) totalLen);
    char *p = buf;

    for (j = 0; j < (tab->numRows); ++j) {
        memcpy(p, tab->rows[j].rawContent, (size_t) tab->rows[j].rawSize);
        p += tab->rows[j].rawSize;
        *p = '\n';
        ++p;
    }

    return buf;
}

void editorSave() {

    struct Tab *tab = getCurrentTab();

    if (NULL == tab) {
        fatal("No current tab (editorSave)");
        return;
    }

    if (NULL == tab->fileName) {
        return;
    }

    int len;
    char *buf = editorRowsToString(&len);

    int fd = open(tab->fileName, O_RDWR | O_CREAT, 0644);

    if (fd != -1) {
        if (-1 != ftruncate(fd, len)) {
            write(fd, buf, (size_t) len);
        }
        close(fd);
    }
    free(buf);
}

void editorOpen(char *filename) {

    struct Tab *currentTab = getCurrentTab();

    FILE *fp = fopen(filename, "r");

    if (!fp) {
        fatal("fopen");
    } else {
        currentTab->fileName = filename;
    }

    char *line = NULL;
    ssize_t lineLen;
    size_t lineCap = 0;

    while ((lineLen = getline(&line, &lineCap, fp)) != -1) {

        if (lineLen > -1) {
            while (lineLen > 0 && (line[lineLen - 1] == '\n' ||
                                   line[lineLen - 1] == '\r')) {
                lineLen--;
            }
            editorAppendRow(line, (size_t) lineLen);
        }
    }

    free(line);
    fclose(fp);
}


/*** small string ***/

struct SmallStr {
    char *b;
    int len;
};

#define SMALLSTR_INIT {NULL, 0};

void appendToStr(struct SmallStr *str, const char *s, int len) {

    char *new = realloc(str->b, (size_t) (str->len + len));

    if (new == NULL) return;

    memcpy(&new[str->len], s, (size_t) len);

    str->b = new;
    str->len += len;

}

void clearStr(struct SmallStr *str) {
    free(str->b);
}

void editorScroll() {

    if (currentSession.cursorRow < currentSession.rowOffset) {
        currentSession.rowOffset = currentSession.cursorRow;
    } else if (currentSession.cursorRow >= (currentSession.rowOffset + env.screenRows)) {
        currentSession.rowOffset = (currentSession.cursorRow - env.screenRows) + 1;
    }

    if (currentSession.cursorCol < currentSession.colOffset) {
        currentSession.colOffset = currentSession.cursorCol;
    } else if (currentSession.cursorCol >= (currentSession.colOffset + env.screenCols)) {
        currentSession.colOffset = (currentSession.cursorCol - env.screenCols) + 1;
    }
}

void init() {
    currentSession.colOffset = 0;
    currentSession.rowOffset = 0;

    currentSession.cursorRow = 0;
    currentSession.cursorCol = 0;

    currentSession.currentTabIdx = 0;
    currentSession.numTabs = 1;
    size_t tabSize = sizeof(struct Tab);
    currentSession.tabs = malloc(tabSize);

    int *cols = &env.screenCols;
    int *rows = &env.screenRows;

    if (getWindowSize(rows, cols) == -1) {
        fatal("getWindowSize");
    }
}

void disableRawMode() {
    //takes the origin settings and restores them
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &env.orig_termios) == -1)
        fatal("tcsetattr");
}

void setRawMode() {
    //save the current settings in the static var
    if (tcgetattr(STDIN_FILENO, &env.orig_termios) == -1)
        fatal("tcgetattr");
    //when we exit, we call disable_raw_mode
    atexit(disableRawMode);

    struct termios raw = env.orig_termios;

    tcgetattr(STDIN_FILENO, &raw);
    //disable the signal for ctrl-s and ctrl-q, so stop output
    raw.c_iflag &= ~(IXON);
    //disable the post output
    //That means that without a \r we wont ge the return
    raw.c_oflag &= ~(OPOST);
    //disable the echo
    //disable the canonical mode
    //disable the ctrl-c
    raw.c_lflag &= ~(ECHO | ICANON | ISIG);
    //minimum byte size before read can return
    raw.c_cc[VMIN] = 0;
    //maximum time to can elapse before read returns
    raw.c_cc[VTIME] = 1;

    //set the terms settings to the new ones
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
        fatal("tcsetattr");
}

void setCursorAtStart() {
    write(STDOUT_FILENO, "\x1b[H", 3); //resends the cursor at the top of the screen
}

void clearAllLinesAndGoToStart(struct SmallStr *str) {
    appendToStr(str, "\x1b[2J", 4); //send an escape sequence to erase the screen
    setCursorAtStart();
}

void editorInvertColor(struct SmallStr *str) {
    appendToStr(str, "\x1b[7m", 4);
}

void editorNormalColor(struct SmallStr *str) {
    appendToStr(str, "\x1b[m", 3);
}

void editorDrawStatusBar(struct SmallStr *str) {
    editorInvertColor(str);

    appendToStr(str, "Status bar will be here", 23);

    editorNormalColor(str);
}

void editorDrawRows(struct SmallStr *str) {

    struct Tab *tab = getCurrentTab();

    if (!tab) {
        fatal("No tab (editorDrawRow");
    }

    //printf("%d rows\r\n", currentSession.screenRows);

    int drawableRows = env.screenRows - 2;

    for (int y = 0; y < drawableRows; ++y) {
        int fileRow = y + currentSession.rowOffset;
        if (fileRow >= tab->numRows) {
            if ((tab->numRows == 0) && (y == (drawableRows / 3) + 1)) {

                char welcome[80];
                int welcomeLen = snprintf(welcome, sizeof(welcome), "-- Mithril -- version %s", VERSION);

                if (welcomeLen > env.screenCols) {
                    welcomeLen = env.screenCols;
                }

                int padding = (env.screenCols - welcomeLen) / 2;

                if (padding) {
                    appendToStr(str, "~", 1);
                    --padding;
                }

                while ((padding--) > 0) {
                    appendToStr(str, " ", 1);
                }

                appendToStr(str, welcome, welcomeLen);
            } else {
                appendToStr(str, "~", 1);
            }
        } else {
            int len = tab->rows[fileRow].realSize;
            len = len - currentSession.colOffset;
            //make sure the len is never more than what we actually can display
            //if we have a col offset of 5 on a len 10 sentence
            //we want to start at the 5th char
            //but we only want to display 5 chars, not the 10
            if (len < 0) {
                len = 0;
            }

            if (len > env.screenCols) {
                len = env.screenCols;
            }

            appendToStr(str, (tab->rows[fileRow].realContent + currentSession.colOffset), len);
        }


        appendToStr(str, "\x1b[K", 3);
        appendToStr(str, "\r\n", 2);
    }
}

void editorRefreshScreen() {

    editorScroll();

    struct SmallStr str = SMALLSTR_INIT;

    appendToStr(&str, "\x1b[?25l", 6);
    appendToStr(&str, "\x1b[H", 3);

    editorDrawRows(&str);
    editorDrawStatusBar(&str);


    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH",
             (currentSession.cursorRow - currentSession.rowOffset) + 1,
             (currentSession.cursorCol - currentSession.colOffset) + 1);

    appendToStr(&str, buf, (int) strlen(buf));

    // Hides the cursor when repainting
    // appendToStr(&str, "\x1b[H", 3);
    appendToStr(&str, "\x1b[?25h", 6);

    write(STDOUT_FILENO, str.b, (size_t) str.len);
    clearStr(&str);
}


/*** input ***/

void moveToBeginningOfLine() {
    currentSession.cursorCol = 0;
    currentSession.colOffset = 0;
}

void moveToEndOfLine() {
    struct Row *row = getCurrentRow();
    if (row) {
        currentSession.cursorCol = row->rawSize;
    }
}


void snapAtEndIfPast() {
    struct Row *row = getCurrentRow();

    if (row && ((currentSession.cursorCol > row->rawSize) || (currentSession.colOffset > row->rawSize))) {
        currentSession.cursorCol = row->rawSize;

        if (row->rawSize < env.screenCols) {
            currentSession.colOffset = 0;
        } else {
            editorScroll();
        }
    } else if (!row) {
        currentSession.cursorCol = 0;
    }

    editorScroll();
}

void editorCursorMove(int code) {

    struct Tab *currentTab = getCurrentTab();

    if (!currentTab) return;

    struct Row *row = getCurrentRow();

    switch (code) {
        case ARROW_UP:
            if (currentSession.cursorRow > 0) {
                currentSession.cursorRow--;
                snapAtEndIfPast();
            }
            break;
        case ARROW_DOWN:

            if (currentSession.cursorRow < (currentTab->numRows)) {
                currentSession.cursorRow++;
                snapAtEndIfPast();
            }
            break;
        case ARROW_LEFT:
            if (currentSession.cursorCol > 0) {
                currentSession.cursorCol--;
            } else if (currentSession.cursorRow > 0) {
                --currentSession.cursorRow;
                moveToEndOfLine();
            }
            break;
        case ARROW_RIGHT:
            if (row && currentSession.cursorCol < row->rawSize) {
                currentSession.cursorCol++;
            } else if (currentSession.cursorRow + currentSession.rowOffset < (currentTab->numRows - 1)) {
                ++currentSession.cursorRow;
                moveToBeginningOfLine();
            }
            break;
        default:
            break;
    }
}


void processKeyPress() {
    int c = readKey();

    switch (c) {
        case '\r':
            fatal("Someone pressed enter! :D");
            break;
        case '\n':
            fatal("Someone pressend enter! :D 4r");
            break;
        case CTRL_KEY('q'): {
            struct SmallStr str = SMALLSTR_INIT;
            clearAllLinesAndGoToStart(&str);
            write(STDOUT_FILENO, str.b, (size_t) str.len);
            clearStr(&str);
            exit(0);
        }
        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editorCursorMove(c);
            break;
        case PG_UP:
        case PG_DOWN: {
            int times = env.screenRows;
            while ((times--) > 0) {
                editorCursorMove((c == PG_UP) ? ARROW_UP : ARROW_DOWN);
            }
        }
            break;
        case HOME_KEY:
        case END_KEY: {
            int times = env.screenCols;
            while ((times--) > 0) {
                editorCursorMove((c == HOME_KEY) ? ARROW_LEFT : ARROW_RIGHT);
            }
        }
            break;
        case BACKSPACE:
        case CTRL_KEY('h'):
        case DEL_KEY:
            editorBackspace();
            break;

        case CTRL_KEY('l'):
        case '\x1b':
            break;

        case CTRL_KEY('o'):
            editorSave();
            break;

        default:
            editorInsertChar(c);
            break;
    }
}


int main(int argc, char *argv[]) {

    setRawMode();
    init();

    //Load the files in args
    if (argc > 1) {
        editorOpen(argv[1]);
    }

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-noreturn"
    while (1) {
        editorRefreshScreen();
        processKeyPress();
    }
#pragma clang diagnostic pop
}