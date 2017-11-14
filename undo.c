/*
 * This file is part of hx - a hex editor for the terminal.
 *
 * Copyright (c) 2016 Kevin Pors. See LICENSE for details.
 */

#include "undo.h"

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <assert.h>

static const char* action_names[] = {
	"delete",
	"insert",
	"replace",
	"append"
};

const char* action_type_name(enum action_type type) {
	return action_names[type];
}


struct action_list* action_list_init() {
	struct action_list* list = malloc(sizeof(struct action_list));
	list->head = NULL;
	list->tail = NULL;
	list->curr = NULL;
	return list;
}

void action_list_add(struct action_list* list, enum action_type type, int offset, unsigned char c) {
	assert(list != NULL);

	struct action* action = malloc(sizeof(struct action));
	action->prev = NULL;
	action->next = NULL;
	action->act = type;
	action->offset = offset;
	action->c = c;

	if (list->head == NULL) {
		// is this the first node added to the list?
		list->head = action;
		list->tail = action;
	} else {
		// Point the previous node of the new action to the current tail.
		action->prev = list->tail;
		// Then point the tail of the list to the new action.
		list->tail->next = action;
		// Then
		list->tail = action;
	}

	list->curr = action;
}

void action_list_delete(struct action_list* list, struct action* action) {
	assert(list != NULL);
	assert(action != NULL);

	// Suppose this is the list, and [c] must be deleted
	//
	//   [a] -> [b] -> [c] -> [d] -> END
	//
	// The result will become:
	//
	//   [a] -> [b] -> END
	//
	// The tail will be the previous of 'c', and the next
	// value of 'b' will point to nothing.

	// Check if the 'action' is the first element.
	if (action->prev != NULL) {
		list->tail = action->prev;
		list->tail->next = NULL;
	}

	bool remove = (action == list->head);
	bool curr_removed = false;

	// temp node
	struct action* node = action;
	while (node != NULL) {
		struct action* temp = node;
		if (list->curr == temp) curr_removed = true;
		node = temp->next;
		free(temp);
		temp = NULL;
	}

	if (remove) {
		list->head = NULL;
		list->tail = NULL;
	}

	// If curr was removed it was after tail - so move it to the latest.
	if (curr_removed) list->curr = list->tail;
}

void action_list_free(struct action_list* list) {
	assert(list != NULL);

	struct action* node = list->head;
	while (node != NULL) {
		struct action* temp = node;
		node = node->next;
		free(temp);
	}
	// after removing all linked nodes from the head,
	// dont forget to remove the head as well:
	free(node);
	free(list);
}


void action_list_print(struct action_list* list) {
	struct action* node = list->head;
	if (node == NULL) {
		fprintf(stderr, "Nothing to delete, head is null\n");
		return;
	}
	while (node != NULL) {
		fprintf(stderr, "(%d, %s, %02x) -> ", node->offset, action_names[node->act], node->c);
		node = node->next;
		if (node == NULL) {
			fprintf(stderr, "END\n");
		}
	}
}

unsigned int action_list_size(struct action_list* list) {
	unsigned int size = 0;
	struct action* node = list->head;
	for (node = list->head; node != NULL; node = node->next) {
		size++;
	}
	return size;
}

void action_list_move(struct action_list* list, int direction) {
	(void)list;
	(void)direction;
}

