/*
 * This file is part of hx - a hex editor for the terminal.
 *
 * Copyright (c) 2016 Kevin Pors. See LICENSE for details.
 */

#ifndef _HX_EDITOR_H
#define _HX_EDITOR_H

#include "charbuf.h"

#include <stdbool.h>

/**
 * Mode the editor can be in.
 */
enum editor_mode {
	MODE_NORMAL,  // normal mode i.e. for navigating, commands.
	MODE_INSERT,  // insert values at cursor position.
	MODE_REPLACE, // replace values at cursor position.
	MODE_COMMAND, // command input mode.
};

/**
 * Current status severity.
 */
enum status_severity {
	STATUS_INFO,    // appear as lightgray bg, black fg
	STATUS_WARNING, // appear as yellow bg, black fg
	STATUS_ERROR,   // appear as red bg, white fg
};

/**
 * This struct contains internal information of the state of the editor.
 */
struct editor {
	int octets_per_line; // Amount of octets (bytes) per line. Ideally multiple of 2.
	int grouping;        // Amount of bytes per group. Ideally multiple of 2.

	int line;        // The 'line' in the editor. Used for scrolling.
	int cursor_x;    // Cursor x pos on the current screen
	int cursor_y;    // Cursor y pos on the current screen
	int screen_rows; // amount of screen rows after init
	int screen_cols; // amount of screen columns after init

	enum editor_mode mode; // mode the editor is in

	bool  dirty;          // whether the buffer is modified
	char* filename;       // the filename currently open
	char* contents;       // the file's contents
	int   content_length; // length of the contents

	enum status_severity status_severity;     // status severity
	char                 status_message[120]; // status message

	char cmdbuffer[80];  // command input buffer.
	int cmdbuffer_index; // the index of the current typed key shiz.
};

/**
 * Initializes the editor struct with basic values.
 */
struct editor* editor_init();

/**
 * Finds the cursor position at the given offset, taking the lines into account.
 * The result is set to the pointers `x' and `y'. We can therefore 'misuse' this
 * to set the cursor position of the editor to a given offset.
 *
 * Note that this function will NOT scroll the editor to the proper line.
 */
void editor_cursor_at_offset(struct editor* e, int offset, int* x, int *y);

/**
 * Deletes the character (byte) at the current cursor position (in other
 * words, the current offset the cursor is at).
 */
void editor_delete_char_at_cursor(struct editor* e);
void editor_free(struct editor* e);
void editor_increment_byte(struct editor* e, int amount);

/**
 * Moves the cursor. The terminal cursor positions are all 1-based, so we
 * take that into account. When we scroll past boundaries (left, right, up
 * and down) we react accordingly. Note that the cursor_x/y are also 1-based,
 * and we calculate the actual position of the hex values by incrementing it
 * later on with the address size, amount of grouping spaces etc.
 */
void editor_move_cursor(struct editor* e, int dir, int amount);

/**
 * Gets the current offset at which the cursor is.
 */
int editor_offset_at_cursor(struct editor* e);

/**
 * Opens a file denoted by `filename', or exit if the file cannot be opened.
 * The editor struct is used to contain the contents and other metadata
 * about the file being opened.
 */
void editor_openfile(struct editor* e, const char* filename);

/**
 * Processes a manual command input when the editor mode is set
 * to MODE_COMMAND. `c' is the character read by read_key().
 */
void editor_process_cmdinput(struct editor* e, char c);

/**
 * Processes a keypress accordingly.
 */
void editor_process_keypress(struct editor* e);

/**
 * Reads two characters from the keyboard, attempts to parse them as a valid
 * hex value and returns it as a char. For example, first char read could be
 * 'F', second '3'. The two inputs are then returned as the char 0xf3. Will
 * return -1 if the input is invalid hexadecimal, and updates the editor's
 * status bar with an error. `c' is the initial character being read by
 * read_key(), since we're looping the editor_process_keypress().
 */
int editor_read_hex_input(struct editor* e, char initial, char* output);

/**
 * Renders the given ASCII string, `asc' to the buffer `b'. The `rownum'
 * specified should be the row number being rendered in an iteration in
 * editor_render_contents. This function will render the selected byte
 * with a different color in the ASCII row to easily identify which
 * byte is being highlighted.
 */
void editor_render_ascii(struct editor* e, int rownum, const char* ascii, struct charbuf* b);

/**
 * Renders the contents of the current state of the editor `e'
 * to the buffer `b'.
 */
void editor_render_contents(struct editor* e, struct charbuf* b);

/*
 * Renders a ruler at the bottom right part of the screen, containing
 * the current offset in hex and in base 10, the byte at the current
 * cursor position, and how far the cursor is in the file (as a percentage).
 */
void editor_render_ruler(struct editor* e, struct charbuf* buf);

/**
 * Renders the status line to the buffer `b'.
 */
void editor_render_status(struct editor* e, struct charbuf* buf);

/**
 * Refreshes the screen. It uses a temporary buffer to write everything that's
 * eligible for display to an internal buffer, and then 'draws' it to the screen
 * in one call.
 */
void editor_refresh_screen(struct editor* e);

/**
 * Replaces the byte(char) at the current selected offset with the given char.
 */
void editor_replace_byte(struct editor* e, char x);

/**
 * Scrolls the editor by updating the `line' accordingly, within
 * the bounds of the readable parts of the buffer.
 */
void editor_scroll(struct editor* e, int units);

/**
 * Scrolls the editor to a particular given offset. The given offset
 * can be given any value, the function will limit it to the upper and
 * lower bounds of what can be displayed.
 *
 * The cursor will be centered on the screen.
 */
void editor_scroll_to_offset(struct editor* e, unsigned int offset);

/**
 * Sets editor to mode to one of the modes defined in the enum.
 */
void editor_setmode(struct editor *e, enum editor_mode mode);

/**
 * Sets statusmessage, including color depending on severity.
 */
int editor_statusmessage(struct editor* e, enum status_severity s, const char* fmt, ...);

/**
 * Writes the contents of the editor's buffer the to the same filename.
 */
void editor_writefile(struct editor* e);

#endif // _HX_EDITOR_H
