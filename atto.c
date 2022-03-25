/*** Includes ***/

// Feature test macro
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/*** defines ***/

// Text editor version
#define ATTO_VERSION "0.1"

// Tab size
#define ATTO_TAB_STOP 4

// Quit confirmation
#define ATTO_QUIT_TIMES 2

// Input for (Ctrl + Something)
// Ctrl key combined with alphabetical keys produce 1-26
#define CTRL_KEY(k) ((k) & 0x1F)

// Editor buttons
enum editorKey
{
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

/*** Data ***/

// Contents of each row
typedef struct erow
{
    int size;
    int rsize;      // Size of the contents of render
    char* chars;
    char* render;   // Actual string to render
} erow;

struct editorConfig
{
    // Cursor location (index into chars field of an erow)
    int cx;
    int cy;

    // Cursor location (index into the `render` field)
    int rx;

    // Row and column offset for scrolling
    int rowoff;
    int coloff;

    // Window dimensions
    int screenrows;
    int screencols;

    // Contents of each row
    int numrows;
    erow* row;

    // Dirty flag (Unsaved changes)
    int dirty;

    // Name of file opened
    char* filename;

    // Status message
    char statusmsg[80];
    time_t statusmsg_time;

    // Save a copy of termios in its original state
    struct termios orig_termios;
};

struct editorConfig E;

/*** Prototypes ***/

void editorSetStatusMessage(const char* fmt, ...);
void editorRefreshScreen();
char* editorPrompt(char* prompt);

/*** Terminal ***/

// ------------------------------------------
// Error Handling
// ------------------------------------------

// Prints an error message and exits the program
void die(const char *s)
{
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    // `perror()` looks at the global "errno" variable and prints a descriptive error message for it
    perror(s);
    exit(1);
}

// ----------------
// Disable Raw Mode
// ----------------
void disableRawMode()
{
    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
    {
        die("tcsetattr");
    }
}

// ---------------
// Enable Raw Mode
// ---------------
void enableRawMode()
{
    // Terminal attributes can be read into a termios with `tcgetattr()`
    if(tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)
    {
        die("tcgetattr");
    }

    // `atexit()` comes from <stdlib.h>
    // This function is used to automatically call `disableRawMode()` when the program exits
    atexit(disableRawMode);

    struct termios raw = E.orig_termios;

    // ----------------------------------
    // Turning Off Certain Terminal Flags
    // ##################################
    // `c_iflag` field is for "input flags".
    // ----------------------------------
    // ICRNL        : flag to disable (Ctrl + M).
    // (Ctrl + M) is read as 10 rather than 13 in the terminal. We need to fix this.
    // ----------------------------------
    // IXON         : flag used for software flow control, (Ctrl + S) and (Ctrl + Q).
    // (Ctrl + S) stops data from being transmitted to the terminal until you press (Ctrl + Q).
    // ##################################
    // `c_oflag` field is for "output flags".
    // ----------------------------------
    // OPOST        : flag for turning off all "post-processing of output".
    // In practice, the "\n" to "\r\n" translation is likely the only output processing feature turned on by default.
    // ##################################
    // `c_lflag` field is for "local flags"
    // ----------------------------------
    // ECHO         : causes each key you type to be printed to the terminal.
    // This is useful in canonical mode but gets in the way in raw mode, so we turn it off.
    // ----------------------------------
    // ICANON       : allows us to set canonical mode.
    // this means we will be reading input byte-by-byte, instead of line-by-line.
    // ----------------------------------
    // IEXTEN       : flag to turn off (Ctrl + V).
    // (Ctrl + V) waits for you to type another character and sends that character literally.
    // ----------------------------------
    // ISIG         : flag to turn off SIGINT (Ctrl + C) signals and SIGTSTP (Ctrl + Z) signals.
    // SIGINT causes the process to terminate and SIGTSTP causes the process to suspend. We'll disable them.
    // ##################################
    // Miscellaneous Flags (Probably no need)
    // ----------------------------------
    // BRKINT       : when turned on, a break condition will cause a SIGINT signal.
    // INPCK        : this flag enables parity checking.
    // ISTRIP       : Causes the 8th bit of each input byte to be stripped, meaning it will set it to 0.
    // CS8          : Not a flag. It sets the character size (CS) to 8 bits per byte.
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | ISIG);

