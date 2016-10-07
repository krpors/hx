CC=gcc
CFLAGS=-std=c99 -Wall -O3

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
	rm -f *.o
