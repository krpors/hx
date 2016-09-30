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

void enable_raw_mode() {
	struct termios orig_termios;
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

void clear_screen() {
	printf("\x1b[2J");
}

void set_cursor_pos(uint8_t x, uint8_t y) {
	printf("\x1b[%d;%dH", x, y);
}

void move_cursor(int dir) {
#if 0
	switch (dir) {
	case 0:
		write(STDOUT_FILENO, "\x1b[1A", 4);
		break;
	case 1:
		write(STDOUT_FILENO, "\x1b[1C", 4);
		break;
	case 2:
		write(STDOUT_FILENO, "\x1b[1B", 4);
		break;
	case 3:
		write(STDOUT_FILENO, "c\x1b[1D", 4);
		break;
	default:
		break;
	}
#endif
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

void buffer_delete(struct buffer* buf) {
	free(buf->contents);
	free(buf);
}

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

	for(int i = 0; i < size; i++) {
		char str[10];
		// XXX:  snprintf adds a null byte.. we don't want that!
		snprintf(str, 10, "%09x", i);
		buffer_append(buf, str, 10);
		buffer_append(buf, ": ", 2);
		char hex[7];
		if (contents[i] < '!' || contents[i] > '~') {
			snprintf(hex, 7, ". = %02x\n", contents[i]);
		} else {
			snprintf(hex, 7, "%c = %02x\n", contents[i], contents[i]);
		}
		buffer_append(buf, hex, 7);
		buffer_append(buf, "\n", 1);
	}

	if (write(STDOUT_FILENO, buf->contents, buf->len) == -1) {
		perror("Unable to write to stdout");
		exit(1);
	}

	buffer_delete(buf);
	free(contents);
	fclose(fp);
}

int process_keypress() {
	char c;
	ssize_t nread;
	// check == 0 to see if EOF.
	while ((nread = read(STDIN_FILENO, &c, 1)) == 0);
	if (nread == -1) {
		perror("Unable to read from stdin");
		exit(2);
	}

	switch (c) {
	case 'j':
		move_cursor(2);
		break;
	case 'k':
		move_cursor(0);
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
	if (argc != 2) {
		fprintf(stderr, "expected arg\n");
		exit(2);
	}

	//openfile(argv[1]);
	print_hex(argv[1]);
#if 0

	enable_raw_mode();

	int rows, cols;

	get_window_size(&rows, &cols);
	clear_screen();
	set_cursor_pos(rows - 1, 0);
	printf("\x1b[7mHi thar!\x1b[0K");

	while (true) {
		int ch = process_keypress();
		process_keypress();
		fflush(stdout);
	}
#endif
	return EXIT_SUCCESS;
}
