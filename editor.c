/*
 * This file is part of hx - a hex editor for the terminal.
 *
 * Copyright (c) 2016 Kevin Pors. See LICENSE for details.
 */

#include "editor.h"
#include "util.h"
#include "undo.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <sys/stat.h>
#include <unistd.h>

/*
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
	unsigned int offset = editor_offset_at_cursor(e);
	if (offset >= e->content_length - 1) {
		editor_cursor_at_offset(e, offset, &e->cursor_x, &e->cursor_y);
		return;
	}
}

void editor_newfile(struct editor* e, const char* filename) {
	e->filename = malloc(strlen(filename) + 1);
	strncpy(e->filename, filename, strlen(filename) + 1);
	e->contents = malloc(0);
	e->content_length = 0;
}

void editor_openfile(struct editor* e, const char* filename) {
	FILE* fp = fopen(filename, "rb");
	if (fp == NULL) {
		if (errno == ENOENT) {
			// file does not exist, open it as a new file and return.
			editor_newfile(e, filename);
			return;
		}

		// Other errors (i.e. permission denied). Exit prematurely,
		// no use in continuing.
		perror("Unable to open file");
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

	// The content buffer. When stat() returns a non-zero length, this will
	// be malloc'd. When <= 0, this will be assigned via a charbuf. This
	// branching is done because 1) Otherwise /proc/ cannot be read, and 2)
	// reading a large file just with fgetc() imposes a major negative performance
	// impact.
	char* contents;
	int content_length = 0;

	if (statbuf.st_size <= 0) {
		// The stat() returned a (less than) zero size length. This may be
		// because the user is trying to read a file from /proc/. In that
		// case, read the data per-byte until EOF.
		struct charbuf* buf = charbuf_create();
		int c;
		char tempbuf[1];
		while ((c = fgetc(fp)) != EOF) {
			tempbuf[0] = (char) c;
			charbuf_append(buf, tempbuf, 1);
		}
		// Point contents to the charbuf's contents and set the length accordingly.
		contents = buf->contents;
		content_length = buf->len;
	} else {
		// stat() returned a size we can work with. Allocate memory for the
		// buffer, No need for extra room for a null string terminator, since
		// we're possibly reading binary data only anyway (which can contain 0x00).
		contents = malloc(sizeof(char) * statbuf.st_size);
		content_length = statbuf.st_size;

		// fread() has a massive performance improvement when reading large files.
		if (fread(contents, 1, statbuf.st_size, fp) < (size_t) statbuf.st_size) {
			perror("Unable to read file contents");
			free(contents);
			exit(1);
		}
	}

	 // duplicate string without using gnu99 strdup().
	e->filename = malloc(strlen(filename) + 1);
	strncpy(e->filename, filename, strlen(filename) + 1);
	e->contents = contents;
	e->content_length = content_length;

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
	e->dirty = false;

	fclose(fp);
}


void editor_cursor_at_offset(struct editor* e, int offset, int* x, int* y) {
	*x = offset % e->octets_per_line + 1;
	*y = offset / e->octets_per_line - e->line + 1;
}


void editor_delete_char_at_cursor(struct editor* e) {
	unsigned int offset = editor_offset_at_cursor(e);
	unsigned int old_length = e->content_length;

	if (e->content_length <= 0) {
		editor_statusmessage(e, STATUS_WARNING, "Nothing to delete");
		return;
	}

	unsigned char charat = e->contents[offset];
	editor_delete_char_at_offset(e, offset);
	e->dirty = true;

	// if the deleted offset was the maximum offset, move the cursor to
	// the left.
	if (offset >= old_length - 1) {
		editor_move_cursor(e, KEY_LEFT, 1);
	}
	action_list_add(e->undo_list, ACTION_DELETE, offset, charat);
}

void editor_delete_char_at_offset(struct editor* e, unsigned int offset) {
	// Remove an element from the contents buffer by moving memory.
	// The character at the current offset is supposed to be removed.
	// Take the offset + 1, until the end of the buffer. Copy that
	// part over the offset, reallocate the contents buffer with one
	// character in size less.
	memmove(e->contents + offset, e->contents + offset + 1 , e->content_length - offset - 1);
	e->contents = realloc(e->contents, e->content_length - 1);
	e->content_length--;

}

void editor_increment_byte(struct editor* e, int amount) {
	unsigned int offset = editor_offset_at_cursor(e);
	unsigned char prev = e->contents[offset];
	e->contents[offset] += amount;

	action_list_add(e->undo_list, ACTION_REPLACE, offset, prev);
}


inline int editor_offset_at_cursor(struct editor* e) {
	// Calculate the offset based on the cursors' x and y coord (which is bound
	// between (1 .. line width) and (1 .. max screen rows). Take the current displayed
	// line into account (which is incremented when we are paging the content).
	// Multiply it by octets_per_line since we're effectively addressing a one dimensional
	// array.
	unsigned int offset = (e->cursor_y - 1 + e->line) * e->octets_per_line + (e->cursor_x - 1);
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

void editor_scroll_to_offset(struct editor* e, unsigned int offset) {
	if (offset > e->content_length) {
		editor_statusmessage(e, STATUS_ERROR, "Out of range: 0x%09x (%u)", offset, offset);
		return;
	}

	// Check if the offset is within range of the current display.
	// Calculate the minimum offset visible, and the maximum. If
	// the requested offset is within that range, do not update
	// the e->line yet (i.e. do not scroll).
	unsigned int offset_min = e->line * e->octets_per_line;
	unsigned int offset_max = offset_min + (e->screen_rows * e->octets_per_line);

	if (offset >= offset_min && offset <= offset_max) {
		// We're within range! Update the cursor position, but
		// do not scroll, and just return.
		editor_cursor_at_offset(e, offset, &(e->cursor_x), &(e->cursor_y));
		return;
	}

	// Determine what 'line' to set, by dividing the offset to
	// be displayed by the number of octets per line. The line
	// is subtracted with the number of rows in the screen, divided
	// by 2 so the cursor can be centered on the screen.
	e->line = offset / e->octets_per_line - (e->screen_rows / 2);

	// TODO: editor_scroll uses this same limit. Probably better to refactor
	// this part on way or another to prevent dupe.
	int upper_limit = e->content_length / e->octets_per_line - (e->screen_rows - 2);
	if (e->line >= upper_limit) {
		e->line = upper_limit;
	}

	if (e->line <= 0) {
		e->line = 0;
	}

	editor_cursor_at_offset(e, offset, &(e->cursor_x), &(e->cursor_y));
}

void editor_setmode(struct editor* e, enum editor_mode mode) {
	e->mode = mode;
	switch (e->mode) {
	case MODE_NORMAL:        editor_statusmessage(e, STATUS_INFO, ""); break;
	case MODE_APPEND:        editor_statusmessage(e, STATUS_INFO, "-- APPEND -- "); break;
	case MODE_APPEND_ASCII:  editor_statusmessage(e, STATUS_INFO, "-- APPEND ASCII --"); break;
	case MODE_INSERT:        editor_statusmessage(e, STATUS_INFO, "-- INSERT --"); break;
	case MODE_INSERT_ASCII:  editor_statusmessage(e, STATUS_INFO, "-- INSERT ASCII --"); break;
	case MODE_REPLACE:       editor_statusmessage(e, STATUS_INFO, "-- REPLACE --"); break;
	case MODE_COMMAND: break;
	case MODE_SEARCH:  break;
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

void editor_render_ascii(struct editor* e, int rownum, unsigned int start_offset, struct charbuf* b) {
	int cc = 0; // cursor counter, to check whether we should highlight the current offset.

	for (unsigned int offset = start_offset; offset < start_offset + e->octets_per_line; offset++) {
		// Make sure we do not go out of bounds.
		if (offset >= e->content_length) {
			return;
		}

		cc++;

		char c =  e->contents[offset];

		// If we need to highlight the cursor in the current iteration,
		// do so by inverting the color (7m). In all other cases, reset (0m).
		if (rownum == e->cursor_y && cc == e->cursor_x) {
			charbuf_append(b, "\x1b[7m", 4);
		} else {
			charbuf_appendf(b, "\x1b[0m", 4);
		}

		// Printable characters use a different color from non-printable characters.
		if (isprint(c)) {
			charbuf_appendf(b, "\x1b[33m%c", c);
		} else {
			charbuf_append(b, "\x1b[36m.", 6);
		}
	}
	charbuf_append(b, "\x1b[0m", 4);
}

void editor_render_contents(struct editor* e, struct charbuf* b) {
	if (e->content_length <= 0) {
		charbuf_append(b, "\x1b[2J", 4);
		charbuf_appendf(b, "File is empty. Use 'i' to insert a hexadecimal value.");
		return;
	}

	// FIXME: proper sizing of these arrays (malloc?)
	char hex[ 32 + 1];  // example: 65
	int  hexlen = 0;    // assigned by snprintf - we need to know the amount of chars written.
	char asc[256 + 1];  // example: Hello.World!


	// Counter to indicate how many chars have been written for the current
	// row of data. This is used for later for padding, when the iteration
	// is over, but there's still some ASCII to write.
	int row_char_count = 0;

	// start_offset is to determine where we should start reading from
	// the buffer. This is dependent on where the cursor is, and on the
	// octets which are visible per line.
	unsigned int start_offset = e->line * e->octets_per_line;
	if (start_offset >= e->content_length) {
		start_offset = e->content_length - e->octets_per_line;
	}

	// Determine the end offset for displaying. There is only so much
	// to be displayed 'per screen'. I.e. if you can only display 1024
	// bytes, you only have to read a maximum of 1024 bytes.
	int bytes_per_screen = e->screen_rows * e->octets_per_line;
	unsigned int end_offset = bytes_per_screen + start_offset - e->octets_per_line;
	if (end_offset > e->content_length) {
		end_offset = e->content_length;
	}

	unsigned int offset;

	int row = 0; // Row counter, from 0 to term height
	int col = 0; // Col counter, from 0 to number of octets per line. Used to render
	             // a colored cursor per byte.

	for (offset = start_offset; offset < end_offset; offset++) {
		unsigned char curr_byte = e->contents[offset];

		if (offset % e->octets_per_line == 0) {
			// start of a new row, beginning with an offset address in hex.
			charbuf_appendf(b, "\x1b[1;35m%09x\x1b[0m:", offset);
			// Initialize the ascii buffer to all zeroes, and reset the row char count.
			memset(asc, '\0', sizeof(asc));
			row_char_count = 0;
			col = 0;
			row++;
		}
		col++;

		// Format a hex string of the current character in the offset.
		if (isprint(curr_byte)) {
			// If the character is printable, use a different color.
			hexlen = snprintf(hex, sizeof(hex), "\x1b[1;34m%02x", curr_byte);
		} else {
			// Non printable: use default color.
			hexlen = snprintf(hex, sizeof(hex), "%02x", curr_byte);
		}

		// Every iteration, set the ascii value in the buffer, until
		// 16 bytes are set. This will be written later when the hex
		// values are drawn to screen.
		if (isprint(curr_byte)) {
			asc[offset % e->octets_per_line] = curr_byte;
		} else {
			// non-printable characters are represented by a dot.
			asc[offset % e->octets_per_line] = '.';
		}

		// Every 'group' count, write a separator space.
		if (offset % e->grouping == 0) {
			charbuf_append(b, " ", 1);
			row_char_count++;
		}

		// Cursor rendering.
		if (e->cursor_y == row) {
			// Render the selected byte with a different color. Easier
			// to distinguish in the army of hexadecimal values.
			if (e->cursor_x == col) {
				charbuf_append(b, "\x1b[7m", 4);
			}
		}
		// Write the hex value of the byte at the current offset, and reset attributes.
		charbuf_append(b, hex, hexlen);
		charbuf_append(b, "\x1b[0m", 4);

		row_char_count += 2;

		// If we reached the end of a 'row', start writing the ASCII equivalents.
		if ((offset+1) % e->octets_per_line == 0) {
			// Two spaces "gap" between the hexadecimal display, and the ASCII equiv.
			charbuf_append(b, "  ", 2);
			// Calculate the 'start offset' of the ASCII part to write. Delegate
			// this to the render_ascii function.
			int the_offset = offset + 1 - e->octets_per_line;
			editor_render_ascii(e, row, the_offset, b);
			charbuf_append(b, "\r\n", 2);
		}
	}

	// Check remainder of the last offset. If its bigger than zero,
	// we got a last line to write (ASCII only).
	unsigned int leftover = offset % e->octets_per_line;
	if (leftover > 0) {
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
		editor_render_ascii(e, row, offset - leftover, b);
		free(padding);
	}

	// clear everything up until the end
	charbuf_append(b, "\x1b[0K", 4);

#ifndef NDEBUG
	charbuf_appendf(b, "\x1b[0m\x1b[1;35m\x1b[1;80HRows: %d", e->screen_rows);
	charbuf_appendf(b, "\x1b[0K\x1b[2;80HOffset: %09x - %09x", start_offset, end_offset);
	charbuf_appendf(b, "\x1b[0K\x1b[3;80H(y,x)=(%d,%d)", e->cursor_y, e->cursor_x);
	unsigned int curr_offset = editor_offset_at_cursor(e);
	charbuf_appendf(b, "\x1b[0K\x1b[5;80H\x1b[0KLine: %d, cursor offset: %d (hex: %02x)", e->line, curr_offset, (unsigned char) e->contents[curr_offset]);
#endif
}

void editor_render_help(struct editor* e) {
	(void) e;
	struct charbuf* b = charbuf_create();
	clear_screen();
	charbuf_append(b, "\x1b[?25l", 6); // hide cursor
	charbuf_appendf(b, "This is hx, version %s\r\n\n", HX_VERSION);
	charbuf_appendf(b,
		"Available commands:\r\n"
		"\r\n"
		"CTRL+Q  : Quit immediately without saving.\r\n"
		"CTRL+S  : Save (in place).\r\n"
		"hjkl    : Vim like cursor movement.\r\n"
		"Arrows  : Also moves the cursor around.\r\n"
		"w       : Skip one group of bytes to the right.\r\n"
		"b       : Skip one group of bytes to the left.\r\n"
		"gg      : Move to start of file.\r\n"
		"G       : Move to end of file.\r\n"
		"x / DEL : Delete byte at cursor position.\r\n"
		"/       : Start search input.\r\n"
		"n       : Search for next occurrence.\r\n"
		"N       : Search for previous occurrence.\r\n"
		"u       : Undo the last action.\r\n"
		"CTRL+R  : Redo the last undone action.\r\n"
		"\r\n");
	charbuf_appendf(b,
		"a       : Append mode. Appends a byte after the current cursor position.\r\n"
		"A       : Append mode. Appends the literal typed keys (except ESC).\r\n"
		"i       : Insert mode. Inserts a byte at the current cursor position.\r\n"
		"I       : Insert mode. Inserts the literal typed keys (except ESC).\r\n"
		"r       : Replace mode. Replaces the byte at the current cursor position.\r\n"
		":       : Command mode. Commands can be typed and executed.\r\n"
		"ESC     : Return to normal mode.\r\n"
		"]       : Increment byte at cursor position with 1.\r\n"
		"[       : Decrement byte at cursor position with 1.\r\n"
		"End     : Move cursor to end of the offset line.\r\n"
		"Home    : Move cursor to the beginning of the offset line.\r\n"
		"\r\n"
	);
	charbuf_appendf(b,
		"Press any key to exit help.\r\n");

	charbuf_draw(b);

	read_key();
	clear_screen();
}



void editor_render_ruler(struct editor* e, struct charbuf* b) {
	// Nothing to see. No address, no byte, no percentage. It's all a plain
	// dark void right now. Oblivion. No data to see here, move along.
	if (e->content_length <= 0) {
		return;
	}

	char rulermsg[80]; // buffer for the actual message.
	char buf[20];      // buffer for the cursor positioning

	unsigned int offset_at_cursor = editor_offset_at_cursor(e);
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
	charbuf_appendf(b, "\x1b[%d;0H", e->screen_rows);

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
	charbuf_append(b, "\x1b[0m\x1b[0K", 8);
}


void editor_refresh_screen(struct editor* e) {
	struct charbuf* b = charbuf_create();

	charbuf_append(b, "\x1b[?25l", 6);
	charbuf_append(b, "\x1b[H", 3); // move the cursor top left

	if (e->mode &
			(MODE_REPLACE |
			 MODE_NORMAL |
			 MODE_APPEND |
			 MODE_APPEND_ASCII |
			 MODE_INSERT |
			 MODE_INSERT_ASCII)) {

		editor_render_contents(e, b);
		editor_render_status(e, b);

		// Ruler: move to the right of the screen etc.
		editor_render_ruler(e, b);
	} else if (e->mode & MODE_COMMAND) {
		// When in command mode, handle rendering different. For instance,
		// the cursor is placed at the bottom. Ruler is not required.
		// After moving the cursor, clear the entire line ([2K).
		charbuf_appendf(b,
			"\x1b[0m"    // reset attributes
			"\x1b[?25h"  // display cursor
			"\x1b[%d;1H" // move cursor down
			"\x1b[2K:",  // clear line, write a colon.
			e->screen_rows);
		charbuf_append(b, e->inputbuffer, e->inputbuffer_index);
	} else if (e->mode & MODE_SEARCH) {
		charbuf_appendf(b,
			"\x1b[0m"    // reset attributes
			"\x1b[?25h"  // display cursor
			"\x1b[%d;1H" // mvoe cursor down
			"\x1b[2K/",  // clear line, write a slash.
			e->screen_rows);
		charbuf_append(b, e->inputbuffer, e->inputbuffer_index);
	}

	charbuf_draw(b);
	charbuf_free(b);
}


void editor_insert_byte(struct editor* e, char x, bool after) {
	int offset = editor_offset_at_cursor(e);
	editor_insert_byte_at_offset(e, offset, x, after);

	if (after) {
		action_list_add(e->undo_list, ACTION_APPEND, offset, x);
	} else {
		action_list_add(e->undo_list, ACTION_INSERT, offset, x);
	}
}

void editor_insert_byte_at_offset(struct editor* e, unsigned int offset, char x, bool after) {
	// We are inserting a single character. Reallocate memory to contain
	// this extra byte.
	e->contents = realloc(e->contents, e->content_length + 1);

	if (after && e->content_length) { // append is the same as insert when buffer is empty
		offset++;
	}
	//          v
	// |t|e|s|t|b|y|t|e|s|...
	// |t|e|s|t|_|b|y|t|e|s|...
	memmove(e->contents + offset + 1, e->contents + offset, e->content_length - offset);

	e->contents[offset] = x;

	// Increase the content length since we inserted a character.
	e->content_length++;

	e->dirty = true;

}


void editor_replace_byte(struct editor* e, char x) {
	unsigned int offset = editor_offset_at_cursor(e);
	unsigned char prev = e->contents[offset];
	e->contents[offset] = x;
	editor_move_cursor(e, KEY_RIGHT, 1);
	editor_statusmessage(e, STATUS_INFO, "Replaced byte at offset %09x with %02x", offset, (unsigned char) x);
	e->dirty = true;

	action_list_add(e->undo_list, ACTION_REPLACE, offset, prev);
}

void editor_process_command(struct editor* e, const char* cmd) {
	// Command: go to base 10 offset
	bool b = is_pos_num(cmd);
	if (b) {
		int offset = str2int(cmd, 0, e->content_length, e->content_length - 1);
		editor_scroll_to_offset(e, offset);
		editor_statusmessage(e, STATUS_INFO, "Positioned to offset 0x%09x (%d)", offset, offset);
		return;
	}

	// Command: go to hex offset
	if (cmd[0] == '0' && cmd[1] == 'x') {
		const char* ptr = &cmd[2];
		if (!is_hex(ptr)) {
			editor_statusmessage(e, STATUS_ERROR, "Error: %s is not valid base 16", ptr);
			return;
		}

		int offset = hex2int(ptr);
		editor_scroll_to_offset(e, offset);
		editor_statusmessage(e, STATUS_INFO, "Positioned to offset 0x%09x (%d)", offset, offset);
		return;
	}

	if (strncmp(cmd, "w", INPUT_BUF_SIZE) == 0) {
		editor_writefile(e);
		return;
	}

	if (strncmp(cmd, "q", INPUT_BUF_SIZE) == 0) {
		if (e->dirty) {
			editor_statusmessage(e, STATUS_ERROR, "No write since last change (add ! to override)", cmd);
			return;
		} else {
			exit(0);
		}
	}
	
	if (strncmp(cmd, "q!", INPUT_BUF_SIZE) == 0) {
		exit(0);
		return;
	}

	if (strncmp(cmd, "help", INPUT_BUF_SIZE) == 0) {
		editor_render_help(e);
		return;
	}

	// Check if we want to set an option at runtime. The first three bytes are
	// checked first, then the rest is parsed.
	if (strncmp(cmd, "set", 3) == 0) {
		char setcmd[INPUT_BUF_SIZE] = {0};
		int setval = 0;
		int items_read = sscanf(cmd, "set %[a-z]=%d", setcmd, &setval);
		// command name_____________________/    /
		// command value________________________/

		if (items_read != 2) {
			editor_statusmessage(e, STATUS_ERROR, "set command format: `set cmd=num`");
			return;
		}

		if (strcmp(setcmd, "octets") == 0 || strcmp(setcmd, "o") == 0) {
			int octets = clampi(setval, 16, 64);

			clear_screen();
			int offset = editor_offset_at_cursor(e);
			e->octets_per_line = octets;
			editor_scroll_to_offset(e, offset);

			editor_statusmessage(e, STATUS_INFO, "Octets per line set to %d", octets);

			return;
		}

		// Set the grouping of bytes to a different value.
		if (strcmp(setcmd, "grouping") == 0 || strcmp(setcmd, "g") == 0) {
			int grouping = clampi(setval, 4, 16);
			clear_screen();
			e->grouping = grouping;

			editor_statusmessage(e, STATUS_INFO, "Byte grouping set to %d", grouping);
			return;
		}

		editor_statusmessage(e, STATUS_ERROR, "Unknown option: %s", setcmd);
		return;
	}

	editor_statusmessage(e, STATUS_ERROR, "Command not found: %s", cmd);
}

/*
 * Reads inputstr and inserts 1 byte per "object" into parsedstr.
 * parsedstr can then be used directly to search the file.
 * err_info is a pointer into inputstr to relevant error information.
 *
 * Objects are:
 *  - ASCII bytes entered normally e.g. 'a', '$', '2'.
 *  - "\xXY" where X and Y match [0-9a-fA-F] (hex representation of bytes).
 *  - "\\" which represents a single '\'
 *
 * Both parsedstr must be able to fit all the characters in inputstr,
 * including the terminating null byte.
 *
 * On success, PARSE_SUCCESS is returned and parsedstr can be used. On failure,
 * an error from parse_errors is returned, err_info is set appropriately,
 * and parsedstr is undefined.
 *
 * err_info:
 *  PARSE_INVALID_HEX     - pointer "XY..." where XY is the invalid hex code.
 *  PARSE_INVALID_ESCAPE  - pointer to "X..." where X is the invalid character
 *                          following \.
 *  other errors    - inputstr.
 *  success         - inputstr.
 */