    // Timeout for `read()`
    // --------------------
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    // Terminal attributes can be applied to the terminal after modifying with `tcsetattr`
    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
    {
        die("tcsetattr");
    }
}

// ----------------------------
// Read Key From Standard Input
// ----------------------------
int editorReadKey()
{
    int nread;
    char c;
    while((nread = read(STDIN_FILENO, &c, 1)) != 1)
    {
        if(nread == -1 && errno != EAGAIN)
        {
            die("read");
        }
    }

    // Read key presses with multiple bytes
    // Example : Arrow keys are in the form `\x1b`, `[`, followed by an `A`, `B`, `C`, or `D`.
    if(c == '\x1b')
    {
        char seq[3];

        if(read(STDIN_FILENO, &seq[0], 1) != 1)
            return '\x1b';

        if(read(STDIN_FILENO, &seq[1], 1) != 1)
            return '\x1b';

        if(seq[0] == '[')
        {
            if(seq[1] >= '0' && seq[1] <= '9')
            {
                if(read(STDIN_FILENO, &seq[2], 1) != 1)
                    return '\x1b';

                if(seq[2] == '~')
                {
                    switch(seq[1])
                    {
                        case '1':
                            return HOME_KEY;
                        case '3':
                            return DEL_KEY;
                        case '4':
                            return END_KEY;
                        case '5':
                            return PAGE_UP;
                        case '6':
                            return PAGE_DOWN;
                        case '7':
                            return HOME_KEY;
                        case '8':
                            return END_KEY;
                    }
                }
            }
            else
            {
                switch (seq[1])
                {
                    // Arrow Keys
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
                }
            }
        }
        else if(seq[0] == 'O')
        {
            switch(seq[1])
            {
                case 'H':
                    return HOME_KEY;
                case 'F':
                    return END_KEY;
            }
        }
        return '\x1b';
    }
    else
    {
        return c;
    }
}

// ------------------------------
// Get Current Position of Cursor
// ------------------------------
int getCursorPosition(int* rows, int* cols)
{
    char buf[32];
    unsigned int i = 0;

    if(write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
    {
        return -1;
    }

    while(i < sizeof(buf) - 1)
    {
        if(read(STDIN_FILENO, &buf[i], 1) != 1)
        {
            break;
        }

        if(buf[i] == 'R')
        {
            break;
        }

        ++i;
    }

    buf[i] = '\0';

    if(buf[0] != '\x1b' || buf[1] != '[')
        return -1;
    if(sscanf(&buf[2], "%d;%d", rows, cols) != 2)
        return -1;

    return 0;
}

// ----------------------------
// Get The Size of The Terminal
// ----------------------------
int getWindowSize(int* rows, int* cols)
{
    struct winsize ws;

    // We can get the size of the terminal by simply calling ioctl()
    if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)
    {
        if(write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
        {
            return -1;
        }
        return getCursorPosition(rows, cols);
    }
    else
    {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;

    }
}

/*** Row Operations ***/

// -------------------------------------------
// Convert `chars` index into a `render` index
// -------------------------------------------
int editorRowCxToRx(erow* row, int cx)
{
    int rx = 0;

    int j;
    for(j = 0; j < cx; ++j)
    {
        if(row->chars[j] == '\t')
        {
            rx += (ATTO_TAB_STOP - 1) - (rx % ATTO_TAB_STOP);
        }
        ++rx;
    }

    return rx;
}

// -------------------------------------------------------------------------------
// Uses the `chars` string of an `erow` to fill in the contents of `render` string
// -------------------------------------------------------------------------------
void editorUpdateRow(erow* row)
{
    int tabs = 0;

    int j;
    for(j = 0; j < row->size; ++j)
    {
        if(row->chars[j] == '\t')
            ++tabs;
    }

    free(row->render);
    row->render = malloc(row->size + tabs*(ATTO_TAB_STOP - 1) + 1);

    int idx = 0;
    for(j = 0; j < row->size; ++j)
    {
        if(row->chars[j] == '\t')
        {
            // Render tabs as 4 spaces
            row->render[idx++] = ' ';
            while(idx % ATTO_TAB_STOP != 0)
            {
                row->render[idx++] = ' ';
            }
        }
        else
        {
            row->render[idx++] = row->chars[j];
        }
    }

    row->render[idx] = '\0';
    row->rsize = idx;
}


// ----------------------------------------------------------------
// Insert a row of text at the index specified by the `at` argument
// ----------------------------------------------------------------
void editorInsertRow(int at, char* s, size_t len)
{
    // If `at` row not within boundaries
    if(at < 0 || at > E.numrows)
        return;

    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));

    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    editorUpdateRow(&E.row[at]);

    ++E.numrows;
    ++E.dirty;
}

