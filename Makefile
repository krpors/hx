# Read the version of the current commit.
hx_git_hash := $(shell git rev-parse --verify HEAD --short=12)

# __BSD_VISIBLE for SIGWINCH on FreeBSD.
CFLAGS=-std=c99 -Wall -Wextra -Wpedantic -O3 -ggdb -DNDEBUG -D__BSD_VISIBLE -MMD -MP \
       -DHX_GIT_HASH=\"$(hx_git_hash)\"
LDFLAGS = -O3 -ggdb

objects=hx.o editor.o charbuf.o util.o undo.o

.PHONY: all
all: hx hx.1.gz

# Make use of implicit rules to build the hx binary.
hx: $(objects)

%.gz: %
	gzip -k $<

.PHONY: install
install: all
	install -s ./hx /usr/bin
	install ./hx.1.gz /usr/share/man/man1

.PHONY: clean
clean:
	$(RM) *.o *.d hx.1.gz hx

-include $(objects:.o=.d)