static int parse_search_string(const char* inputstr, char* parsedstr,
                               const char** err_info) {
	// Used to pass values to hex2bin.
	char hex[3] = {'\0'};
	*err_info = inputstr;

	while (*inputstr != '\0') {
		if (*inputstr == '\\') {
			++inputstr;
			switch (*(inputstr)) {
			case '\0':  // We have "\\0"
				*parsedstr = '\0';
				return PARSE_INCOMPLETE_BACKSLASH;
			case '\\':  // We have: "\\".
				*parsedstr = '\\';
				++inputstr;
				break;
			case 'x':  // We have: "\x".
				++inputstr;

				if (*inputstr == '\0'
				    || *(inputstr + 1) == '\0') {
					*parsedstr = '\0';
					return PARSE_INCOMPLETE_HEX;
				}

				if (!isxdigit(*inputstr)
				    || !isxdigit(*(inputstr + 1))) {
					*parsedstr = '\0';
					*err_info = inputstr;
					return PARSE_INVALID_HEX;
				}

				// We have: "\xXY" (valid X, Y).
				memcpy(hex, inputstr, 2);
				*parsedstr = hex2bin(hex);

				inputstr += 2;
				break;
			default:
				// No need to increment - we're failing.
				*err_info = inputstr;
                                return PARSE_INVALID_ESCAPE;
			}
		} else {
			// Nothing interesting.
			*parsedstr = *inputstr;
			++inputstr;
		}

		++parsedstr;
	}

	*parsedstr = '\0';

	return PARSE_SUCCESS;
}

