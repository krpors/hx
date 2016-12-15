hx_git_hash != git rev-parse --verify HEAD --short=12
hx_version != git describe --tags 2>/dev/null || echo "1.0.0"

CPPFLAGS = -DNDEBUG -DHX_GIT_HASH=\"$(hx_git_hash)\" -DHX_VERSION=\"$(hx_version)\"
CPPFLAGS += -D__BSD_VISIBLE # SIGWINCH on FreeBSD.
CFLAGS = -std=c99 -Wall -Wextra -Wpedantic -O3 -MMD -MP
LDFLAGS = -O3

objects := hx.o editor.o charbuf.o util.o undo.o

PREFIX ?= /usr/local
bindir = /bin
mandir = /man

%.gz: %
	gzip -k $<

all: hx hx.1.gz
hx: $(objects)

debug: all
debug: CFLAGS += -ggdb -Og
debug: LDFLAGS += -ggdb -Og

install: all
	install -Dm755 -s ./hx -t $(DESTDIR)$(PREFIX)$(bindir)
	install -Dm644 ./hx.1.gz -t $(DESTDIR)$(PREFIX)$(mandir)/man1

clean:
	$(RM) $(objects) $(objects:.o=.d) hx.1.gz hx

-include $(objects:.o=.d)

.PHONY: all debug install clean
