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

	int hex_line_width;  // the width in chars of a hex line, including
	                     // grouping spaces.

	int line; // The 'line' in the editor. Used for scrolling.

	int cursor_x;    // Cursor x pos on the current screen
	int cursor_y;    // Cursor y pos on the current screen
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
void           editor_openfile(struct editor* e, const char* filename);
void           editor_writefile(struct editor* e);
void           editor_free(struct editor* ec);
int            editor_statusmessage(struct editor* ec, const char* fmt, ...);
void           editor_scroll(struct editor* e, int units);

// Global editor config.
struct editor* ec;

// Terminal IO settings. Used to reset it when exiting to prevent terminal
// data garbling. Declared global since we require it in the atexit() call.
struct termios orig_termios;

// Key enumerations
enum KEY_CODES {
	KEY_NULL     = 0,
	KEY_CTRL_Q   = 0x11, // DC1, to exit the program.
	KEY_CTRL_S   = 0x13, // DC2, to save the current buffer.
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
	KEY_PAGEDOWN,       // ??
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
	int x = tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
	if (x != 0) {
		perror("Unable to set terminal attributes");
		exit(1);
	}
}

void disable_raw_mode() {
	editor_free(ec);
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
	printf("\x1b[2J\x1b[7mThank you for using hx!\x1b[0m\x1b[0K\r\n");
	fflush(stdout);
}


void clear_screen() {
	if (write(STDOUT_FILENO, "\x1b[2J", 4) == -1) {
		perror("Unable to clear screen");
	}
}

/**
 * Moves the cursor. The terminal cursor positions are all 1-based, so we
 * take that into account. When we scroll past boundaries (left, right, up
 * and down) we react accordingly. Note that the cursor_x/y are also 1-based,
 * and we calculate the actual position of the hex values by incrementing it
 * later on with the address size, amount of grouping spaces etc.
 */
void editor_move_cursor(struct editor* e, int dir) {
	switch (dir) {
	case KEY_LEFT:
		e->cursor_x--;
		break;
	case KEY_RIGHT:
		e->cursor_x++;
		break;
	}

	if (e->cursor_x < 1) {
		// move up a row.
		e->cursor_x = 16;
		e->cursor_y--;
		if (e->cursor_y < 1) {
			e->cursor_y = 1;
			editor_scroll(e, -1);
		}
	} else if (e->cursor_x > 16) {
		// move down a row
		e->cursor_x = 1;
		e->cursor_y++;
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

	if (size <= 0) {
		// TODO: file size is empty, then what?
		printf("File is empty.\n");
		fflush(stdout);
		exit(0);
	}

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
	editor_statusmessage(ec, "\"%s\" (%d bytes)", ec->filename, ec->content_length);

	fclose(fp);
}

/**
 * Writes the contents of the editor's buffer the to the same filename.
 */
void editor_writefile(struct editor* e) {
	assert(e->filename != NULL);

	FILE* fp = fopen(e->filename, "w");
	if (fp == NULL) {
		editor_statusmessage(ec, "Unable to open '%s' for writing", e->filename);
		return;
	}

	size_t bw = fwrite(e->contents, sizeof(char), e->content_length, fp);
	if (bw <= 0) {
		editor_statusmessage(ec, "Couldn't write to file!!");
	}

	editor_statusmessage(ec, "\"%s\", %d bytes written", ec->filename, ec->content_length);

	fclose(fp);
}

/**
 * Scrolls the editor by updating the `line' accordingly, within
 * the bounds of the readable parts of the buffer.
 */
void editor_scroll(struct editor* e, int units) {
	e->line += units;

	// if our cursor goes beyond the lower limit (which is 0, duh)
	// then set our cursor position to just that.
	if (e->line <= 0) {
		e->line = 0;
	}

	// if our cursor goes beyond the upper limit, then set our cursor
	// to the max. Since we are displaying data in a sort of matrix form,
	// meaning (rows × columns), we have to calculate the upper limit of
	// the cursor.
	int upper_limit = e->content_length / e->octets_per_line;
	if (e->line >= upper_limit) {
		e->line = upper_limit;
	}
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

		// home = 0x1b, [ = 0x5b, 1 = 0x31, ~ = 0x7e,
		// end  = 0x1b, [ = 0x5b, 4 = 0x34, ~ = 0x7e,
		// pageup   1b, [=5b, 5=35, ~=7e,
		// pagedown 1b, [=5b, 6=36, ~=7e,

		if (seq[0] == '[') {
			if (seq[1] >= '0' && seq[1] <= '9') {
				if (read(STDIN_FILENO, seq + 2, 1) == 0) {
					return KEY_ESC;
				}
				if (seq[2] == '~') {
					switch (seq[1]) {
					case '1': return KEY_HOME;
					case '4': return KEY_END;
					case '5': return KEY_PAGEUP;
					case '6': return KEY_PAGEDOWN;
					}
				}
			}
			switch (seq[1]) {
			case 'A': return KEY_UP;
			case 'B': return KEY_DOWN;
			case 'C': return KEY_RIGHT;
			case 'D': return KEY_LEFT;
			case 'H': return KEY_HOME; // does not work with me?
			case 'F': return KEY_END;  // ... same?
			}
		}
		break;
	}

	return c;
}

/**
 * Renders the contents of the current state of the editor `e'
 * to the buffer `b'.
 *
 * TODO: parameterize things like grouping of bytes and whatnot,
 * correctly!
 */