void editor_process_search(struct editor* e, const char* str, enum search_direction dir) {
	// Empty search string, reset the searchstr to an empty one and
	// stops searching anything.
	if (strncmp(str, "", INPUT_BUF_SIZE) == 0) {
		strncpy(e->searchstr, str, INPUT_BUF_SIZE);
		return;
	}

	// new search query, update searchstr.
	if (strncmp(str, e->searchstr, INPUT_BUF_SIZE) != 0) {
		strncpy(e->searchstr, str, INPUT_BUF_SIZE);
	}

	char parsedstr[INPUT_BUF_SIZE];
	const char* parse_err;
	int parse_errno = parse_search_string(str, parsedstr, &parse_err);
	switch (parse_errno) {
	case PARSE_INCOMPLETE_BACKSLASH:
		editor_statusmessage(e, STATUS_ERROR,
				     "Nothing follows '\\' in search"
				     " string: %s", str);
		return;
	case PARSE_INCOMPLETE_HEX:
		editor_statusmessage(e, STATUS_ERROR,
				     "Incomplete hex value at end"
				     " of search string: %s", str);
		return;
	case PARSE_INVALID_HEX:
		editor_statusmessage(e, STATUS_ERROR,
				     "Invalid hex value (\\x%c%c)"
				     " in search string: %s",
				     *parse_err, *(parse_err + 1), str);
		return;
	case PARSE_INVALID_ESCAPE:
		editor_statusmessage(e, STATUS_ERROR,
				     "Invalid character after \\ (%c)"
				     " in search string: %s",
				     *parse_err, str);
		return;
	case PARSE_SUCCESS:
		// All good.
		break;
	}

	unsigned int current_offset = editor_offset_at_cursor(e);

	if (dir == SEARCH_FORWARD) {
		current_offset++;
		for (; current_offset < e->content_length; current_offset++) {
			if (memcmp(e->contents + current_offset, parsedstr,
				   strlen(parsedstr)) == 0) {
				editor_statusmessage(e, STATUS_INFO, "");
				editor_scroll_to_offset(e, current_offset);
				return;
			}
		}
	} else if (dir == SEARCH_BACKWARD) {
		// if we are already at the beginning of the file, no use for searching
		// backwards any more.
		if (current_offset == 0) {
			editor_statusmessage(e, STATUS_INFO, "Already at start of the file");
			return;
		}

		// Decrement the offset once, or else we keep comparing the current offset
		// position with an already found string, keeping us in the same position.
		current_offset--;

		// Since we are working with unsigned integers, do this trick in the for-statement
		// to 'include' the zero offset with comparing.
		for (; current_offset-- != 0; ) {
			if (memcmp(e->contents + current_offset, parsedstr,
				   strlen(parsedstr)) == 0) {
				editor_statusmessage(e, STATUS_INFO, "");
				editor_scroll_to_offset(e, current_offset);
				return;
			}
		}
	}

	editor_statusmessage(e, STATUS_WARNING, "String not found: '%s'", str);
}

