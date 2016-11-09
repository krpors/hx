/*
 * This file is part of hx - a hex editor for the terminal.
 *
 * Copyright (c) 2016 Kevin Pors. See LICENSE for details.
 */
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

// Terminal IO settings. Used to reset it when exiting to prevent terminal
// data garbling. See enable/disable_raw_mode().
static struct termios orig_termios;

bool ishex(const char c) {
	return
		(c >= '0' && c <= '9') ||
		(c >= 'A' && c <= 'F') ||
		(c >= 'a' && c <= 'f');
}

int hex2bin(const char* s) {
	int ret=0;
	for(int i = 0; i < 2; i++) {
		char c = *s++;
		int n=0;
		if( '0' <= c && c <= '9')  {
			n = c-'0';
		} else if ('a' <= c && c <= 'f') {
			n = 10 + c - 'a';
		} else if ('A' <= c && c <= 'F') {
			n = 10 + c - 'A';
		}
		ret = n + ret*16;
	}
	return ret;
}

bool is_pos_num(const char* s) {
	const char* ptr = s;
	while(*++ptr) {
		if (!isdigit(*ptr)) {
			return false;
		}
	}
	return true;
}

int str2int(const char* s, int min, int max, int def) {
	char* endptr;
	uintmax_t x = strtoimax(s, &endptr, 10);
	if (errno  == ERANGE) {
		return def;
	}
	if (x < min || x > max) {
		return def;
	}
	return x;
}

/**
 * Reads keypresses from stdin, and processes them accordingly. Escape sequences
 * will be read properly as well (e.g. DEL will be the bytes 0x1b, 0x5b, 0x33, 0x7e).
 * The returned integer will contain either one of the enum values, or the key pressed.
 *
 * read_key() will only return the correct key code, or -1 when anything fails.
 */
int read_key() {
	char c;
	ssize_t nread;
	// check == 0 to see if EOF.
	while ((nread = read(STDIN_FILENO, &c, 1)) == 0);
	if (nread == -1) {
		// This error may happen when a SIGWINCH is received by resizing the terminal.
		// The read() call is interrupted and will fail here. In that case, just return
		// -1 prematurely and continue the main loop. In all other cases, this will
		// be unexpected so inform the user that something has happened.
		if (errno == EINTR) {
			return -1;
		}

		fprintf(stderr, "Unable to read from stdin: %s\n", strerror(errno));
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
					case '3': return KEY_DEL;
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

void enable_raw_mode() {
	// only enable raw mode when stdin is a tty.
	if (!isatty(STDIN_FILENO)) {
		perror("Input is not a TTY");
		exit(1);
	}

	// Disable raw mode when we exit hx normally.
	//atexit(editor_exit);

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
	// 100 ms timeout (unit is tens of second). Do not set this to 0 for
	// whatever reason, because this will skyrocket the cpu usage to 100%!
	raw.c_cc[VTIME] = 1;

    // put terminal in raw mode after flushing
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) {
		perror("Unable to set terminal to raw mode");
		exit(1);
	}
}

void disable_raw_mode() {
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}


void clear_screen() {
	// clear the colors, move the cursor up-left, clear the screen.
	char stuff[80];
	int bw = snprintf(stuff, 80, "\x1b[0m\x1b[H\x1b[2J");
	if (write(STDOUT_FILENO, stuff, bw) == -1) {
		perror("Unable to clear screen");
	}
}

