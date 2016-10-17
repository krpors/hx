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

enum editor_mode {
	MODE_NORMAL,  // normal mode i.e. for navigating, commands.
	MODE_INSERT,  // insert values at cursor position.
	MODE_REPLACE, // replace values at cursor position.
};

/**
 * This struct contains internal information of the state of the editor.
 */
struct editor {
	int octets_per_line; // Amount of octets (bytes) per line. Ideally multiple of 2.
	int grouping;        // Amount of bytes per group. Ideally multiple of 2.

	int hex_line_width;  // the width in chars of a hex line, including
	                     // grouping spaces.

	int line;        // The 'line' in the editor. Used for scrolling.
	int cursor_x;    // Cursor x pos on the current screen
	int cursor_y;    // Cursor y pos on the current screen
	int screen_rows; // amount of screen rows after init
	int screen_cols; // amount of screen columns after init

	enum editor_mode mode;           // mode the editor is in
	bool             dirty;          // whether the buffer is modified
	char*            filename;       // the filename currently open
	char*            contents;       // the file's contents
	int              content_length; // length of the contents

	char statusmessage[80];  // status message
};

// Utility functions.
void enable_raw_mode();
void disable_raw_mode();
void clear_screen();
int read_key();

// editor functions:
struct editor* editor_init();

void editor_cursor_at_offset(struct editor* e, int offset, int* x, int *y);
void editor_delete_char_at_cursor(struct editor* e);
void editor_free(struct editor* ec);
void editor_move_cursor(struct editor* e, int dir, int amount);
int  editor_offset_at_cursor(struct editor* e);
void editor_openfile(struct editor* e, const char* filename);
void editor_process_keypress(struct editor* ec);
void editor_refresh_screen(struct editor* ec);
void editor_scroll(struct editor* e, int units);
void editor_setmode(struct editor *e, enum editor_mode mode);
int  editor_statusmessage(struct editor* ec, const char* fmt, ...);
void editor_writefile(struct editor* e);

// Global editor config.
struct editor* g_ec;

// Terminal IO settings. Used to reset it when exiting to prevent terminal
// data garbling. Declared global since we require it in the atexit() call.
struct termios orig_termios;

// Key enumerations
enum key_codes {
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
	raw.c_cc[VTIME] = 0;

    // put terminal in raw mode after flushing
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) {
		perror("Unable to set terminal to raw mode");
		exit(1);
	}
}

void disable_raw_mode() {
	editor_free(g_ec);
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
	clear_screen();
}


void clear_screen() {
	if (write(STDOUT_FILENO, "\x1b[2J\n", 5) == -1) {
		perror("Unable to clear screen");
	}
}

/**
 * Moves the cursor. The terminal cursor positions are all 1-based, so we
 * take that into account. When we scroll past boundaries (left, right, up
 * and down) we react accordingly. Note that the cursor_x/y are also 1-based,
 * and we calculate the actual position of the hex values by incrementing it
 * later on with the address size, amount of grouping spaces etc.
 *
 * This function looks convoluted as hell, but it works...
 */
