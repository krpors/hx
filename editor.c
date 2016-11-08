/*
 * This file is part of hx - a hex editor for the terminal.
 *
 * Copyright (c) 2016 Kevin Pors. See LICENSE for details.
 */

#include "editor.h"
#include "util.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <sys/stat.h>
#include <unistd.h>

/**
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

void editor_openfile(struct editor* e, const char* filename) {
	FILE* fp = fopen(filename, "rb");
	if (fp == NULL) {
		fprintf(stderr, "Cannot open file '%s': %s\n", filename, strerror(errno));
		exit(1);
	}

	// stat() the file.
	struct stat statbuf;
	if (stat(filename, &statbuf) == -1) {
		perror("Cannot stat file");
		exit(1);
	}
	// S_ISREG is a a POSIX macro to check whether the given st_mode denotes a
	// regular file. See `man 2 stat'.
	if (!S_ISREG(statbuf.st_mode)) {
		fprintf(stderr, "File '%s' is not a regular file\n", filename);
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
		perror("Unable to read file contents");
		free(contents);
		exit(1);
	}

	// duplicate string without using gnu99 strdup().
	e->filename = malloc(strlen(filename) + 1);
	strncpy(e->filename, filename, strlen(filename) + 1);
	e->contents = contents;
	e->content_length = size;

	// Check if the file is readonly, and warn the user about that.
	if (access(filename, W_OK) == -1) {
		editor_statusmessage(e, STATUS_WARNING, "\"%s\" (%d bytes) [readonly]", e->filename, e->content_length);
	} else {
		editor_statusmessage(e, STATUS_INFO, "\"%s\" (%d bytes)", e->filename, e->content_length);
	}

	fclose(fp);
}

void editor_writefile(struct editor* e) {
	assert(e->filename != NULL);

	FILE* fp = fopen(e->filename, "wb");
	if (fp == NULL) {
		editor_statusmessage(e, STATUS_ERROR, "Unable to open '%s' for writing: %s", e->filename, strerror(errno));
		return;
	}

	size_t bw = fwrite(e->contents, sizeof(char), e->content_length, fp);
	if (bw <= 0) {
		editor_statusmessage(e, STATUS_ERROR, "Unable write to file: %s", strerror(errno));
		return;
	}

	editor_statusmessage(e, STATUS_INFO, "\"%s\", %d bytes written", e->filename, e->content_length);

	fclose(fp);
}


void editor_cursor_at_offset(struct editor* e, int offset, int* x, int* y) {
	*x = offset % e->octets_per_line + 1;
	*y = offset / e->octets_per_line - e->line + 1;
}


void editor_delete_char_at_cursor(struct editor* e) {
	int offset = editor_offset_at_cursor(e);
	int old_length = e->content_length;

	if (e->content_length <= 0) {
		editor_statusmessage(e, STATUS_WARNING, "Nothing to delete");
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


void editor_increment_byte(struct editor* e, int amount) {
	int offset = editor_offset_at_cursor(e);
	e->contents[offset] += amount;
}


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
	case MODE_NORMAL:  editor_statusmessage(e, STATUS_INFO, ""); break;
	case MODE_INSERT:  editor_statusmessage(e, STATUS_INFO, "-- INSERT --"); break;
	case MODE_REPLACE: editor_statusmessage(e, STATUS_INFO, "-- REPLACE --"); break;
	case MODE_COMMAND: break;
	}
}


int editor_statusmessage(struct editor* e, enum status_severity sev, const char* fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	int x = vsnprintf(e->status_message, sizeof(e->status_message), fmt, ap);
	va_end(ap);

	e->status_severity = sev;

	return x;
}


void editor_render_ascii(struct editor* e, int rownum, const char* asc, struct charbuf* b) {
	assert(rownum > 0);

	// If the rownum given is the current y cursor position, then render the line
	// differently from the rest.
	if (rownum == e->cursor_y) {
		// Check the cursor position on the x axis
		for (int i = 0; i < strlen(asc); i++) {
			char x[1];
			if (i+1 == e->cursor_x) {
				// Highlight by 'inverting' the color
				charbuf_append(b, "\x1b[30;47m", 8);
			} else {
				// Other characters with greenish
				charbuf_append(b, "\x1b[32;40;1m", 10);
			}
			x[0] = asc[i];
			charbuf_append(b, x, 1);
		}
	} else {
		charbuf_append(b, "\x1b[1;37m", 7);
		charbuf_append(b, asc, strlen(asc));
	}
}

void editor_render_cmd(struct editor* e) {
}


void editor_render_contents(struct editor* e, struct charbuf* b) {
	if (e->content_length <= 0) {
		// TODO: handle this in a better way.
		charbuf_append(b, "\x1b[2J", 4);
		charbuf_append(b, "empty", 5);
		return;
	}

	// FIXME: proper sizing of these arrays (malloc?)
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
	int row = 0;
	for (offset = start_offset; offset < end_offset; offset++) {
		if (offset % e->octets_per_line == 0) {
			// start of a new row, beginning with an offset address in hex.
			int bwritten = snprintf(address, sizeof(address), "\e[0;33m%09x\e[0m:", offset);
			charbuf_append(b, address, bwritten);
			// Initialize the ascii buffer to all zeroes, and reset the row char count.
			memset(asc, 0, sizeof(asc));
			row_char_count = 0;
			row++;
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
			charbuf_append(b, " ", 1);
			row_char_count++;
		}

		// First, write the hex value of the byte at the current offset.
		charbuf_append(b, hex, 2);
		row_char_count += 2;

		// If we reached the end of a 'row', start writing the ASCII equivalents
		// of the 'row'. Highlight the current line and offset on the ASCII part.
		if ((offset+1) % e->octets_per_line == 0) {
			charbuf_append(b, "  ", 2);
			editor_render_ascii(e, row, asc, b);
			charbuf_append(b, "\r\n", 2);
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
		charbuf_append(b, padding, padding_size);
		charbuf_append(b, "\x1b[0m  ", 6);
		// render cursor on the ascii when applicable.
		editor_render_ascii(e, row, asc, b);
		free(padding);
	}

	// clear everything up until the end
	charbuf_append(b, "\x1b[0J", 4);

#ifndef NDEBUG
	int len = 256;
	char debug[len];
	memset(debug, 0, len);
	snprintf(debug, len, "\x1b[0m\x1b[37m\x1b[1;80HRows: %d, start offset: %09x, end offset: %09x", e->screen_rows, start_offset, end_offset);
	charbuf_append(b, debug, len);

	memset(debug, 0, len);
	snprintf(debug, len, "\e[2;80H(cur_y,cur_x)=(%d,%d)", e->cursor_y, e->cursor_x);
	charbuf_append(b, debug, len);

	memset(debug, 0, len);
	snprintf(debug, len, "\e[3;80HHex line width: %d", e->hex_line_width);
	charbuf_append(b, debug, len);

	memset(debug, 0, len);
	int curr_offset = editor_offset_at_cursor(e);
	snprintf(debug, len, "\e[4;80H\e[0KLine: %d, cursor offset: %d (hex: %02x)", e->line, curr_offset, (unsigned char) e->contents[curr_offset]);
	charbuf_append(b, debug, len);

	memset(debug, 0, len);
	int xx;
	int yy;
	editor_cursor_at_offset(e, curr_offset, &xx, &yy);
	snprintf(debug, len, "\e[5;80H\e[0Kyy,xx = %d, %d", yy, xx);
	charbuf_append(b, debug, len);
#endif
}


void editor_render_ruler(struct editor* e, struct charbuf* b) {
	// Nothing to see. No address, no byte, no percentage. It's all a plain
	// dark void right now. Oblivion. No data to see here, move along.
	if (e->content_length <= 0) {
		return;
	}

	char rulermsg[80]; // buffer for the actual message.
	char buf[20];      // buffer for the cursor positioning

	int offset_at_cursor = editor_offset_at_cursor(e);
	unsigned char val = e->contents[offset_at_cursor];
	int percentage = (float)(offset_at_cursor + 1) / (float)e->content_length * 100;

	// TODO: move cursor down etc to remain independent on the previous cursor
	// movement in refresh_screen().

	// Create a ruler string. We need to calculate the amount of bytes
	// we've actually written, to subtract that from the screen_cols to
	// align the string properly.
	int rmbw = snprintf(rulermsg, sizeof(rulermsg),
			"0x%09x,%d (%02x)  %d%%",
			offset_at_cursor, offset_at_cursor, val, percentage);
	// Create a string for the buffer to position the cursor.
	int cpbw = snprintf(buf, sizeof(buf), "\x1b[0m\x1b[%d;%dH", e->screen_rows, e->screen_cols - rmbw);

	// First write the cursor string, followed by the ruler message.
	charbuf_append(b, buf, cpbw);
	charbuf_append(b, rulermsg, rmbw);
}


void editor_render_status(struct editor* e, struct charbuf* b) {
	// buf holds the string for the cursor movement.
	char buf[20];
	int bw = snprintf(buf, sizeof(buf), "\x1b[%d;0H", e->screen_rows);
	charbuf_append(b, buf, bw);

	// Set color, write status message, and reset the color after.
	switch (e->status_severity) {
	case STATUS_INFO:    charbuf_append(b, "\x1b[0;30;47m", 10); break; // black on white
	case STATUS_WARNING: charbuf_append(b, "\x1b[0;30;43m", 10); break; // black on yellow
	case STATUS_ERROR:   charbuf_append(b, "\x1b[1;37;41m", 10); break; // white on red
	//                bold/increased intensity__/ /  /
	//                    foreground color_______/  /
	//                       background color______/
	}

	charbuf_append(b, e->status_message, strlen(e->status_message));
	charbuf_append(b, "\x1b[0m", 4);
}


void editor_refresh_screen(struct editor* e) {
	char buf[32]; // temp buffer for snprintf.
	int bw; // bytes written by snprintf.
	struct charbuf* b = charbuf_create();

	charbuf_append(b, "\x1b[?25l", 6);
	charbuf_append(b, "\x1b[H", 3); // move the cursor top left

	if (e->mode == MODE_REPLACE || e->mode == MODE_NORMAL) {
		editor_render_contents(e, b);
		editor_render_status(e, b);

		// Ruler: move to the right of the screen etc.
		editor_render_ruler(e, b);

		// Position cursor. This is done by taking into account the current
		// cursor position (1 .. 40), and the amount of spaces to add due to
		// grouping.
		// TODO: this is currently a bit hacky and/or out of place.
		int curx = (e->cursor_x - 1) * 2; // times 2 characters to represent a byte in hex
		int spaces = curx / (e->grouping * 2); // determine spaces to add due to grouping.
		int cruft = curx + spaces + 12; // 12 = the size of the address + ": "
		bw = snprintf(buf, sizeof(buf), "\x1b[%d;%dH", e->cursor_y, cruft);
		charbuf_append(b, buf, bw);
	} else if (e->mode == MODE_COMMAND) {
		// When in command mode, handle rendering different. For instance,
		// the cursor is placed at the bottom. Ruler is not required.
		int cpbw = snprintf(buf, sizeof(buf), "\x1b[0m\x1b[%d;1H", e->screen_rows);
		charbuf_append(b, buf, cpbw);
		charbuf_append(b, "\x1b[2K:", 5); // clear entire line

		charbuf_append(b, e->cmdbuffer, e->cmdbuffer_index);
		// 1. move cursor to bottom.
		// 2. read input
		// 3. execute
	}

	charbuf_append(b, "\x1b[?25h", 6);

	charbuf_draw(b);
	charbuf_free(b);
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


void editor_process_cmdinput(struct editor* e, char c) {
	// if we hit enter, set the mode to normal mode, execute
	// the command, and possibly set a statusmessage.
	if (c == KEY_ENTER) {
		editor_setmode(e, MODE_NORMAL);
		fprintf(stderr, "what: %s\n", e->cmdbuffer);
		editor_statusmessage(e, STATUS_ERROR, "Command not found: %s", e->cmdbuffer);
		e->cmdbuffer_index = 0;
		memset(e->cmdbuffer, 0, sizeof(e->cmdbuffer));
		return;
	}

	// Safety guard. Our cmdbuffer size is limited to 180 chars, so stop incrementing
	// our buffer index after this maximum.
	if (e->cmdbuffer_index >= sizeof(e->cmdbuffer)) {
		return;
	}

	if (c == KEY_BACKSPACE && e->cmdbuffer_index > 0) {
		e->cmdbuffer[e->cmdbuffer_index] = '\0';
		e->cmdbuffer_index--;
	}

	// Only act on printable characters.
	if (!isprint(c)) {
		return;
	}

	e->cmdbuffer[e->cmdbuffer_index++] = c;
}


void editor_process_keypress(struct editor* e) {
	int c = read_key();
	if (c == -1) {
		return;
	}

	// Handle some keys, independent of mode we're in.
	switch (c) {
	case KEY_ESC:    editor_setmode(e, MODE_NORMAL); return;
	case KEY_CTRL_Q: exit(0); return;
	case KEY_CTRL_S: editor_writefile(e); return;
	}

	// Handle commands when in normal mode.
	if (e->mode == MODE_NORMAL) {
		switch (c) {
		// cursor movement:
		case KEY_UP:
		case KEY_DOWN:
		case KEY_RIGHT:
		case KEY_LEFT: editor_move_cursor(e, c, 1); break;

		case 'h': editor_move_cursor(e, KEY_LEFT,  1); break;
		case 'j': editor_move_cursor(e, KEY_DOWN,  1); break;
		case 'k': editor_move_cursor(e, KEY_UP,    1); break;
		case 'l': editor_move_cursor(e, KEY_RIGHT, 1); break;
		case ']': editor_increment_byte(e, 1); break;
		case '[': editor_increment_byte(e, -1); break;
		case KEY_DEL:
		case 'x': editor_delete_char_at_cursor(e); break;

		case 'i': editor_setmode(e, MODE_INSERT); return;
		case 'r': editor_setmode(e, MODE_REPLACE); return;
		case ':': editor_setmode(e, MODE_COMMAND); return;

		// move `grouping` amount back or forward:
		case 'b': editor_move_cursor(e, KEY_LEFT, e->grouping); break;
		case 'w': editor_move_cursor(e, KEY_RIGHT, e->grouping); break;
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

		case KEY_HOME:     e->cursor_x = 1; return;
		case KEY_END:      e->cursor_x = e->octets_per_line; return;
		case KEY_PAGEUP:   editor_scroll(e, -(e->screen_rows) + 2); return;
		case KEY_PAGEDOWN: editor_scroll(e, e->screen_rows - 2); return;
		}
	}

	if (e->mode == MODE_INSERT) {
		// Insert character after the cursor.
		//editor_insert(e, (char) c);
	} else if (e->mode == MODE_REPLACE) {
		// Check if the input character was a valid hex value. If not, return prematurely.
		if (!ishex(c)) {
			editor_statusmessage(e, STATUS_ERROR, "'%c' is not valid hex", c);
			return;
		}
		int next = read_key();
		if (!ishex(next)) {
			editor_statusmessage(e, STATUS_ERROR, "'%c' is not valid hex", next);
			return;
		}

		int offset = editor_offset_at_cursor(e);
		char crud[2 + 1];
		snprintf(crud, sizeof(crud), "%c%c", (char)c, (char)next);
		char ashex = hex2bin(crud);
		editor_replace_byte(e, ashex);
		editor_statusmessage(e, STATUS_INFO, "Replaced byte at offset %09x with %02x", offset, (unsigned char)ashex);
	} else if (e->mode == MODE_COMMAND) {
		// Input manual, typed commands.
		editor_process_cmdinput(e, c);
	}
}

/**
 * Initializes editor struct with some default values.
 */
struct editor* editor_init() {
	struct editor* e = malloc(sizeof(struct editor));

	e->octets_per_line = 16;
	e->grouping = 2;
	e->hex_line_width = e->octets_per_line * 2 + (e->octets_per_line / 2) - 1;

	e->line = 0;
	e->cursor_x = 1;
	e->cursor_y = 1;
	e->filename = NULL;
	e->contents = NULL;
	e->content_length = 0;

	memset(e->status_message, 0, sizeof(e->status_message));

	e->mode = MODE_NORMAL;

	memset(e->cmdbuffer, 0, sizeof(e->cmdbuffer));
	e->cmdbuffer_index = 0;

	get_window_size(&(e->screen_rows), &(e->screen_cols));

	return e;
}

void editor_free(struct editor* e) {
	free(e->filename);
	free(e->contents);
	free(e);
}

