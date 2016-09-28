#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>

#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>

void enable_raw_mode() {
	struct termios orig_termios;
	tcgetattr(STDIN_FILENO, &orig_termios);

	struct termios raw = orig_termios;
    /* input modes: no break, no CR to NL, no parity check, no strip char,
     * no start/stop output control. */
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    /* output modes - disable post processing */
    raw.c_oflag &= ~(OPOST);
    /* control modes - set 8 bit chars */
    raw.c_cflag |= (CS8);
    /* local modes - choing off, canonical off, no extended functions,
     * no signal chars (^Z,^C) */
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    /* control chars - set return condition: min number of bytes and timer. */
    raw.c_cc[VMIN] = 0; /* Return each byte, or zero for timeout. */
    raw.c_cc[VTIME] = 1; /* 100 ms timeout (unit is tens of second). */

    /* put terminal in raw mode after flushing */
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

	for(int i = 0; i < size; i++) {
		printf("%09x: ", i);
		if (contents[i] < '!' || contents[i] > '~') {
			printf(". = %02x\n", contents[i]);
		} else {
			printf("%c = %02x\n", contents[i], contents[i]);
		}
	}

	free(contents);
	fclose(fp);
}

int open(const char* filename) {
	FILE* fp = NULL;
	fp = fopen(filename, "r");

	if (fp == NULL) {
		if (errno != ENOENT) {
			perror("Opening file");
			exit(1);
		}
		return EINVAL;
	}

	unsigned int c = 0;
	unsigned int offset = 0;
	bool firstline = true;
	while ((c = fgetc(fp)) != EOF) {
		if (offset % 15 == 0) {
			if (firstline) {
				firstline = false;
			} else {
				printf("\n");
			}
			printf("%010x: ", offset);
		}

		printf("%02x ", c);
		offset++;
	}

	fclose(fp);

	return 0;
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



int main(int argc, char* argv[]) {
	if (argc != 2) {
		fprintf(stderr, "expected arg\n");
		exit(2);
	}

	openfile(argv[1]);
//	open(argv[1]);
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
