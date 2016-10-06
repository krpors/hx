#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>

#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>

// Declarations.

/**
 * This struct contains internal information of the state of the editor.
 */
struct editor {
	int octets_per_line;
	int grouping;

	int cursor_x;    // Cursor x pos
	int cursor_y;    // Cursor y pos
	int screen_rows; // amount of screen rows after init
	int screen_cols; // amount of screen columns after init

	bool  dirty;          // whether the buffer is modified
	char* filename;       // the filename currently open
	char* contents;       // the file's contents
	int   content_length; // length of the contents

	char statusmessage[80];  // status message
};

// Functions:
void enable_raw_mode();
void disable_raw_mode();
int read_key();
void process_keypress(struct editor* ec);

struct editor* editor_init();
void           editor_free(struct editor* ec);
int            editor_statusmessage(struct editor* ec, const char* fmt, ...);

// Global editor config.
struct editor* ec;

// Terminal IO settings. Used to reset it when exiting to prevent terminal
// data garbling. Declared global since we require it in the atexit() call.
struct termios orig_termios;

// Key enumerations
enum KEY_CODES {
	KEY_NULL     = 0,
	KEY_CTRL_Q   = 0x11, // DC1, to exit the program.
	KEY_ESC      = 0x1b, // ESC, for things like keys up, down, left, right, delete, ...

	// 'Virtual keys', i.e. not corresponding to terminal escape sequences
	// or any other ANSI stuff. Merely to identify keys returned by read_key().
	KEY_UP      = 1000, // [A
	KEY_DOWN,           // [B
	KEY_RIGHT,          // [C
	KEY_LEFT,           // [D
	KEY_HOME,           // [H
	KEY_END,            // [F
	KEY_PAGEUP,         // ??
};

void enable_raw_mode() {
	// only enable raw mode when stdin is a tty.
	if (!isatty(STDIN_FILENO)) {
		perror("Input is not a TTY");
		exit(1);
	}

	// Disable raw mode when we exit hx normally.
	atexit(disable_raw_mode);

	tcgetattr(STDIN_FILENO, &orig_termios);

	struct termios raw = orig_termios;
	// input modes: no break, no CR to NL, no parity check, no strip char,
	// no start/stop output control.
	raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
	// output modes - disable post processing
	raw.c_oflag &= ~(OPOST);
	// control modes - set 8 bit chars
	raw.c_cflag |= (CS8);
	// local modes - choing off, canonical off, no extended functions,
	// no signal chars (^Z,^C)
	raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
	// control chars - set return condition: min number of bytes and timer.
	// Return each byte, or zero for timeout.
	raw.c_cc[VMIN] = 0;
	// 100 ms timeout (unit is tens of second).
	raw.c_cc[VTIME] = 1;

    // put terminal in raw mode after flushing
	int x = tcsetattr(STDIN_FILENO, TCSANOW, &raw);
	if (x != 0) {
		perror("Unable to set terminal attributes");
		exit(1);
	}
}

void disable_raw_mode() {
	editor_free(ec);
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
	printf("\x1b[2J\x1b[7mThank you for using hx!\x1b[0K\r\n");
	fflush(stdout);
}


void clear_screen() {
	if (write(STDOUT_FILENO, "\x1b[2J", 4) == -1) {
		perror("Unable to clear screen");
	}
}

void move_cursor(int dir) {
	static const int LEN = 4;
	char cruft[LEN];

	switch (dir) {
	case KEY_UP:    strncpy(cruft, "\x1b[1A", LEN); break;
	case KEY_RIGHT: strncpy(cruft, "\x1b[1C", LEN); break;
	case KEY_DOWN:  strncpy(cruft, "\x1b[1B", LEN); break;
	case KEY_LEFT:  strncpy(cruft, "\x1b[1D", LEN); break;
	default: return;
	}

	if (write(STDOUT_FILENO, cruft, 4) == -1) {
		perror("Cant write to stdout??");
	}
}

