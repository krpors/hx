#include "charbuf.h"

#include <assert.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>


/**
 * Create a charbuf on the heap and return it.
 */
struct charbuf* charbuf_create() {
	struct charbuf* b = malloc(sizeof(struct charbuf));
	if (b) {
		b->contents = NULL;
		b->len = 0;
		return b;
	} else {
		perror("Unable to allocate size for struct charbuf");
		exit(1);
	}
}

/**
 * Deletes the charbuf's contents, and the charbuf itself.
 */
void charbuf_free(struct charbuf* buf) {
	free(buf->contents);
	free(buf);
}

/**
 * Appends `what' to the charbuf, writing at most `len' bytes. Note that
 * if we use snprintf() to format a particular string, we have to subtract
 * 1 from the `len', to discard the null terminator character.
 */
void charbuf_append(struct charbuf* buf, const char* what, size_t len) {
	assert(what != NULL);

	// reallocate the contents with more memory, to hold 'what'.
	char* new = realloc(buf->contents, buf->len + len);
	if (new == NULL) {
		perror("Unable to realloc charbuf");
		exit(1);
	}

	// copy 'what' to the target memory
	memcpy(new + buf->len, what, len);
	buf->contents = new;
	buf->len += len;
}

/**
 * Draws (writes) the charbuf to the screen.
 */
void charbuf_draw(struct charbuf* buf) {
	if (write(STDOUT_FILENO, buf->contents, buf->len) == -1) {
		perror("Can't write charbuf");
		exit(1);
	}
}

