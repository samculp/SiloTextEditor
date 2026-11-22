/*** includes ***/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <unistd.h>
#include <termios.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <stdarg.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <sys/types.h>
#include <errno.h>
#include <time.h>

/*** defines ***/

#define SILO_VERSION "0.0.1"
#define SILO_TAB_STOP 4
#define CTRL_KEY(k) ((k) & 0x1f) // strips letters to their Ctrl+letter ASCII value

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

// represents one row of text
typedef struct erow {
	int size;
	int rend_size;
	char* chars;
	char* render;
} erow;

// global editor state
struct editorConfig {
	int cx, cy; // cursor position in text file
	int rx; // position of cursor in rendered line (if there are tabs, rx will be greater than cx)
	int row_offset;
	int col_offset;
	int screen_rows;
	int screen_cols;
	int num_text_rows;
	erow* text_rows;
	char* file_name;
	char status_msg[80];
	time_t status_msg_time;
	struct termios orig_termios;
};

struct editorConfig E;

/*** terminal ***/

void die(const char* s) {
	write(STDOUT_FILENO, "\x1b[2J", 4); // clear screen
	write(STDOUT_FILENO, "\x1b[H", 3);  // move cursor to top left

	perror(s);
	exit(1);
}

void disableRawMode() {
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
		die("tcsetattr");
}

void enableRawMode() {
	if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)
		die("tcgetattr");
	atexit(disableRawMode);

	struct termios raw = E.orig_termios;
	raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
	raw.c_oflag = ~(OPOST);
	raw.c_cflag &= ~(CS8);
	raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 1;

	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
		die("tcsetattr");
}

int editorReadKey() {
	int nread;
	char c;
	while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
		if (nread == -1 && errno != EAGAIN)
			die("read");
	}

	if (c == '\x1b') { // process escape sequences
		char seq[3];

		if (read(STDIN_FILENO, &seq[0], 1) != 1)
			return '\x1b';
		if (read(STDIN_FILENO, &seq[1], 1) != 1) 
			return '\x1b';

		if (seq[0] == '[') {

			if (seq[1] >= '0' && seq[1] <= '9') {
				if (read(STDIN_FILENO, &seq[2], 1) != 1)
					return '\x1b';
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
			}
			else {
				switch (seq[1]) {
					case 'A': return ARROW_UP;
					case 'B': return ARROW_DOWN;
					case 'C': return ARROW_RIGHT;
					case 'D': return ARROW_LEFT;
					case 'H': return HOME_KEY;
					case 'F': return END_KEY;
				}
			}
		}
		else if (seq[0] == 'O') {
			switch (seq[1]) {
				case 'H': return HOME_KEY;
				case 'F': return END_KEY;
			}
		}

		return '\x1b';
	}
	else {
		return c;
	}
}

int getCursorPosition(int* rows, int* cols) {
	char buf[32];
	unsigned int i = 0;

	if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) // 'n' is used to query terminal for status information. 6 asks for cursor position
		return -1;
	
	while (i < sizeof(buf) - 1) {
		if (read(STDIN_FILENO, &buf[i], 1) != 1)
			break;
		if (buf[i] == 'R')
			break;
		i++;
	}

	buf[i] = '\0';

	if (buf[0] != '\x1b' || buf[1] != '[')
		return -1;
	if (sscanf(&buf[2], "%d;%d", rows, cols) != 2)
		return -1;

	return 0;
}

int getWindowSize(int* rows, int* cols) {
	struct winsize ws;

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
		if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
			return -1;
		return getCursorPosition(rows, cols);
	}
	else {
		*cols = ws.ws_col;
		*rows = ws.ws_row;
		return 0;
	}
}

/*** row operations ***/

int editorRowCxToRx(erow* row, int cx) {
	int rx = 0;
	int j;
	for (j = 0; j < cx; j++) {
		if (row->chars[j] == '\t') {
			// subtract however many columns we are from the last tab stop from TAB_STOP - 1 to find out
			// how many columns we are to the left of the next tab stop
			// gets us just to the left of the tab stop
			rx += (SILO_TAB_STOP - 1) - (rx % SILO_TAB_STOP);
		}
		// gets us just to the right of the next tab stop
		rx++;
	}
	return rx;
}