int editor_read_hex_input(struct editor* e, char* out) {
	// Declared static to avoid unnecessary 'global' variable state. We're
	// only interested in this data in this function anyway. For now.
	static int  hexstr_idx = 0; // what index in hexstr are we updating?
	static char hexstr[2 + 1];  // the actual string updated with the keypress.

	int next = read_key();

	if (next == KEY_ESC) {
		// escape the current mode to NORMAL, reset the hexstr and index so
		// we can start afresh with the next REPLACE mode.
		editor_setmode(e, MODE_NORMAL);
		memset(hexstr, '\0', 3);
		hexstr_idx = 0;
		return -1;
	}

	// Check if the input character was a valid hex value. If not, return prematurely.
	if (!isprint(next)) {
		editor_statusmessage(e, STATUS_ERROR, "Error: unprintable character (%02x)", next);
		return -1;
	}
	if (!isxdigit(next)) {
		editor_statusmessage(e, STATUS_ERROR, "Error: '%c' (%02x) is not valid hex", next, next);
		return -1;
	}

	// hexstr[0] will contain the first typed character
	// hexstr[1] contains the second.
	hexstr[hexstr_idx++] = next;

	if (hexstr_idx >= 2) {
		// Parse the hexstr to an actual char. Example: '65' will return 'e'.
		*out = hex2bin(hexstr);
		memset(hexstr, '\0', 3);
		hexstr_idx = 0;
		return 0;
	}

	return -1;
}


