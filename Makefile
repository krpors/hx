# Read the version of the current commit.
HX_GIT_HASH := $(shell git rev-parse --verify HEAD --short=12)

CC=gcc
CFLAGS=-std=c99 -Wall -O3 -ggdb -DNDEBUG -DHX_GIT_HASH=\"$(HX_GIT_HASH)\"

DEPS=
OBJECTS=main.o

# $@ = compilation left side of the :
# $^ = compilation right side of the :
# $< = first item in dependency list

%.o: %.c $(DEPS)
	$(CC) $(CFLAGS) -c $< -o $@

hx: $(OBJECTS)
	$(CC) $(CFLAGS) -o $@ $^

.PHONY: clean
clean:
	rm -f *.o hx
