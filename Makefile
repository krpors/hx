# Read the version of the current commit.
hx_git_hash := $(shell git rev-parse --verify HEAD --short=12)

# __BSD_VISIBLE for SIGWINCH on FreeBSD.
CFLAGS=-std=c99 -Wall -Wextra -Wpedantic -O3 -ggdb -DNDEBUG -D__BSD_VISIBLE -DHX_GIT_HASH=\"$(hx_git_hash)\"

objects=main.o editor.o charbuf.o util.o undo.o

# Make use of implicit rules to build the hx binary.
hx: $(objects)
	$(CC) -o $@ $(CFLAGS) $(objects)

main.o: charbuf.o util.o undo.o editor.o
editor.o: editor.h charbuf.o util.o undo.o
charbuf.o: charbuf.h
util.o: util.h
undo.o: undo.h

hx.1.gz: hx.1
	gzip -k hx.1

.PHONY: all
all: hx hx.1.gz

.PHONY: install
install: all
	install -s ./hx /usr/bin
	install ./hx.1.gz /usr/share/man/man1

.PHONY: clean
clean:
	rm -f *.o hx.1.gz hx