int editor_read_string(struct editor* e, char* dst, int len) {
	// if we hit enter, set the mode to normal mode, execute
	// the command, and possibly set a statusmessage.
	int c = read_key();
	if (c == KEY_ENTER || c == KEY_ESC) {
		editor_setmode(e, MODE_NORMAL);
		// copy the 'temp' inputbuffer to the dst.
		strncpy(dst, e->inputbuffer, len);
		// After copying, reset the index and the inputbuffer
		e->inputbuffer_index = 0;
		memset(e->inputbuffer,  '\0', sizeof(e->inputbuffer));
		return c;
	}

	// backspace characters by setting characters to 0
	if (c == KEY_BACKSPACE && e->inputbuffer_index > 0) {
		e->inputbuffer_index--;
		e->inputbuffer[e->inputbuffer_index] = '\0';
		return c;
	}

	// Safety guard. Our inputbuffer size is limited so stop
	// incrementing our buffer index after this maximum.
	if ((size_t) e->inputbuffer_index >= sizeof(e->inputbuffer)) {
		return c;
	}

	// if the buffer is empty (no command), and we're
	// still backspacing, return to normal mode.
	if (c == KEY_BACKSPACE && e->inputbuffer_index == 0) {
		editor_setmode(e, MODE_NORMAL);
		return c;
	}

	// Only act on printable characters.
	if (!isprint(c)) {
		return c;
	}

	e->inputbuffer[e->inputbuffer_index++] = c;
	return c;
}