bool get_window_size(int* rows, int* cols) {
	struct winsize ws;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0) {
		perror("Failed to query terminal size");
		exit(1);
	}

	*rows = ws.ws_row;
	*cols = ws.ws_col;
	return true;
}

/**
 * This buffer contains the character sequences to render the current
 * 'screen'. The buffer is changed as a whole, then written to the screen
 * in one go to prevent 'flickering' in the terminal.
 */
struct buffer {
	char* contents;
	int len;
};

/**
 * Create a buffer on the heap and return it.
 */
struct buffer* buffer_create() {
	struct buffer* b = malloc(sizeof(struct buffer));
	if (b) {
		b->contents = NULL;
		b->len = 0;
		return b;
	} else {
		perror("Unable to allocate size for struct buffer");
		exit(1);
	}
}

/**
 * Deletes the buffer's contents, and the buffer itself.
 */
void buffer_free(struct buffer* buf) {
	free(buf->contents);
	free(buf);
}

/**
 * Appends `what' to the buffer, writing at most `len' bytes. Note that
 * if we use snprintf() to format a particular string, we have to subtract
 * 1 from the `len', to discard the null terminator character.
 */
void buffer_append(struct buffer* buf, const char* what, size_t len) {
	assert(what != NULL);

	// reallocate the contents with more memory, to hold 'what'.
	char* new = realloc(buf->contents, buf->len + len);
	if (new == NULL) {
		perror("Unable to realloc buffer");
		exit(1);
	}

	// copy 'what' to the target memory
	memcpy(new + buf->len, what, len);
	buf->contents = new;
	buf->len += len;
}

/**
 * Draws (writes) the buffer to the screen.
 */
void buffer_draw(struct buffer* buf) {
	if (write(STDOUT_FILENO, buf->contents, buf->len) == -1) {
		perror("Can't write buffer");
		exit(1);
	}
}

/**
 * Opens a file denoted by `filename', or exit if the file cannot be opened.
 * The editor struct is used to contain the contents and other metadata
 * about the file being opened.
 */
void editor_openfile(struct editor* ec, const char* filename) {
	FILE* fp = fopen(filename, "rb");
	if (fp == NULL) {
		perror("Opening file");
		exit(1);
	}

	// go to the end of the file.
	fseek(fp, 0, SEEK_END);
	// determine file size
	long size = ftell(fp);
	// set the indicator to the start of the file
	fseek(fp, 0, SEEK_SET);

	printf("File size in bytes is %ld\n", size);
	fflush(stdout);

	// allocate memory for the buffer. No need for extra
	// room for a null string terminator, since we're possibly
	// reading binary data only anyway (which can contain 0x00).
	char* contents = malloc(sizeof(char) * size);

	if (fread(contents, size, 1, fp) <= 0) {
		perror("Unable to read bytes");
		free(contents);
		exit(1);
	}

	ec->filename = malloc(strlen(filename));
	strncpy(ec->filename, filename, strlen(filename));
	ec->contents = contents;
	ec->content_length = size;
	editor_statusmessage(ec, "File opened: %s (%d bytes)", ec->filename, ec->content_length);

	fclose(fp);
}

int editor_statusmessage(struct editor* e, const char* fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	int x = vsnprintf(e->statusmessage, sizeof(e->statusmessage), fmt, ap);
	va_end(ap);
	return x;
}

/**
 * Reads keypresses from stdin, and processes them accordingly. Escape sequences
 * will be read properly as well (e.g. DEL will be the bytes 0x1b, 0x5b, 0x33, 0x7e).
 * The returned integer will contain either one of the enum values, or the key pressed.
 *
 * read_key() will only return the correct key code.
 */