// --------------------------------------------------
// Freeing the memory owned by `erow` we are deleting
// --------------------------------------------------
void editorFreeRow(erow* row)
{
    free(row->render);
    free(row->chars);
}

// --------------
// Deleting a row
// --------------
void editorDelRow(int at)
{
    if(at < 0 || at >= E.numrows)
        return;
    
    editorFreeRow(&E.row[at]);
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
    --E.numrows;
    ++E.dirty;
}

// -----------------------------------
// Insert single character into `erow`
// -----------------------------------
void editorRowInsertChar(erow* row, int at, int c)
{
    if(at < 0 || at > row->size)
    {
        at = row->size;
    }

    row->chars = realloc(row->chars, row->size + 2);
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = c;

    editorUpdateRow(row);
    ++E.dirty;
}

// ------------------------------------
// Appends a string to the end of a row
// ------------------------------------
void editorRowAppendString(erow* row, char* s, size_t len)
{
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    ++E.dirty;
}

// --------------------------------
// Deletes a character in an `erow`
// --------------------------------
void editorRowDelChar(erow* row, int at)
{
    if(at < 0 || at >= row->size)
        return;
    
    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    editorUpdateRow(row);
    ++E.dirty;
}

/*** Editor Operations ***/

// --------------------------------------------------------
// Take a chracter and use `editorRowInserChar()` to insert
// that character into the position that the cursor is at
// --------------------------------------------------------
void editorInsertChar(int c)
{
    // If `E.cy == E.numrows`, then the cursor is on the tilde line after the
    // end of the file, so we need to append a new row to the file before
    // inserting a character there.
    if(E.cy == E.numrows)
    {
        editorInsertRow(E.numrows, "", 0);
    }

    editorRowInsertChar(&E.row[E.cy], E.cx, c);
    ++E.cx;
}

// ---------------
// Insert new line
// ---------------
void editorInsertNewLine()
{
    if(E.cx == 0)
    {
        editorInsertRow(E.cy, "", 0);
    }
    else
    {
        // Move the characters on the right of the cursor to the new row
        erow* row = &E.row[E.cy];
        editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
        row = &E.row[E.cy];
        row->size = E.cx;
        row->chars[row->size] = '\0';
        editorUpdateRow(row);
    }

    ++E.cy;
    E.cx = 0;
}

// ------------------------------------------------------
// Delete the character that is to the left of the cursor
// ------------------------------------------------------
void editorDelChar()
{
    // If the cursor is past the end of the file, we return immediately
    if(E.cy == E.numrows)
        return;

    // If the cursor is at the beginning of a line
    if(E.cx == 0 && E.cy == 0)
        return;
    
    erow* row = &E.row[E.cy];

    if(E.cx > 0)    // Not first character of row
    {
        editorRowDelChar(row, E.cx - 1);
        --E.cx;
    }
    else    // First character of row
    {
        E.cx = E.row[E.cy - 1].size;
        editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
        editorDelRow(E.cy);
        --E.cy;
    }
}

/*** File I/O ***/

// ---------------------------------------------------------
// Converts our array of `erow` structs into a single string
// that is ready to be written out to a file
// ---------------------------------------------------------
char* editorRowsToString(int* buflen)
{
    // Add up the lengths of each row of text
    int totlen = 0;
    int j;
    for(j = 0; j < E.numrows; ++j)
    {
        totlen += E.row[j].size + 1;
    }
    *buflen = totlen;

    // Allocate the required memory
    char* buf = malloc(totlen);
    char* p = buf;

    // Loop through the rows and `memcpy()` the contents of each row to the
    // end of the buffer, appending a newline character after each row.
    for(j = 0; j < E.numrows; ++j)
    {
        memcpy(p, E.row[j].chars, E.row[j].size);
        p += E.row[j].size;
        *p = '\n';
        ++p;
    }

    return buf;
}

