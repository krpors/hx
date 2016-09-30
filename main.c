#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>

#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>

// Declarations.
void enable_raw_mode();
void disable_raw_mode();
int read_key();

// Terminal IO settings. Used to reset it when exiting to prevent terminal
// data garbling.
struct termios orig_termios;

// Key enumerations
enum KEY_CODES {
	KEY_NULL     = 0,
	KEY_CTRL_Q   = 0x11, // DC1, to exit the program.
	KEY_ESC      = 0x1b, // ESC, for things like keys up, down, left, right, delete, ...

	// 'Virtual keys', i.e. not corresponding to terminal escape sequences
	// or any other ANSI stuff. Merely to identify keys returned by read_key().
	VKEY_UP      = 1000, // [A
	VKEY_DOWN,           // [B
	VKEY_RIGHT,          // [C
	VKEY_LEFT,           // [D
	VKEY_HOME,           // [H
	VKEY_END,            // [F
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
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);

	printf("\x1b[7mThank you for using hx!\x1b[0K");
	fflush(stdout);
}


void clear_screen() {
	if (write(STDOUT_FILENO, "\x1b[2J", 4) == -1) {
		perror("Unable to clear screen");
	}
}

void set_cursor_pos(uint8_t x, uint8_t y) {
	printf("\x1b[%d;%dH", x, y);
}

void move_cursor(int dir) {
	static const int LEN = 4;
	char cruft[LEN];

	switch (dir) {
	case VKEY_UP:    strncpy(cruft, "\x1b[1A", LEN); break;
	case VKEY_RIGHT: strncpy(cruft, "\x1b[1C", LEN); break;
	case VKEY_DOWN:  strncpy(cruft, "\x1b[1B", LEN); break;
	case VKEY_LEFT:  strncpy(cruft, "\x1b[1D", LEN); break;
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
void buffer_delete(struct buffer* buf) {
	free(buf->contents);
	free(buf);
}

/**
 * Appends `what' to the buffer, writing at most `len' bytes. Note that
 * if we use snprintf() to format a particular string, we have to subtract
 * 1 from the `len', to discard the null terminator character.
 */
void buffer_append(struct buffer* buf, const char* what, size_t len) {
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
 * Opens a file denoted by `filename', or exit if the file cannot be opened.
 */
void openfile(const char* filename) {
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
	unsigned char* contents = malloc(sizeof(unsigned char) * size);

	if (fread(contents, size, 1, fp) <= 0) {
		perror("Unable to read bytes");
		free(contents);
		exit(1);
	}

	// write formatted data to a string
	struct buffer* buf = buffer_create();

	// Variable used to check how many bytes were written using sprintf,
	// excluding the terminating null byte.
	int bytes_written = 0;

	for(int i = 0; i < size; i++) {
		char str[1000] = {0};

		if (isprint(contents[i])) {
			bytes_written = sprintf(str, "%09x: %c = %02x\n", i, contents[i], contents[i]);
		} else {
			bytes_written = sprintf(str, "%09x: \x1b[31;47m.\x1b[0m = %02x\n", i, contents[i]);
		}

		buffer_append(buf, str, bytes_written);
	}
	if (write(STDOUT_FILENO, buf->contents, buf->len) == -1) {
		perror("Unable to write to stdout");
		exit(1);
	}

	buffer_delete(buf);
	free(contents);
	fclose(fp);
}

/**
 * Reads keypresses from stdin, and processes them accordingly. Escape sequences
 * will be read properly as well (e.g. DEL will be the bytes 0x1b, 0x5b, 0x33, 0x7e).
 * The returned integer will contain either one of the enum values, or the key pressed.
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
			printf("Just an escape!\n");
			return KEY_ESC;
		}
		if (read(STDIN_FILENO, seq + 1, 1) == 0) {
			printf("Just an escape, 2!\n");
			return KEY_ESC;
		}

		if (seq[0] == '[') {
			switch (seq[1]) {
			case 'A':
				move_cursor(VKEY_UP);
				return VKEY_UP;
			case 'B':
				move_cursor(VKEY_DOWN);
				return VKEY_DOWN;
			case 'C':
				move_cursor(VKEY_RIGHT);
				return VKEY_RIGHT;
			case 'D':
				move_cursor(VKEY_LEFT);
				return VKEY_LEFT;
			}
		}

		break;
	case KEY_CTRL_Q:
		exit(0);
		break;
	default:
		//write(STDOUT_FILENO, &c, 1);
		//printf("%x\n", (unsigned char)c);
		break;
	}

	return c;
}

/**
 * Simply prints the file's contents as hex.
 */
void print_hex(const char* filename) {
	FILE* fp = fopen(filename, "rb");
	if (fp == NULL) {
		perror("Unable to open file");
		exit(EXIT_FAILURE);
	}

	char full[16 + 1];  // e.g. 'MIT License..Cop\0'
	char asci[ 1 + 1];  // e.g. 'a\0'
	char chex[ 2 + 1];  // e.g. '4f\0'

	int c;
	int offset;
	for (offset = 0; (c = fgetc(fp)) != EOF; offset++) {
		if (offset % 16 == 0) {
			printf("%07x: ", offset);
			memset(&full, 0, sizeof(full));
		}

		if (isprint(c)) {
			snprintf(asci, 2, "%c", (unsigned char) c);
		} else {
			snprintf(asci, 2, ".");
		}
		// concat it
		strcat(full, asci);

		snprintf(chex, 3, "%02x", (unsigned char) c);
		printf("%s", chex);

		if ((offset+1) % 2 == 0) {
			printf(" ");
		}

		if ((offset+1) % 16 == 0) {
			// TODO: print the string of chars.
			printf(" %s\n",full);
			//printf("\n");
		}
	}

	// calc offset remainder, to pad some leftover hexes.
	int leftover_bytes = (16 - offset % 16);
	printf("\n\nPadding chars amount = %d\n", leftover_bytes);
	if (leftover_bytes > 0) {
		printf("%s\n", full);
	}

	fclose(fp);
}


int main(int argc, char* argv[]) {
#if 0
	if (argc != 2) {
		fprintf(stderr, "expected arg\n");
		exit(2);
	}

	openfile(argv[1]);
	//print_hex(argv[1]);
#endif

	enable_raw_mode();

	clear_screen();
	while (true) {
		read_key();
	}
	return EXIT_SUCCESS;
}