void editorUpdateRow(erow* row) {
	int tabs = 0;
	int j;
	for (j = 0; j < row->size; j++)
		if (row->chars[j] == '\t')
			tabs++;

	free(row->render);
	row->render = malloc(row->size + tabs*(SILO_TAB_STOP-1) + 1);

	int idx = 0;
	for (j = 0; j < row->size; j++) {
		if (row->chars[j] == '\t') {
			row->render[idx++] = ' ';
			while (idx % SILO_TAB_STOP != 0)
				row->render[idx++] = ' '; 
		}
		else {
			row->render[idx++] = row->chars[j];
		}
	}

	row->render[idx] = '\0';
	row->rend_size = idx;
}

void editorAppendRow(char* s, size_t len) {
	E.text_rows = realloc(E.text_rows, sizeof(erow) * (E.num_text_rows + 1));

	int at = E.num_text_rows;
	E.text_rows[at].size = len;
	E.text_rows[at].chars = malloc(len + 1);
	memcpy(E.text_rows[at].chars, s, len);
	E.text_rows[at].chars[len] = '\0';

	E.text_rows[at].rend_size = 0;
	E.text_rows[at].render = NULL;
	editorUpdateRow(&E.text_rows[at]);

	E.num_text_rows++;
}

/*** file i/o ***/

void editorOpen(char* filename) {
	free(E.file_name);
	E.file_name = strdup(filename);

	FILE* fp = fopen(filename, "r");
	if (!fp) 
		die("fopen");

	char* line = NULL;
	size_t line_cap = 0;
	ssize_t line_len;

	while ((line_len = getline(&line, &line_cap, fp)) != -1) {
		while (line_len > 0 && (line[line_len - 1] == '\n' || line[line_len - 1] == '\r'))
			line_len--;
		editorAppendRow(line, line_len);
	}

	free(line);
	fclose(fp);
}

/*** append buffer ***/

struct abuf {
	char* b;
	int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf* ab, const char* s, int len) {
	char* new = realloc(ab->b, ab->len + len);
	if (new == NULL)
		return;
	memcpy(&new[ab->len], s, len);
	ab->b = new;
	ab->len += len;
}

void abFree(struct abuf* ab) {
	free(ab->b);
}

/*** output ***/

void editorScroll() {
	E.rx = 0;
	if (E.cy < E.num_text_rows) {
		E.rx = editorRowCxToRx(&E.text_rows[E.cy], E.cx);
	}

	if (E.cy < E.row_offset)
		E.row_offset = E.cy;
	if (E.cy >= E.row_offset + E.screen_rows)
		E.row_offset = E.cy - E.screen_rows + 1;
	if (E.rx < E.col_offset)
		E.col_offset = E.rx;
	if (E.rx >= E.col_offset + E.screen_cols)
		E.col_offset = E.rx - E.screen_cols + 1;
}

void editorDrawRows(struct abuf* ab) {
	int y;

	// iterate over each visible console row
	for (y = 0; y < E.screen_rows; y++) {
		int file_row = y + E.row_offset;

		// if we have surpassed the end of the text file
		if (file_row >= E.num_text_rows) {

			// print welcome message or tilda
			if (E.num_text_rows == 0 && y == E.screen_rows / 3) {
				char welcome[80];
				int welcome_len = snprintf(welcome, sizeof(welcome), "Silo Editor -- v%s", SILO_VERSION);
				if (welcome_len > E.screen_cols)
					welcome_len = E.screen_cols;
				int padding = (E.screen_cols - welcome_len) / 2;
				if (padding) {
					abAppend(ab, "~", 1);
					padding--;
				}
				while (padding--)
					abAppend(ab, " ", 1);
				abAppend(ab, welcome, welcome_len);
			}
			else
				abAppend(ab, "~", 1);
		}
		// otherwise, print file contents
		else {
			int len = E.text_rows[file_row].rend_size - E.col_offset;
			if (len < 0) len = 0;
			if (len > E.screen_cols) len = E.screen_cols;
			abAppend(ab, &E.text_rows[file_row].render[E.col_offset], len);
		}

		abAppend(ab, "\x1b[K", 3); // erase part of the current line to right of cursor
		abAppend(ab, "\r\n", 2); // return carriage and newline
	}
}

