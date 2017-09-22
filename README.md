# hx

A hex editor using plain C + POSIX libs. The project's code
is somewhat influenced by the [kilo project](https://github.com/antirez/kilo).

For an idea how it looks:

![hx](http://i.imgur.com/5XPbMGW.png)

# Compiling and running

The project does not have a dependency on libraries, not even curses. Like the
kilo editor, it makes use of ANSI escape sequences. The source can be compiled
by simply running `make`, or `make all` to gzip the manpage as well. To install,
simply run `make install` (as root). The files are currently installed under
`/usr/local/bin/` (binary) and `/usr/local/man/man1` (man page). Not really sure yet
if this is portable across distributions, though.

Running `hx`:

	hx filename       # open a file
	hx -h             # for help
	hx -v             # version information
	hx -o 32 filename # open file with 32 octets per line
	hx -g 8 filename  # open file, set octet grouping to 8

Keys which can be used:

	CTRL+Q  : Quit immediately without saving.
	CTRL+S  : Save (in place).
	hjkl    : Vim like cursor movement.
	Arrows  : Also moves the cursor around.
	w       : Skip one group of bytes to the right.
	b       : Skip one group of bytes to the left.
	gg      : Move to start of file.
	G       : Move to end of file.
	x / DEL : Delete byte at cursor position.
	/       : Start search input.
	n       : Search for next occurrence.
	N       : Search for previous occurrence.
	u       : Undo the last action.

	a       : Append mode. Appends a byte after the current cursor position.
	A       : Append mode. Appends the literal typed keys (except ESC).
	i       : Insert mode. Inserts a byte at the current cursor position.
	I       : Insert mode. Inserts the literal typed keys (except ESC).
	r       : Replace mode. Replaces the byte at the current cursor position.
	:       : Command mode. Commands can be typed and executed (see below).
	ESC     : Return to normal mode.

	]       : Increment byte at cursor position with 1.
	[       : Decrement byte at cursor position with 1.

	End     : Move cursor to end of the offset line.
	Home    : Move cursor to the beginning of the offset line.


# Command mode

Being in normal mode (`ESC`) then hitting the colon key `:`, you can enter command
mode where manual commands can be typed. The following commands are recognized currently:

* `:123`      : go to offset 123 (base 10)
* `:0x7a69`   : go to offset 0x7a69 (base 16), 31337 in base 10.
* `:w`        : writes the file.
* `:q`        : quits (will warn if the buffer is dirty).
* `:q!`       : quits promptly without warning.
* `set o=16`  : sets the amount of octets per line.
* `set g=8`   : sets grouping of bytes.

Input is very basic in command mode. Cursor movement is not available (yet?).

# Implementation details

The program uses raw ANSI escape sequences for manipulating colors, cursor
positions and whatnot. The program first initializes the terminal in
so-called raw mode (see `man termios`). Then keypresses are read, processed
and then one function renders the contents, cursor and stuff.

In any case, the source code is pretty heavily commented I guess. For more
details, Use The Source Luke.

# Wishlist and TODOs

1. Perhaps a configuration file to control the colors or some default settings.
1. Searching a byte sequence (not by ASCII) would be handy.
1. Undo works, but redo is not yet implemented.
