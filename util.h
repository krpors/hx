/*
 * This file is part of hx - a hex editor for the terminal.
 *
 * Copyright (c) 2016 Kevin Pors. See LICENSE for details.
 */

#ifndef HX_UTIL_H
#define HX_UTIL_H

#include <stdbool.h>
#include <termios.h>

// Key enumeration, returned by read_key().
enum key_codes {
	KEY_NULL      = 0,
	KEY_CTRL_D    = 0x04,
	KEY_CTRL_Q    = 0x11, // DC1, to exit the program.
	KEY_CTRL_S    = 0x13, // DC2, to save the current buffer.
	KEY_CTRL_U    = 0x15,
	KEY_ESC       = 0x1b, // ESC, for things like keys up, down, left, right, delete, ...
	KEY_ENTER     = 0x0d,
	KEY_BACKSPACE = 0x7f,

	// 'Virtual keys', i.e. not corresponding to terminal escape sequences
	// or any other ANSI stuff. Merely to identify keys returned by read_key().
	KEY_UP      = 1000, // [A
	KEY_DOWN,           // [B
	KEY_RIGHT,          // [C
	KEY_LEFT,           // [D
	KEY_DEL,            // . = 1b, [ = 5b, 3 = 33, ~ = 7e,
	KEY_HOME,           // [H
	KEY_END,            // [F
	KEY_PAGEUP,         // ??
	KEY_PAGEDOWN,       // ??
};

void enable_raw_mode();
void disable_raw_mode();
void clear_screen();
int  read_key();
int  hex2bin(const char* s);
bool get_window_size(int* rows, int* cols);

/*
 * Returns true when the given char can be successfully parsed as a positive
 * integer, or return false if otherwise.
 */
bool is_pos_num(const char* s);

/*
 * Returns true when the given string is a valid hexadecimal string, or false
 * if othwerise.
 */
bool is_hex(const char* s);

int hex2int(const char* s);

/*
 * Clamps the given integer i to the given min or max. If i is smaller than
 * min, min is returned. If i is larger than max, max is returned. In all
 * other cases, i is returned.
 */
int clampi(int i, int min, int max);

/*
 * Parses a string to an integer and returns it. In case of errors, the default
 * `def' will be returned. When the `min' > parsed_value > `max', then the
 * default `def' will also be returned.
 *
 * XXX: probably return some error indicator instead, and exit with a message?
 */
int str2int(const char* s, int min, int max, int def);

#endif // HX_UTIL_H
