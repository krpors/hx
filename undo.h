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
 * action done, and the head the first. By undoing, the tail is
 * currently removed. Needs further work.
 */

// Type of undo actions.
enum action_type {
	ACTION_DELETE,  // character deleted
	ACTION_INSERT,  // character inserted
	ACTION_REPLACE, // character replaced
	ACTION_APPEND   // character appended
};

/*
 * This struct contains the data about an action, as well as the pointers
 * to the previous action (or NULL if this is the first), the next action
 * (or NULL if this is the first or last), the type of action, the offset
 * where the action was done, and the character which was deleted, inserted
 * or replaced.
 */
struct action {
	struct action* prev; // previous action or NULL if first.
	struct action* next; // next action or NULL if first, or last.

	enum action_type act; // the type of action.
	int offset;           // the offset where something was changed.
	unsigned char c;      // the character inserted, deleted, etc.
};


/*
 * Simply returns the name of the action corresponding to the given enum type.
 * This is defined as a function to prevent declaring `action_names' as extern
 * in other source files.
 */
const char* action_type_name(enum action_type type);

/*
 * The action_list contains head and tail information. Functions are defined
 * to operate on this struct to add or delete `action's.
 */
struct action_list {
	struct action* head; // head/start of the list
	struct action* tail; // tail/end of the list.
};

/*
 * Creates an action_list on the heap, initializes the head and tail to NULL
 * and returns it.
 */
struct action_list* action_list_init();

/*
 * Adds an `action' to the tail of the list by allocating it on the heap, and
 * then setting the pointers correctly.
 */
void action_list_add(struct action_list* list, enum action_type type, int offset, unsigned char c);

/*
 * Deletes an `action' from the list. All trailing actions are freed as well.
 * So if the complete list needs to be freed, one can call this function with
 * the `head' of the action_list.
 */
void action_list_delete(struct action_list* list, struct action* action);

/*
 * Frees the action_list by first freeing the `head' by calling
 * action_list_delete, then freeing the list itself.
 */
void action_list_free(struct action_list* list);

/*
 * For debugging purposes, prints the contents to the list to stderr.
 */
void action_list_print(struct action_list* list);

/*
 * Gets the size of the list.
 */
unsigned int action_list_size(struct action_list* list);

#endif // HX_UNDO_H