int read_key() {
	char c;
	ssize_t nread;
	// check == 0 to see if EOF.
	while ((nread = read(STDIN_FILENO, &c, 1)) == 0);
	if (nread == -1) {
		perror("Unable to read from stdin");
		exit(2);
	}

	char seq[4]; // escape sequence buffer.

	switch (c) {
	case KEY_ESC:
		// Escape key was pressed, OR things like delete, arrow keys, ...
		// So we will try to read ahead a few bytes, and see if there's more.
		// For instance, a single Escape key only produces a single 0x1b char.
		// A delete key produces 0x1b 0x5b 0x33 0x7e.
		if (read(STDIN_FILENO, seq, 1) == 0) {
			return KEY_ESC;
		}
		if (read(STDIN_FILENO, seq + 1, 1) == 0) {
			return KEY_ESC;
		}

		if (seq[0] == '[') {
			switch (seq[1]) {
			case 'A': return KEY_UP;
			case 'B': return KEY_DOWN;
			case 'C': return KEY_RIGHT;
			case 'D': return KEY_LEFT;
			case 'H': return KEY_HOME;
			case 'F': return KEY_END;
			}
		}
		break;
	}

	//write(STDOUT_FILENO, &c, 1);
	//printf("%x\n", (unsigned char)c);
	return c;
}

/**
 * Renders the contents of the current state of the editor `e'
 * to the buffer `b'.
 *
 * TODO: parameterize things like grouping of bytes and whatnot,
 * correctly! I.e. malloc the padding size dynamically based on
 * the width.
 */
void render_contents(struct editor* e, struct buffer* b) {
	char address[80];  // example: 000000040
	char hex[ 2 + 1];  // example: 65
	char asc[256 + 1]; // example: Hello.World!

	// Counter to indicate how many chars have been written for the current
	// row of data. This is used for later for padding, when the iteration
	// is over, but there's still some ASCII to write.
	int row_char_count = 0;

	int offset;
	int start_offset = e->cursor_y * ec->octets_per_line;
	if (start_offset > ec->content_length) {
		start_offset = ec->content_length - ec->octets_per_line;
	}
	int bytes_per_screen = e->screen_rows * ec->octets_per_line - ec->octets_per_line;
	int end_offset = bytes_per_screen + start_offset;
	if (end_offset > ec->content_length) {
		end_offset = ec->content_length;
	}

	for (offset = start_offset; offset < end_offset; offset++) {
		if (offset % ec->octets_per_line == 0) {
			int bwritten = snprintf(address, sizeof(address), "\e[0;33m%09x\e[0m:", offset);
			buffer_append(b, address, bwritten);
			memset(asc, 0, sizeof(asc));
			row_char_count = 0;
		}

		snprintf(hex, sizeof(hex), "%02x", (unsigned char) e->contents[offset]);

		// Every iteration, set the ascii value in the buffer, until
		// 16 bytes are set. This will be written later when the hex
		// values are drawn to screen.
		if (isprint(e->contents[offset])) {
			asc[offset % ec->octets_per_line] = e->contents[offset];
		} else {
			asc[offset % ec->octets_per_line] = '.';
		}

		if (offset % ec->grouping == 0) {
			buffer_append(b, " ", 1);
			row_char_count++;
		}

		// First, write the hex value of the byte at the current offset.
		buffer_append(b, hex, 2);
		row_char_count += 2;

		// If we reached the end of a 'row', start writing the ASCII equivalents
		// of the 'row', in a different color. Then hit CRLF to go to the next line.
		if ((offset+1) % ec->octets_per_line == 0) {
			buffer_append(b, "\e[1;32m", 8);
			buffer_append(b, "  ", 2);
			buffer_append(b, asc, strlen(asc));
			buffer_append(b, "\r\n", 2);
		}
	}

	// Check remainder of the last offset. If its bigger than zero,
	// we got a last line to write (ASCII only).
	if (offset % ec->octets_per_line > 0) {
		char padding[980]; // padding characters, to align the ASCII.
		memset(padding, ' ', sizeof(padding));
		buffer_append(b, padding, (ec->octets_per_line* 2) + (ec->octets_per_line / ec->grouping) - row_char_count);
		buffer_append(b, "\e[1;32m  ", 10);
		buffer_append(b, asc, strlen(asc));
	}

	// clear everything up until the end
	buffer_append(b, "\x1b[0J", 4);

	char debug[80] = {0};
	snprintf(debug, sizeof(debug), "\x1b[37m\x1b[0;80HRows: %d, start offset: %09x, end offset: %09x", ec->screen_rows, start_offset, end_offset);
	buffer_append(b, debug, sizeof(debug));
}

