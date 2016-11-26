/*
 * This file is part of hx - a hex editor for the terminal.
 *
 * Copyright (c) 2016 Kevin Pors. See LICENSE for details.
 */

#ifndef HX_UNDO_H
#define HX_UNDO_H

/*
 * Contains definitions and functions to allow undo/redo actions.
 * It's basically a double-linked list, where the tail is the last
 * action done, and the head the first. By undoing, the pointer is
 * 'decremented', and by redoing, the pointer is 'incremented'.
 *
 * Once an action is done again, all following redo-actions will
 * be discarded.
 */

enum action_type {
	ACTION_NONE,
	ACTION_DELETE,
	ACTION_INSERT,
	ACTION_REPLACE,
	ACTION_APPEND
};

struct action {
	struct action* prev;
	struct action* next;

	enum action_type act;

	int offset; // the offset where something was changed.
	unsigned char c; // the character inserted, deleted, etc.
};

struct action_list {
	struct action* head;
	struct action* tail;
};

struct action_list* action_list_init();
void action_list_add(struct action_list* list, enum action_type type, int offset, unsigned char c);
void action_list_free(struct action_list* list);
void action_list_print(struct action_list* list);


#endif // HX_UNDO_H