// ---------
// Open file
// ---------
void editorOpen(char* filename)
{
    // Get name of file
    free(E.filename);
    E.filename = strdup(filename);      // `strdup()` makes copy of string

    FILE *fp = fopen(filename, "r");
    if(!fp)
        die("fopen");

    char* line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    linelen = getline(&line, &linecap, fp);

    // Read all lines of text from file
    while((linelen = getline(&line, &linecap, fp)) != -1)
    {
        while(linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
        {
            --linelen;
        }

        editorInsertRow(E.numrows, line, linelen);
    }

    free(line);
    fclose(fp);
    E.dirty = 0;
}

// -----------------------------------------------------------
// Write the string returned by `editorRowsToString()` to disk
// -----------------------------------------------------------
void editorSave()
{
    // New file
    if(E.filename == NULL)
    {
        E.filename = editorPrompt("Save as : %s (ESC to cancel)");
        if(E.filename == NULL)
        {
            editorSetStatusMessage("Save aborted");
            return;
        }
    }

    int len;
    char* buf = editorRowsToString(&len);

    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);      // 0644 is the standard permissions for text files
    
    // Error handling for file
    if(fd != -1)
    {
        if(ftruncate(fd, len) != -1)
        {
            if(write(fd, buf, len) == len)
            {
                close(fd);
                free(buf);
                E.dirty = 0;
                editorSetStatusMessage("%d bytes written to disk", len);
                return;
            }
        }
        close(fd);
    }
    free(buf);
    editorSetStatusMessage("Can't save! I/O error : %s", strerror(errno));
}

/*** Append Buffer ***/

// --------------------------------------------------------
// "Dynamic string" that supports one operation : appending
// --------------------------------------------------------
struct abuf
{
    char* b;
    int len;
};

#define ABUF_INIT {NULL, 0}

// ------------------------------
// Append to our "Dynamic String"
// ------------------------------
void abAppend(struct abuf* ab, const char* s, int len)
{
    char* new = realloc(ab->b, ab->len + len);

    if(new == NULL)
        return;

    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

// --------------------------------------
// Deallocates memory of "Dynamic String"
// --------------------------------------
void abFree(struct abuf* ab)
{
    free(ab->b);
}

/*** Output ***/

// ---------------------------------
// Enable Scrolling Through The Text
// ---------------------------------
void editorScroll()
{
    // Convert `chars` index to `render` index
    E.rx = 0;
    if(E.cy < E.numrows)
    {
        E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
    }

    // Vertical scrolling
    // ------------------
    if(E.cy < E.rowoff)
    {
        E.rowoff = E.cy;
    }
    if(E.cy >= E.rowoff + E.screenrows)
    {
        E.rowoff = E.cy - E.screenrows + 1;
    }

    // Horizontal scrolling
    // --------------------
    if(E.rx < E.coloff)
    {
        E.coloff = E.rx;
    }
    if(E.rx >= E.coloff + E.screencols)
    {
        E.coloff = E.rx - E.screencols + 1;
    }
}

// --------------------------
// Draw Tilde On Left of Rows
// --------------------------
void editorDrawRows(struct abuf *ab)
{
    int y;
    for(y = 0; y < E.screenrows; ++y)
    {
        // Display the correct range of lines of the file according to the value of `rowoff`
        int filerow = y + E.rowoff;
        if(filerow >= E.numrows)
        {
            // Draw welcome message on when use start the program with no arguments
            if (E.numrows == 0 && y == E.screenrows / 3)
            {
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome), "Atto editor -- version %s", ATTO_VERSION);

                if(welcomelen > E.screencols)
                    welcomelen = E.screencols;

                int padding = (E.screencols - welcomelen) / 2;

                if(padding)
                {
                    abAppend(ab, "~", 1);
                    --padding;
                }

                while(padding--)
                {
                    abAppend(ab, " ", 1);
                }

                abAppend(ab, welcome, welcomelen);
            }
            else
            {
                // Add tilde to rows
                abAppend(ab, "~", 1);
            }
        }
        else
        {
            // Append text from opened file as rows to terminal
            int len = E.row[filerow].rsize - E.coloff;
            if(len < 0)
                len = 0;
            if(len > E.screencols)
                len = E.screencols;
            
            abAppend(ab, &E.row[filerow].render[E.coloff], len);
        }


        // Clear each line as we redraw
        abAppend(ab, "\x1b[K", 3);

        // Append newline to every line
        abAppend(ab, "\r\n", 2);
    }
}