/**
 * Refreshes the screen. It uses a temporary buffer to write everything that's
 * eligible for display to an internal buffer, and then 'draws' it to the screen
 * in one call.
 */
void refresh_screen(struct editor* ec) {
	char buf[32]; // temp buffer for snprintf.
	int bw; // bytes written by snprintf.
	struct buffer* b = buffer_create();

	buffer_append(b, "\x1b[H", 3); // move the cursor top left

	render_contents(ec, b);

	// Move down to write the status message.
	bw = snprintf(buf, sizeof(buf), "\x1b[%d;0H", ec->screen_rows);
	buffer_append(b, buf, bw);
	// Change the color
	buffer_append(b, "\x1b[0;30;47m", 10);
	buffer_append(b, ec->statusmessage, strlen(ec->statusmessage));
	// Clear until the end of the line to the right
	//buffer_append(b, "\x1b[0K", 4);

	// Position cursor.
	bw = snprintf(buf, sizeof(buf), "\x1b[%d;%dH", 0, 0);
	buffer_append(b, buf, bw);

	buffer_draw(b);
	buffer_free(b);
}

void editor_insert(struct editor* ec, char x) {
	// We are inserting a single character. Reallocate memory to contain
	// this extra byte.
	ec->contents = realloc(ec->contents, ec->content_length + 1);
	// Set the last allocated index to the character x.
	ec->contents[ec->content_length] = x;
	// Increase the content length since we inserted a character.
	ec->content_length++;
}

/**
 * Processes a keypress accordingly.
 */
void process_keypress(struct editor* ec) {
	int c = read_key();
	if (c == KEY_CTRL_Q) {
		exit(0);
	} else if (c == KEY_UP) {
		if (ec->cursor_y > 0) {
			ec->cursor_y--;
		}
	} else if (c == KEY_DOWN) {
		ec->cursor_y++;
	} else if (c == KEY_RIGHT) {
	} else if (c == KEY_LEFT) {
	} else if (c == KEY_HOME) {
		ec->cursor_y -= 10;
	} else if (c == KEY_END) {
		ec->cursor_y += 10;
	} else {
		editor_insert(ec, (char) c);
	}

	editor_statusmessage(ec, "Row: %d", ec->cursor_y);
}

/**
 * Initializes editor struct with some default values.
 */
struct editor* editor_init() {
	struct editor* ec = malloc(sizeof(struct editor));

	ec->octets_per_line = 16;
	ec->grouping = 2;

	ec->cursor_x = 0;
	ec->cursor_y = 0;
	ec->filename = NULL;
	ec->contents = NULL;
	ec->content_length = 0;
	get_window_size(&(ec->screen_rows), &(ec->screen_cols));

	return ec;
}

void editor_free(struct editor* ec) {
	free(ec->filename);
	free(ec->contents);
	free(ec);
}

int main(int argc, char* argv[]) {
	if (argc != 2) {
		fprintf(stderr, "expected arg\n");
		exit(2);
	}

	// Editor configuration passed around.
	ec = editor_init();
	editor_openfile(ec, argv[1]);

	enable_raw_mode();
	clear_screen();

	while (true) {
		refresh_screen(ec);
		process_keypress(ec);
	}

	editor_free(ec);
	return EXIT_SUCCESS;
}

/*
0000000: 65 AF EF AA CC AA BB AA FF  A.....A.A.
*/
