/*
 * This file is part of hx - a hex editor for the terminal.
 *
 * Copyright (c) 2016 Kevin Pors. See LICENSE for details.
 */

// Define _POSIX_SOURCE to enable sigaction(). See `man 2 sigaction'
#define _POSIX_SOURCE

#include "editor.h"
#include "util.h"

// C99 includes
#include <stdio.h>
#include <string.h>

// POSIX and Linux cruft
#include <getopt.h>
#include <signal.h>

// hx defines. Not declared as const because we may want to adjust
// this by using a tool or whatever.
#ifndef HX_GIT_HASH
#define HX_GIT_HASH "unknown"
#endif

#ifndef HX_VERSION
#define HX_VERSION "1.0.0"
#endif

// Comp unit wide editor config.
static struct editor* g_ec;

/**
 * Exits the editor, frees some stuff and resets the terminal setting.
 */
static void editor_exit() {
	editor_free(g_ec);
	clear_screen();
	disable_raw_mode();
}

/**
 * Prints help to the stderr when invoked with -h or with unknown arguments.
 * Explanation can be given for some extra information.
 */
static void print_help(const char* explanation) {
	fprintf(stderr,
	"%s"\
	"usage: hx [-hv] [-o octets_per_line] [-g grouping_bytes] filename\n"\
	"\n"
	"Command options:\n"
	"    -h     Print this cruft and exits\n"
	"    -v     Version information\n"
	"    -o     Amount of octets per line\n"
	"    -g     Grouping of bytes in one line\n"
	"\n"
	"Currently, both these values are advised to be a multiple of 2\n"
	"to prevent garbled display :)\n"
	"\n"
	"Report bugs to <krpors at gmail.com> or see <http://github.com/krpors/hx>\n"
	, explanation);
}

/**
 * Prints some version information back to the stdout.
 */
static void print_version() {
	printf("hx version %s (git: %s)\n", HX_VERSION, HX_GIT_HASH);
}

#if 0
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
#endif


/**
 * Handles the SIGWINCH signal upon terminal resizing.
 */
static void handle_term_resize(int sig) {
	clear_screen();
	get_window_size(&(g_ec->screen_rows), &(g_ec->screen_cols));
	editor_refresh_screen(g_ec);
}


int main(int argc, char* argv[]) {
	char* file = NULL;
	int octets_per_line = 16;
	int grouping = 4;

	int ch = 0;
	while ((ch = getopt(argc, argv, "vhg:o:")) != -1) {
		switch (ch) {
		case 'v':
			print_version();
			return 0;
		case 'h':
			print_help("");
			exit(0);
			break;
		case 'g':
			// parse grouping
			grouping = str2int(optarg, 2, 16, 4);
			break;
		case 'o':
			// parse octets per line
			octets_per_line = str2int(optarg, 16, 64, 16);
			break;
		default:
			print_help("");
			exit(1);
			break;
		}
	}


	// After all options are parsed, we expect a filename to open.
	if (optind >= argc) {
		print_help("error: expected filename\n");
		exit(1);
	}

	file = argv[optind];

	// Signal handler to react on screen resizing.
	struct sigaction act;
	memset(&act, 0, sizeof(struct sigaction));
	act.sa_handler = handle_term_resize;
	sigaction(SIGWINCH, &act, NULL);

	// Editor configuration passed around.
	g_ec = editor_init();
	g_ec->octets_per_line = octets_per_line;
	g_ec->grouping = grouping;

	editor_openfile(g_ec, file);

	enable_raw_mode();
	atexit(editor_exit);
	clear_screen();

	while (true) {
		editor_refresh_screen(g_ec);
		editor_process_keypress(g_ec);
		//debug_keypress();
	}

	editor_free(g_ec);
	return EXIT_SUCCESS;
}
