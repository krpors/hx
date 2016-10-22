# hx
Attempt to create a hex editor using plain C + POSIX libs. The project's code
is somewhat influenced by the [kilo project](https://github.com/antirez/kilo)
project.

# Compiling and running
The project does not have a dependency on libraries, not even curses. Like the
kilo editor, it makes use of ANSI escape sequences. The source can be compiled
by simply running `make`.

Running `hx`:

    ./hx filename

Keys which can be used:

    CTRL+Q: Quit
	CTRL+S: Save (in place)
	hjkl  : Vim like movement
	w     : Skip one group of bytes to the right
	b     : Skip one group of bytes to the left
	gg    : Move to start of file
	G     : Move to end of file
	x     : Delete byte at cursor position
	r     : Replace mode. Type two valid hex chars to insert that byte
	        at the current offset
	i     : Insert mode (not functional yet)
	]     : Increment byte at cursor position with 1
	[     : Decrement byte at cursor position with 1
	ESC   : Normal mode
	End   : Move cursor to end of the offset line
	Home  : Move cursor to the beginning of the offset line

Subject to change however.

# Implementation details
The program uses raw ANSI escape sequences for manipulating colors, cursor
positions and whatnot. The program first initializes the terminal in
so-called mode (see `man termios`). Then keypresses are read, processed
and then one function renders the contents, cursor and stuff.

Not everything is final and some refactoring is needed. Valgrind doesn't
report any leaks so that looks OK :)

# Wishlist
Some extras I still like to see:

1. Searching a string or byte sequence.
1. Going to offset of choice.