void editor_process_keypress(struct editor* e) {
	if (e->mode & (MODE_INSERT | MODE_APPEND)) {
		char out = 0;
		if (editor_read_hex_input(e, &out) != -1) {
			editor_insert_byte(e, out, e->mode & MODE_APPEND);
			editor_move_cursor(e, KEY_RIGHT, 1);
		}
		return;
	}

	// Append or insert 'literal' ASCII values.
	if (e->mode & (MODE_INSERT_ASCII | MODE_APPEND_ASCII)) {
		char c = read_key();
		if (c == KEY_ESC) {
			editor_setmode(e, MODE_NORMAL); return;
		}
		editor_insert_byte(e, c, e->mode & MODE_APPEND_ASCII);
		editor_move_cursor(e, KEY_RIGHT, 1);
		return;
	}

	if (e->mode & MODE_REPLACE) {
		char out = 0;
		if (e->content_length > 0) {
			if (editor_read_hex_input(e, &out) != -1) {
				editor_replace_byte(e, out);
			}
			return;
		} else {
			editor_statusmessage(e, STATUS_ERROR, "File is empty, nothing to replace");
		}
	}

	if (e->mode & MODE_COMMAND) {
		// Input manual, typed commands.
		char cmd[INPUT_BUF_SIZE];
		int c = editor_read_string(e, cmd, INPUT_BUF_SIZE);
		if (c == KEY_ENTER && strlen(cmd) > 0) {
			editor_process_command(e, cmd);
		}
		return;
	}

	if (e->mode & MODE_SEARCH) {
		char search[INPUT_BUF_SIZE];
		int c = editor_read_string(e, search, INPUT_BUF_SIZE);
		if (c == KEY_ENTER && strlen(search) > 0) {
			editor_process_search(e, search, SEARCH_FORWARD);
		}
		return;
	}


	// When in normal mode, start reading 'raw' keys.
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
	if (e->mode & MODE_NORMAL) {
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
		case 'n': editor_process_search(e, e->searchstr, SEARCH_FORWARD); break;
		case 'N': editor_process_search(e, e->searchstr, SEARCH_BACKWARD); break;

		case 'a': editor_setmode(e, MODE_APPEND);       return;
		case 'A': editor_setmode(e, MODE_APPEND_ASCII); return;
		case 'i': editor_setmode(e, MODE_INSERT);       return;
		case 'I': editor_setmode(e, MODE_INSERT_ASCII); return;
		case 'r': editor_setmode(e, MODE_REPLACE);      return;
		case ':': editor_setmode(e, MODE_COMMAND);      return;
		case '/': editor_setmode(e, MODE_SEARCH);       return;

		case 'u':         editor_undo(e); return;
		case KEY_CTRL_R : editor_redo(e); return;

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

		case KEY_HOME: e->cursor_x = 1; return;
		case KEY_END:  editor_move_cursor(e, KEY_RIGHT, e->octets_per_line - e->cursor_x); return;

		case KEY_CTRL_U:
		case KEY_PAGEUP:   editor_scroll(e, -(e->screen_rows) + 2); return;

		case KEY_CTRL_D:
		case KEY_PAGEDOWN: editor_scroll(e, e->screen_rows - 2); return;
		}
	}
}