void render_contents(struct editor* e, struct buffer* b) {
	char address[80];  // example: 000000040
	char hex[ 2 + 1];  // example: 65
	char asc[256 + 1]; // example: Hello.World!

	// Counter to indicate how many chars have been written for the current
	// row of data. This is used for later for padding, when the iteration
	// is over, but there's still some ASCII to write.
	int row_char_count = 0;

	// start_offset is to determine where we should start reading from
	// the buffer. This is dependent on where the cursor is, and on the
	// octets which are visible per line.
	int start_offset = e->line * ec->octets_per_line;
	if (start_offset >= ec->content_length) {
		start_offset = ec->content_length - ec->octets_per_line;
	}

	// Determine the end offset for displaying. There is only so much
	// to be displayed 'per screen'. I.e. if you can only display 1024
	// bytes, you only have to read a maximum of 1024 bytes.
	int bytes_per_screen = e->screen_rows * ec->octets_per_line;
	int end_offset = bytes_per_screen + start_offset - ec->octets_per_line;
	if (end_offset > ec->content_length) {
		end_offset = ec->content_length;
	}

	int offset;
	for (offset = start_offset; offset < end_offset; offset++) {
		if (offset % ec->octets_per_line == 0) {
			// start of a new row, beginning with an offset address in hex.
			int bwritten = snprintf(address, sizeof(address), "\e[0;33m%09x\e[0m:", offset);
			buffer_append(b, address, bwritten);
			// Initialize the ascii buffer to all zeroes, and reset the row char count.
			memset(asc, 0, sizeof(asc));
			row_char_count = 0;
		}

		// Format a hex string of the current character in the offset.
		snprintf(hex, sizeof(hex), "%02x", (unsigned char) e->contents[offset]);

		// Every iteration, set the ascii value in the buffer, until
		// 16 bytes are set. This will be written later when the hex
		// values are drawn to screen.
		if (isprint(e->contents[offset])) {
			asc[offset % ec->octets_per_line] = e->contents[offset];
		} else {
			// non-printable characters are represented by a dot.
			asc[offset % ec->octets_per_line] = '.';
		}

		// Every 'group' count, write a separator space.
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
		// Padding characters, to align the ASCII properly. For example, this
		// could be the output at the end of the file:
		// 000000420: 0a53 4f46 5457 4152 452e 0a              .SOFTWARE..
		//                                       ^^^^^^^^^^^^
		//                                       padding chars
		int padding_size = (ec->octets_per_line * 2) + (ec->octets_per_line / ec->grouping) - row_char_count;
		char* padding = malloc(padding_size * sizeof(char));
		memset(padding, ' ', padding_size);
		buffer_append(b, padding, padding_size);
		buffer_append(b, "\e[1;32m  ", 10);
		buffer_append(b, asc, strlen(asc));
		free(padding);
	}

	// clear everything up until the end
	buffer_append(b, "\x1b[0J", 4);

	char debug[80] = {0};
	snprintf(debug, sizeof(debug), "\x1b[37m\x1b[1;80HRows: %d, start offset: %09x, end offset: %09x", ec->screen_rows, start_offset, end_offset);
	buffer_append(b, debug, sizeof(debug));

	memset(debug, 0, sizeof(debug));
	snprintf(debug, sizeof(debug), "\e[2;80H(row,col)=(%d,%d)", ec->cursor_y, ec->cursor_x);
	buffer_append(b, debug, sizeof(debug));

	memset(debug, 0, sizeof(debug));
	snprintf(debug, sizeof(debug), "\e[3;80HHex line width: %d", ec->hex_line_width);
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

	// Position cursor. This is done by taking into account the current
	// cursor position (1 .. 40), and the amount of spaces to add due to
	// grouping.
	// TODO: this is currently a bit hacky and/or out of place.
	int curx = (ec->cursor_x - 1) * 2; // times 2 characters to represent a byte in hex
	int spaces = curx / (ec->grouping * 2); // determine spaces to add due to grouping.
	int cruft = curx + spaces + 12; // 12 = the size of the address + ": "
	bw = snprintf(buf, sizeof(buf), "\x1b[%d;%dH", ec->cursor_y, cruft);
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
	switch (c) {
	case KEY_CTRL_Q:   exit(0); break;
	case KEY_CTRL_S:   editor_writefile(ec); break;
	case KEY_UP:       editor_scroll(ec, -1); break;
	case KEY_DOWN:     editor_scroll(ec, 1); break;
	case KEY_RIGHT:    editor_move_cursor(ec, KEY_RIGHT); break;
	case KEY_LEFT:     editor_move_cursor(ec, KEY_LEFT); break;
	case KEY_HOME:     ec->line = 0; break;
	case KEY_END:      editor_scroll(ec, ec->content_length); break;
	case KEY_PAGEUP:   editor_scroll(ec, -(ec->screen_rows) + 2); break;
	case KEY_PAGEDOWN: editor_scroll(ec, ec->screen_rows - 2); break;
	default:
		editor_insert(ec, (char) c);
	}
}

/**
 * Initializes editor struct with some default values.
 */
struct editor* editor_init() {
	struct editor* ec = malloc(sizeof(struct editor));

	ec->octets_per_line = 16;
	ec->grouping = 2;
	ec->hex_line_width = ec->octets_per_line * 2 + (ec->octets_per_line / 2) - 1;

	ec->line = 0;

	ec->cursor_x = 1;
	ec->cursor_y = 1;
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

void debug_keypress() {
	char c;
	ssize_t nread;
	// check == 0 to see if EOF.
	while ((nread = read(STDIN_FILENO, &c, 1))) {
		if (c == 'q') { exit(1); };
		if (c == '\r') { printf("\r\n"); continue; }
		printf("%c=%02x, ", c, c);
		fflush(stdout);
	}
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
		//debug_keypress();
	}

	editor_free(ec);
	return EXIT_SUCCESS;
}

/*
0000000: 65 AF EF AA CC AA BB AA FF  A.....A.A.
*/