// ---------------
// Draw Status Bar
// ---------------
void editorDrawStatusBar(struct abuf* ab)
{
    abAppend(ab, "\x1b[7m", 4);
    char status[80], rstatus[80];

    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
                       E.filename ? E.filename : "[No Name]", E.numrows, E.dirty ? "(modified)" : "");

    int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", E.cy + 1, E.numrows);

    if(len > E.screencols)
    {
        len = E.screencols;
    }

    abAppend(ab, status, len);

    while(len < E.screencols)
    {
        if(E.screencols - len == rlen)
        {
            abAppend(ab, rstatus, rlen);
            break;
        }
        else
        {
            abAppend(ab, " ", 1);
            ++len;
        }
    }
    abAppend(ab, "\x1b[m", 3);
    abAppend(ab, "\r\n", 2);
}

// -----------------------------------------
// Draw the message bar below the status bar
// -----------------------------------------
void editorDrawMessageBar(struct abuf* ab)
{
    abAppend(ab, "\x1b[K", 3);

    int msglen = strlen(E.statusmsg);

    if(msglen > E.screencols)
        msglen = E.screencols;

    if(msglen && time(NULL) - E.statusmsg_time < 5)
        abAppend(ab, E.statusmsg, msglen);
}

// ------------
// Clear Screen
// ------------
void editorRefreshScreen()
{
    // Enable scrolling
    editorScroll();

    // Create our "Dynamic String"
    struct abuf ab = ABUF_INIT;

    // Hide the cursor while repainting
    abAppend(&ab, "\x1b[?25l", 6);

    // The 4 in `write()` means we are writing 4 bytes out to the terminal.
    // The first byte is "\x1b", which is the escape character, or 27 in decimal.
    // Escape sequences always start with an escape character (27) followed by '['.
    // We are using the 'J' command (Erase In Display) to clear the screen. In this case,
    // the argument is 2, which says to clear the entire screen.
    // "<esc>[1J" would clear the screen up to where the cursor is.
    // "<esc>[0J" would clear the screen from the cursor up to the end of the screen.
    // abAppend(&ab, "\x1b[2J", 4);         // No longer clearing entire screen. Now clearing line-by-line

    // Reposition the cursor to the top-left corner.
    // The 'H' command actually takes in two arguments.
    // For example, we could use "<esc>[12;40H".
    abAppend(&ab, "\x1b[H", 3);

    // Draw rows with tilde
    editorDrawRows(&ab);

    // Draw status bar
    editorDrawStatusBar(&ab);

    // Draw message bar
    editorDrawMessageBar(&ab);

    // Move cursor to position stored in `E.cx` and `E.cy`
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, (E.rx - E.coloff) + 1);
    abAppend(&ab, buf, strlen(buf));

    // Show the cursor after repainting
    abAppend(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

// ------------------
// Set status message
// ------------------
void editorSetStatusMessage(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}

/*** Input ***/

// -----------------------------------
// Displays a prompt in the status bar
// -----------------------------------
char* editorPrompt(char* prompt)
{
    size_t bufsize = 128;
    char* buf = malloc(bufsize);

    size_t buflen = 0;
    buf[0] = '\0';

    while(1)
    {
        editorSetStatusMessage(prompt, buf);
        editorRefreshScreen();

        int c = editorReadKey();

        // User presses BACKSPACE
        if(c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE)
        {
            if(buflen != 0)
                buf[--buflen] = '\0';
        }
        // User presses ESC
        else if(c == '\x1b')
        {
            editorSetStatusMessage("");
            free(buf);
            return NULL;
        }
        // User presses ENTER
        else if(c == '\r')
        {
            if(buflen != 0)
            {
                editorSetStatusMessage("");
                return buf;
            }
        }
        // User types in alphanumeric
        else if(!iscntrl(c) && c < 128)
        {
            if(buflen == bufsize - 1)
            {
                bufsize *= 2;
                buf = realloc(buf, bufsize);
            }
            buf[buflen++] = c;
            buf[buflen] = '\0';
        }
    }
}

// -----------
// Move cursor
// -----------
void editorMoveCursor(int key)
{
    // Limit horizontal right scrolling to the last character in row
    erow* row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

    switch(key)
    {
        case ARROW_LEFT:
            if(E.cx != 0)
            {
                // Move left
                --E.cx;
            }
            else if(E.cy > 0)
            {
                // Move to end of previous line if at start of current line
                --E.cy;
                E.cx = E.row[E.cy].size;
            }
            break;
        case ARROW_RIGHT:
            if(row && E.cx < row->size)
            {
                // Move right
                ++E.cx;
            }
            else if(row && E.cx == row->size)
            {
                // Move to start of next line if at end of current line
                ++E.cy;
                E.cx = 0;
            }
            break;
        case ARROW_UP:
            if(E.cy != 0)
            {
                --E.cy;
            }
            break;
        case ARROW_DOWN:
            if(E.cy < E.numrows)
            {
                ++E.cy;
            }
            break;
    }

    // Snap cursor to end of line
    // --------------------------
    row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    int rowlen = row ? row->size : 0;
    if(E.cx > rowlen)
    {
        E.cx = rowlen;
    }
}

// ------------------------------
// Process Read Key Into Commands
// ------------------------------
void editorProcessKeypress()
{
    static int quit_times = ATTO_QUIT_TIMES;

    int c = editorReadKey();

    switch(c)
    {
        // ENTER key
        case '\r':
            editorInsertNewLine();
            break;

        // Exit
        case CTRL_KEY('q'):
            if(E.dirty && quit_times > 0)
            {
                editorSetStatusMessage("WARNING!!! File has unsaved changes. "
                                       "Press Ctrl-Q %d more times to quit.", quit_times);
                --quit_times;
                return;
            }
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;

        // Save
        case CTRL_KEY('s'):
            editorSave();
            break;

        // HOME button moves cursor to first column of row
        case HOME_KEY:
            E.cx = 0;
            break;
        
        // END button moves cursor to last column of row
        case END_KEY:
            if(E.cy < E.numrows)
            {
                E.cx = E.row[E.cy].size;
            }
            break;

        case BACKSPACE:
        case CTRL_KEY('h'):
        case DEL_KEY:
            if(c == DEL_KEY)
                editorMoveCursor(ARROW_RIGHT);
            editorDelChar();
            break;

        // PAGE_UP and PAGE_DOWN
        case PAGE_UP:
        case PAGE_DOWN:
            {
                // Scrolling with PAGE_UP and PAGE_DOWN
                if(c == PAGE_UP)
                {
                    E.cy = E.rowoff;
                }
                else if(c == PAGE_DOWN)
                {
                    E.cy = E.rowoff + E.screenrows - 1;
                    if(E.cy > E.numrows)
                        E.cy = E.numrows;
                }

                int times = E.screenrows;
                while (times--)
                {
                    editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
                }
            }
            
            break;
        
        // Cursor movement
        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editorMoveCursor(c);
            break;

        // (Ctrl + l) and ESC key
        case CTRL_KEY('l'):
        case '\x1b':
            break;

        default:
            editorInsertChar(c);
            break;
    }

    quit_times = ATTO_QUIT_TIMES;
}

/*** Init ***/

// --------------------------
// Initialize The Text Editor
// --------------------------
void initEditor()
{
    // Set initial cursor location
    E.cx = 0;
    E.cy = 0;

    // Set initial cursor location on render
    E.rx = 0;

    // Set row and column offset for scrolling
    E.rowoff = 0;
    E.coloff = 0;

    // Rows of text from file
    E.numrows = 0;
    E.row = NULL;

    // Dirty flag (unsaved changes)
    E.dirty = 0;

    // Name of file
    E.filename = NULL;

    // Status message
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;

    if(getWindowSize(&E.screenrows, &E.screencols) == -1)
    {
        die("getWindowSize");
    }

    // Last row excluded for status bar
    E.screenrows -= 2;
}

int main(int argc, char* argv[])
{
    // Set terminal to raw mode from canonical mode
    enableRawMode();

    // Initialize editor
    initEditor();

    // Open file
    if(argc >= 2)
    {
        editorOpen(argv[1]);
    }

    editorSetStatusMessage("HELP : Ctrl-S = save | Ctrl-Q = quit");

    while(1)
    {
        editorRefreshScreen();
        editorProcessKeypress();
    }
    return 0;
}