void editor_undo(struct editor* e) {
	struct action* last_action = e->undo_list->curr;

	if (e->undo_list->curr_status == AFTER_TAIL) {
		// Move back to undo the previous action.
		action_list_move(e->undo_list, -1);
	}

	if (e->undo_list->curr_status != NODE) {
		// Either curr is before head or the list is empty.
		editor_statusmessage(e, STATUS_INFO, "No action to undo");
		return;
	}

	// Save the old contents in case we're undoing a replace.
	char old_contents = e->contents[last_action->offset];
	switch (last_action->act) {
	case ACTION_APPEND:
		editor_delete_char_at_offset(e, last_action->offset+1);
		break;
	case ACTION_DELETE:
		editor_insert_byte_at_offset(e, last_action->offset, last_action->c, false);
		break;
	case ACTION_REPLACE:
		e->contents[last_action->offset] = last_action->c;
		last_action->c = old_contents;
		break;
	case ACTION_INSERT:
		editor_delete_char_at_offset(e, last_action->offset);
		break;
	}

	// move cursor to the undone action's offset.
	editor_scroll_to_offset(e, last_action->offset);

	// Move to the previous action.
	action_list_move(e->undo_list, -1);

	editor_statusmessage(e, STATUS_INFO,
		"Reverted '%s' at offset %d to byte '%02x' (%d left)",
			action_type_name(last_action->act),
			last_action->offset,
			last_action->c,
			action_list_curr_pos(e->undo_list));
}

