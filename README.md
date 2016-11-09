# hx
Attempt to create a hex editor using plain C + POSIX libs. The project's code
is somewhat influenced by the [kilo project](https://github.com/antirez/kilo).

# Compiling and running
The project does not have a dependency on libraries, not even curses. Like the
kilo editor, it makes use of ANSI escape sequences. The source can be compiled
by simply running `make`.

Running `hx`:

    ./hx filename

Keys which can be used:

    CTRL+Q  : Quit immediately without saving.
	CTRL+S  : Save (in place).
	hjkl    : Vim like cursor movement.
	Arrows  : Also moves the cursor around.
	w       : Skip one group of bytes to the right.
	b       : Skip one group of bytes to the left.
	gg      : Move to start of file.
	G       : Move to end of file.
	x       : Delete byte at cursor position.
	Delete  : Delete byte at cursor position.
	r       : Replace mode. Type two valid hex chars to insert that byte
	          at the current offset.
	i       : Insert mode (not functional yet).
	]       : Increment byte at cursor position with 1.
	[       : Decrement byte at cursor position with 1.
	ESC     : Normal mode.
	End     : Move cursor to end of the offset line.
	Home    : Move cursor to the beginning of the offset line.
	:       : Enable command mode.

Subject to change however.

# Command mode

Being in normal mode (`ESC`) then hitting the colon key `:`, you can enter command
mode where manual commands can be typed. The following commands are recognized currently:

* `:123`      : go to offset 123 (base 10)
* `:0x7a69`   : go to offset 0x7a69 (base 16), 31337 in base 10.

Input is very basic. Cursor movement is not available. Only backspace/enter.

# Implementation details
The program uses raw ANSI escape sequences for manipulating colors, cursor
positions and whatnot. The program first initializes the terminal in
so-called mode (see `man termios`). Then keypresses are read, processed
and then one function renders the contents, cursor and stuff.

Not everything is final and some refactoring is needed. Valgrind doesn't
report any leaks so that looks OK :)

# Wishlist and TODOs

1. Searching a string or byte sequence.
1. Colorize changes made to a file
1. Undo/redo actions