void editorDrawStatusBar(struct abuf* ab) {
	abAppend(ab, "\x1b[7m", 4);

	char status[80], rstatus[80];

	int len = snprintf(status, sizeof(status), " %.20s - %d lines",
		E.file_name ? E.file_name : "[No Name]", E.num_text_rows);

	int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d",
		E.cy + 1, E.num_text_rows);

	if (len > E.screen_cols)
		len = E.screen_cols;
	abAppend(ab, status, len);

	while (len < E.screen_cols) {
		if (E.screen_cols - len - 1 == rlen) {
			abAppend(ab, rstatus, rlen);
			abAppend(ab, " ", 1);
			break;
		}
		else {
			abAppend(ab, " ", 1);
			len++;
		}
	}

	abAppend(ab, "\x1b[m", 3);
	abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf* ab) {
	abAppend(ab, "\x1b[K", 3);
	int msglen = strlen(E.status_msg);
	if (msglen > E.screen_cols)
		msglen = E.screen_cols;
	if (msglen && time(NULL) - E.status_msg_time < 5)
		abAppend(ab, E.status_msg, msglen);
}

void editorRefreshScreen() {
	editorScroll();

	struct abuf ab = ABUF_INIT;

	abAppend(&ab, "\x1b[?25l", 6); // hide cursor
	abAppend(&ab, "\x1b[H", 3); // move cursor to top left

	editorDrawRows(&ab);
	editorDrawStatusBar(&ab);
	editorDrawMessageBar(&ab);

	// move cursor to stored position
	char buf[32];
	snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.row_offset) + 1, (E.rx - E.col_offset) + 1);
	abAppend(&ab, buf, strlen(buf));

	abAppend(&ab, "\x1b[?25h", 6); // show cursor

	write(STDOUT_FILENO, ab.b, ab.len);
	abFree(&ab);
}

void editorSetStatusMessage(const char* fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(E.status_msg, sizeof(E.status_msg), fmt, ap);
	va_end(ap);
	E.status_msg_time = time(NULL);
}

/*** input ***/

void editorMoveCursor(int key) {
	erow* row = (E.cy >= E.num_text_rows) ? NULL : &E.text_rows[E.cy];

	switch(key) {
		case ARROW_LEFT:
			if (E.cx != 0)
				E.cx--;
			else if (E.cy > 0) {
				E.cy--;
				E.cx = E.text_rows[E.cy].size;
			}
			break;
		case ARROW_RIGHT:
			if (row && E.cx < row->size)
				E.cx++;
			else if (row && E.cx == row->size) {
				E.cy++;
				E.cx = 0;
			}
			break;
		case ARROW_UP:
			if (E.cy != 0)
				E.cy--;
			break;
		case ARROW_DOWN:
			if (E.cy < E.num_text_rows)
				E.cy++;
			break;
	}

	// snap cursor to the end of a line
	row = (E.cy >= E.num_text_rows) ? NULL : &E.text_rows[E.cy];
	int row_len = row ? row->size : 0;
	if (E.cx > row_len)
		E.cx = row_len;
}

void editorProcessKeypress() {
	int c = editorReadKey();

	switch (c) {
		case CTRL_KEY('q'):
			write(STDOUT_FILENO, "\x1b[2J]", 4); // clear screen
			write(STDOUT_FILENO, "\x1b[H", 3);  // move cursor to top left
			exit(0);
			break;

		case HOME_KEY:
			E.cx = 0;
			break;
		case END_KEY:
			if (E.cy < E.num_text_rows)
				E.cx = E.text_rows[E.cy].size;
			break;

		case PAGE_UP:
		case PAGE_DOWN:
			{
				if (c == PAGE_UP) {
					E.cy = E.row_offset;
				}
				else if (c == PAGE_DOWN) {
					E.cy = E.row_offset + E.screen_rows - 1;
					if (E.cy > E.num_text_rows)
						E.cy = E.num_text_rows;
				}

				int times = E.screen_rows;
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
	E.cx = 0;
	E.cy = 0;
	E.rx = 0;
	E.row_offset = 0;
	E.col_offset = 0;
	E.num_text_rows = 0;
	E.text_rows = NULL;
	E.file_name = NULL;
	E.status_msg[0] = '\0';
	E.status_msg_time = 0;

	if (getWindowSize(&E.screen_rows, &E.screen_cols) == -1)
		die("getWindowSize");
	E.screen_rows -= 2;
}

int main(int argc, char* argv[]) {
	enableRawMode();
	initEditor();
	if (argc >= 2) {
		editorOpen(argv[1]);
	}

	editorSetStatusMessage("HELP: Ctrl-Q = quit");

	while (1) {
		editorRefreshScreen();
		editorProcessKeypress();
	}

	return 0;
}