void editor_redo(struct editor* e) {
	if (e->undo_list->curr_status == AFTER_TAIL
	    || e->undo_list->curr_status == NOTHING) {
		editor_statusmessage(e, STATUS_INFO, "No action to redo");
		return;
	}

	struct action* next_action = NULL;

	if (e->undo_list->curr_status == BEFORE_HEAD) {
		next_action = e->undo_list->head;
	} else {  // == NODE
		next_action = e->undo_list->curr->next;
	}

	if (next_action == NULL) {
		// May happen when curr is tail.
		editor_statusmessage(e, STATUS_INFO, "No action to redo");
		return;
	}

	// Save the old contents in case we're redoing a replace.
	char old_contents = e->contents[next_action->offset];
	switch (next_action->act) {
	case ACTION_APPEND:
		editor_insert_byte_at_offset(e, next_action->offset, next_action->c, true);
		break;
	case ACTION_DELETE:
		editor_delete_char_at_offset(e, next_action->offset);
		break;
	case ACTION_REPLACE:
		e->contents[next_action->offset] = next_action->c;
		next_action->c = old_contents;
		break;
	case ACTION_INSERT:
		editor_insert_byte_at_offset(e, next_action->offset, next_action->c, false);
		break;
	}

	// Move cursor to the redone action's offset.
	editor_scroll_to_offset(e, next_action->offset);

	// Move to the next action.
	action_list_move(e->undo_list, 1);

	editor_statusmessage(e, STATUS_INFO,
		"Redone '%s' at offset %d to byte '%02x' (%d left)",
			action_type_name(next_action->act),
			next_action->offset,
			next_action->c,
			action_list_size(e->undo_list)
			- action_list_curr_pos(e->undo_list));
}

/*
 * Initializes editor struct with some default values.
 */
struct editor* editor_init() {
	struct editor* e = malloc(sizeof(struct editor));

	e->octets_per_line = 16;
	e->grouping = 2;

	e->line = 0;
	e->cursor_x = 1;
	e->cursor_y = 1;
	e->filename = NULL;
	e->contents = NULL;
	e->content_length = 0;

	memset(e->status_message, '\0', sizeof(e->status_message));

	e->mode = MODE_NORMAL;

	memset(e->inputbuffer, '\0', sizeof(e->inputbuffer));
	e->inputbuffer_index = 0;

	memset(e->searchstr, '\0', sizeof(e->searchstr));

	get_window_size(&(e->screen_rows), &(e->screen_cols));

	e->undo_list = action_list_init();

	return e;
}

void editor_free(struct editor* e) {
	action_list_free(e->undo_list);
	free(e->filename);
	free(e->contents);
	free(e);
}

