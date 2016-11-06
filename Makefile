# Read the version of the current commit.
hx_git_hash := $(shell git rev-parse --verify HEAD --short=12)

CC=gcc
CFLAGS=-std=c99 -Wall -O3 -ggdb -DNDEBUG -DHX_GIT_HASH=\"$(hx_git_hash)\"

objects=main.o charbuf.o

# Make use of implicit rules to build the hx binary.
hx: $(objects)
	$(CC) -o $@ $(CFLAGS) $(objects)

main.o: charbuf.o
charbuf.o: charbuf.h

hx.1.gz: hx.1
	gzip -k hx.1

.PHONY: all
all: hx hx.1.gz

.PHONY: install
install: all
	@[ `id -u` = 0 ] || { echo "Root required to install."; exit 1; }
	install -s ./hx /usr/bin
	install ./hx.1.gz /usr/share/man/man1

.PHONY: uninstall
uninstall:
	@[ `id -u` = 0 ] || { echo "Root required to uninstall."; exit 1; }
	rm /usr/bin/hx
	rm /usr/share/man/man1/hx.1.gz

.PHONY: clean
clean:
	rm -f *.o hx.1.gz hx