void editor_move_cursor(struct editor* e, int dir, int amount) {
	switch (dir) {
	case KEY_UP:    e->cursor_y-=amount; break;
	case KEY_DOWN:  e->cursor_y+=amount; break;
	case KEY_LEFT:  e->cursor_x-=amount; break;
	case KEY_RIGHT: e->cursor_x+=amount; break;
	}
	// Did we hit the start of the file? If so, stop moving and place
	// the cursor on the top-left of the hex display.
	if (e->cursor_x <= 1 && e->cursor_y <= 1 && e->line <= 0) {
		e->cursor_x = 1;
		e->cursor_y = 1;
		return;
	}

	// Move the cursor over the x (columns) axis.
	if (e->cursor_x < 1) {
		// Are we trying to move left on the leftmost boundary?
		//
		// 000000000: 4d49 5420 4c69 6365 6e73 650a 0a43 6f70  MIT License..Cop
		// 000000010: 7972 6967 6874 2028 6329 2032 3031 3620  yright (c) 2016
		//            <--- [cursor goes to the left]
		//
		// Then we go up a row, cursor to the right. Like a text editor.
		if (e->cursor_y >= 1) {
			e->cursor_y--;
			e->cursor_x = e->octets_per_line;
		}
	} else if (e->cursor_x > e->octets_per_line) {
		// Moving to the rightmost boundary?
		//
		// 000000000: 4d49 5420 4c69 6365 6e73 650a 0a43 6f70  MIT License..Cop
		// 000000010: 7972 6967 6874 2028 6329 2032 3031 3620  yright (c) 2016
		//                    [cursor goes to the right] --->
		//
		// Then move a line down, position the cursor to the beginning of the row.
		// Unless it's the end of file.
		e->cursor_y++;
		e->cursor_x = 1;
	}

	// Did we try to move up when there's nothing? For example
	//
	//                       [up here]
	// --------------------------^
	// 000000000: 4d49 5420 4c69 6365 6e73 650a 0a43 6f70  MIT License..Cop
	//
	// Then stop moving upwards, do not scroll, return.
	if (e->cursor_y <= 1 && e->line <= 0) {
		e->cursor_y = 1;
	}

	// Move the cursor over the y axis
	if (e->cursor_y > e->screen_rows - 1) {
		e->cursor_y = e->screen_rows - 1;
		editor_scroll(e, 1);
	} else if (e->cursor_y < 1 && e->line > 0) {
		e->cursor_y = 1;
		editor_scroll(e, -1);
	}

	// Did we hit the end of the file somehow? Set the cursor position
	// to the maximum cursor position possible.
	int offset = editor_offset_at_cursor(e);
	if (offset >= e->content_length - 1) {
		editor_cursor_at_offset(e, offset, &e->cursor_x, &e->cursor_y);
		return;
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
void editor_openfile(struct editor* e, const char* filename) {
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

	e->filename = malloc(strlen(filename));
	strncpy(e->filename, filename, strlen(filename));
	e->contents = contents;
	e->content_length = size;
	editor_statusmessage(e, "\"%s\" (%d bytes)", e->filename, e->content_length);

	fclose(fp);
}

/**
 * Writes the contents of the editor's buffer the to the same filename.
 */
void editor_writefile(struct editor* e) {
	assert(e->filename != NULL);

	FILE* fp = fopen(e->filename, "w");
	if (fp == NULL) {
		editor_statusmessage(e, "Unable to open '%s' for writing", e->filename);
		return;
	}

	size_t bw = fwrite(e->contents, sizeof(char), e->content_length, fp);
	if (bw <= 0) {
		editor_statusmessage(e, "Couldn't write to file!!");
	}

	editor_statusmessage(e, "\"%s\", %d bytes written", e->filename, e->content_length);

	fclose(fp);
}

/**
 * Finds the cursor position at the given offset, taking the lines into account.
 * The result is set to the pointers `x' and `y'. We can therefore 'misuse' this
 * to set the cursor position of the editor to a given offset.
 *
 * Note that this function will NOT scroll the editor to the proper line.
 */
void editor_cursor_at_offset(struct editor* e, int offset, int* x, int* y) {
	*x = offset % e->octets_per_line + 1;
	*y = offset / e->octets_per_line - e->line + 1;
}

/**
 * Deletes the character (byte) at the current cursor position (in other
 * words, the current offset the cursor is at).
 */
void editor_delete_char_at_cursor(struct editor* e) {
	int offset = editor_offset_at_cursor(e);
	int old_length = e->content_length;

	if (e->content_length <= 0) {
		editor_statusmessage(e, "Nothing to delete");
		return;
	}

	// FIXME: when all chars have been removed from a file, this blows up.

	// Remove an element from the contents buffer by moving memory.
	// The character at the current offset is supposed to be removed.
	// Take the offset + 1, until the end of the buffer. Copy that
	// part over the offset, reallocate the contents buffer with one
	// character in size less.
	memmove(e->contents + offset, e->contents + offset + 1 , e->content_length - offset - 1);
	e->contents = realloc(e->contents, e->content_length - 1);
	e->content_length--;

	// if the deleted offset was the maximum offset, move the cursor to
	// the left.
	if (offset >= old_length - 1) {
		editor_move_cursor(e, KEY_LEFT, 1);
	}
}

/**
 * Gets the current offset at which the cursor is.
 */
inline int editor_offset_at_cursor(struct editor* e) {
	// Calculate the offset based on the cursors' x and y coord (which is bound
	// between (1 .. line width) and (1 .. max screen rows). Take the current displayed
	// line into account (which is incremented when we are paging the content).
	// Multiply it by octets_per_line since we're effectively addressing a one dimensional
	// array.
	int offset = (e->cursor_y - 1 + e->line) * e->octets_per_line + (e->cursor_x - 1);
	// Safety measure. Since we're using the value of this function to
	// index the content array, we must not go out of bounds.
	if (offset <= 0) {
		return 0;
	}
	if (offset >= e->content_length) {
		return e->content_length - 1;
	}
	return offset;
}

/**
 * Scrolls the editor by updating the `line' accordingly, within
 * the bounds of the readable parts of the buffer.
 */
void editor_scroll(struct editor* e, int units) {
	e->line += units;

	// If we wanted to scroll past the end of the file, calculate the line
	// properly. Subtract the amount of screen rows (minus 2??) to prevent
	// scrolling past the end of file.
	int upper_limit = e->content_length / e->octets_per_line - (e->screen_rows - 2);
	if (e->line >= upper_limit) {
		e->line = upper_limit;
	}

	// If we scroll past the beginning of the file (offset 0 of course),
	// set our line to zero and return. This particular condition is also
	// necessary when the upper_limit calculated goes negative, because
	// This is either some weird calculation failure from my part, but
	// this seems to work. Failing to cap this will result in bad addressing
	// of the content in render_contents().
	if (e->line <= 0) {
		e->line = 0;
	}
}

void editor_setmode(struct editor* e, enum editor_mode mode) {
	e->mode = mode;
	switch (e->mode) {
	case MODE_NORMAL:  editor_statusmessage(e, ""); break;
	case MODE_INSERT:  editor_statusmessage(e, "-- INSERT --"); break;
	case MODE_REPLACE: editor_statusmessage(e, "-- REPLACE --"); break;
	}
}

int editor_statusmessage(struct editor* e, const char* fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	// TODO valgrind complains here
	int x = vsnprintf(e->statusmessage, 80, fmt, ap);
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
	if (e->content_length <= 0) {
		// TODO: handle this in a better way.
		buffer_append(b, "\x1b[2J", 4);
		buffer_append(b, "empty", 5);
		return;
	}

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
	int start_offset = e->line * e->octets_per_line;
	if (start_offset >= e->content_length) {
		start_offset = e->content_length - e->octets_per_line;
	}

	// Determine the end offset for displaying. There is only so much
	// to be displayed 'per screen'. I.e. if you can only display 1024
	// bytes, you only have to read a maximum of 1024 bytes.
	int bytes_per_screen = e->screen_rows * e->octets_per_line;
	int end_offset = bytes_per_screen + start_offset - e->octets_per_line;
	if (end_offset > e->content_length) {
		end_offset = e->content_length;
	}

	int offset;
	for (offset = start_offset; offset < end_offset; offset++) {
		if (offset % e->octets_per_line == 0) {
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
			asc[offset % e->octets_per_line] = e->contents[offset];
		} else {
			// non-printable characters are represented by a dot.
			asc[offset % e->octets_per_line] = '.';
		}

		// Every 'group' count, write a separator space.
		if (offset % e->grouping == 0) {
			buffer_append(b, " ", 1);
			row_char_count++;
		}

		// First, write the hex value of the byte at the current offset.
		buffer_append(b, hex, 2);
		row_char_count += 2;

		// If we reached the end of a 'row', start writing the ASCII equivalents
		// of the 'row', in a different color. Then hit CRLF to go to the next line.
		if ((offset+1) % e->octets_per_line == 0) {
			buffer_append(b, "\e[1;32m", 8);
			buffer_append(b, "  ", 2);
			buffer_append(b, asc, strlen(asc));
			buffer_append(b, "\r\n", 2);
		}
	}

	// Check remainder of the last offset. If its bigger than zero,
	// we got a last line to write (ASCII only).
	if (offset % e->octets_per_line > 0) {
		// Padding characters, to align the ASCII properly. For example, this
		// could be the output at the end of the file:
		// 000000420: 0a53 4f46 5457 4152 452e 0a              .SOFTWARE..
		//                                       ^^^^^^^^^^^^
		//                                       padding chars
		int padding_size = (e->octets_per_line * 2) + (e->octets_per_line / e->grouping) - row_char_count;
		char* padding = malloc(padding_size * sizeof(char));
		memset(padding, ' ', padding_size);
		buffer_append(b, padding, padding_size);
		buffer_append(b, "\e[1;32m  ", 10);
		buffer_append(b, asc, strlen(asc));
		free(padding);
	}

	// clear everything up until the end
	buffer_append(b, "\x1b[0J", 4);

	int len = 256;
	char debug[len];
	memset(debug, 0, len);
	snprintf(debug, len, "\x1b[37m\x1b[1;80HRows: %d, start offset: %09x, end offset: %09x", e->screen_rows, start_offset, end_offset);
	buffer_append(b, debug, len);

	memset(debug, 0, len);
	snprintf(debug, len, "\e[2;80H(cur_y,cur_x)=(%d,%d)", e->cursor_y, e->cursor_x);
	buffer_append(b, debug, len);

	memset(debug, 0, len);
	snprintf(debug, len, "\e[3;80HHex line width: %d", e->hex_line_width);
	buffer_append(b, debug, len);

	memset(debug, 0, len);
	int curr_offset = editor_offset_at_cursor(e);
	snprintf(debug, len, "\e[4;80H\e[0KLine: %d, cursor offset: %d (hex: %02x)", e->line, curr_offset, (unsigned char) e->contents[curr_offset]);
	buffer_append(b, debug, len);

	memset(debug, 0, len);
	int xx;
	int yy;
	editor_cursor_at_offset(e, curr_offset, &xx, &yy);
	snprintf(debug, len, "\e[5;80H\e[0Kyy,xx = %d, %d", yy, xx);
	buffer_append(b, debug, len);


}

/**
 * Refreshes the screen. It uses a temporary buffer to write everything that's
 * eligible for display to an internal buffer, and then 'draws' it to the screen
 * in one call.
 */
void editor_refresh_screen(struct editor* e) {
	char buf[32]; // temp buffer for snprintf.
	int bw; // bytes written by snprintf.
	struct buffer* b = buffer_create();

	buffer_append(b, "\x1b[H", 3); // move the cursor top left

	render_contents(e, b);

	// Move down to write the status message.
	bw = snprintf(buf, sizeof(buf), "\x1b[%d;0H", e->screen_rows);
	buffer_append(b, buf, bw);
	// Change the color
	buffer_append(b, "\x1b[0;30;47m", 10);
	buffer_append(b, e->statusmessage, strlen(e->statusmessage));
	// Clear until the end of the line to the right
	//buffer_append(b, "\x1b[0K", 4);

	// Position cursor. This is done by taking into account the current
	// cursor position (1 .. 40), and the amount of spaces to add due to
	// grouping.
	// TODO: this is currently a bit hacky and/or out of place.
	int curx = (e->cursor_x - 1) * 2; // times 2 characters to represent a byte in hex
	int spaces = curx / (e->grouping * 2); // determine spaces to add due to grouping.
	int cruft = curx + spaces + 12; // 12 = the size of the address + ": "
	bw = snprintf(buf, sizeof(buf), "\x1b[%d;%dH", e->cursor_y, cruft);
	buffer_append(b, buf, bw);

	buffer_draw(b);
	buffer_free(b);
}

void editor_insert(struct editor* e, char x) {
	// We are inserting a single character. Reallocate memory to contain
	// this extra byte.
	e->contents = realloc(e->contents, e->content_length + 1);
	// Set the last allocated index to the character x.
	e->contents[e->content_length] = x;
	// Increase the content length since we inserted a character.
	e->content_length++;
}

void editor_replace_byte(struct editor* e, char x) {
	int offset = editor_offset_at_cursor(e);
	e->contents[offset] = x;
	editor_move_cursor(e, KEY_RIGHT, 1);
}

/**
 * Processes a keypress accordingly.
 */
void editor_process_keypress(struct editor* e) {
	int c = read_key();

	// Handle some keys, independent of mode we're in.
	switch (c) {
	case KEY_ESC: editor_setmode(e, MODE_NORMAL); return;
	case KEY_CTRL_Q:   exit(0); return;
	case KEY_CTRL_S:   editor_writefile(e); return;

	case KEY_UP:
	case KEY_DOWN:
	case KEY_RIGHT:
	case KEY_LEFT:     editor_move_cursor(e, c, 1); return;

	case KEY_HOME:     e->cursor_x = 1; return;
	case KEY_END:      e->cursor_x = e->octets_per_line; return;
	case KEY_PAGEUP:   editor_scroll(e, -(e->screen_rows) + 2); return;
	case KEY_PAGEDOWN: editor_scroll(e, e->screen_rows - 2); return;
	}

	// Handle commands when in normal mode.
	if (e->mode == MODE_NORMAL) {
		switch (c) {
		// vi(m) like movement:
		case 'h': editor_move_cursor(e, KEY_LEFT,  1); break;
		case 'j': editor_move_cursor(e, KEY_DOWN,  1); break;
		case 'k': editor_move_cursor(e, KEY_UP,    1); break;
		case 'l': editor_move_cursor(e, KEY_RIGHT, 1); break;
		case 'x':
			editor_delete_char_at_cursor(e);
			break;
		case 'i':
			editor_setmode(e, MODE_INSERT); break;
		case 'r':
			editor_setmode(e, MODE_REPLACE); break;
		case 'b':
			// Move one group back.
			editor_move_cursor(e, KEY_LEFT, e->grouping); break;
		case 'w':
			// Move one group further.
			editor_move_cursor(e, KEY_RIGHT, e->grouping); break;
		case 'G':
			// Scroll to the end, place the cursor at the end.
			editor_scroll(e, e->content_length);
			editor_cursor_at_offset(e, e->content_length-1, &e->cursor_x, &e->cursor_y);
			break;
		case 'g':
			// Read extra keypress
			c = read_key();
			if (c == 'g') {
				// scroll to the start, place cursor at start.
				e->line = 0;
				editor_cursor_at_offset(e, 0, &e->cursor_x, &e->cursor_y);
			}
			break;
		}

		// Command parsed, do not continue.
		return;
	}

	// Actual edit mode.
	if (e->mode == MODE_INSERT) {
		editor_insert(e, (char) c);
	} else if (e->mode == MODE_REPLACE) {
		editor_replace_byte(e, (char) c);
	}
}
/**
 * Initializes editor struct with some default values.
 */
struct editor* editor_init() {
	struct editor* e = malloc(sizeof(struct editor));

	e->octets_per_line = 32;
	e->grouping = 4;
	e->hex_line_width = e->octets_per_line * 2 + (e->octets_per_line / 2) - 1;

	e->line = 0;
	e->cursor_x = 1;
	e->cursor_y = 1;
	e->filename = NULL;
	e->contents = NULL;
	e->content_length = 0;

	e->mode = MODE_NORMAL;

	get_window_size(&(e->screen_rows), &(e->screen_cols));

	return e;
}

void editor_free(struct editor* e) {
	free(e->filename);
	free(e->contents);
	free(e);
}

void debug_keypress() {
	char c;
	ssize_t nread;
	// check == 0 to see if EOF.
	while ((nread = read(STDIN_FILENO, &c, 1))) {
		if (c == 'q') { exit(1); };
		if (c == '\r' || c == '\n') { printf("\r\n"); continue; }
		if (isprint(c)) {
			printf("%c = %02x, ", c, c);
		} else {
			printf(". = %02x, ", c);
		}
		fflush(stdout);
	}
}

int main(int argc, char* argv[]) {
	if (argc != 2) {
		fprintf(stderr, "expected arg\n");
		exit(2);
	}

	// Editor configuration passed around.
	g_ec = editor_init();
	editor_openfile(g_ec, argv[1]);

	enable_raw_mode();
	clear_screen();

	while (true) {
		editor_refresh_screen(g_ec);
		editor_process_keypress(g_ec);
		//debug_keypress();
	}

	editor_free(g_ec);
	return EXIT_SUCCESS;
}
