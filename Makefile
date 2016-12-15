# Read the version of the current commit.
hx_git_hash := $(shell git rev-parse --verify HEAD --short=12)
hx_version := $(shell git describe --tags 2>/dev/null || echo "1.0.0")

# __BSD_VISIBLE for SIGWINCH on FreeBSD.
CPPFLAGS = -DNDEBUG -D__BSD_VISIBLE \
       -DHX_GIT_HASH=\"$(hx_git_hash)\" -DHX_VERSION=\"$(hx_version)\"
CFLAGS=-std=c99 -Wall -Wextra -Wpedantic -O3 -MMD -MP
LDFLAGS = -O3

objects=hx.o editor.o charbuf.o util.o undo.o

PREFIX ?= /usr/local
bindir = /bin
mandir = /man

.PHONY: all
all: hx hx.1.gz

# Make use of implicit rules to build the hx binary.
hx: $(objects)

.PHONY: debug
debug: all
debug: CFLAGS += -ggdb -Og
debug: LDFLAGS += -ggdb -Og

%.gz: %
	gzip -k $<

.PHONY: install
install: all
	install -Dm755 -s ./hx -t $(DESTDIR)$(PREFIX)$(bindir)
	install -Dm644 ./hx.1.gz -t $(DESTDIR)$(PREFIX)$(mandir)/man1

.PHONY: clean
clean:
	$(RM) *.o *.d hx.1.gz hx

-include $(objects:.o=.d)
