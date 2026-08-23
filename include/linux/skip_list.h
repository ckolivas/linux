/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SKIP_LISTS_H
#define _LINUX_SKIP_LISTS_H

/*
 * Four levels is enough: randomLevel() returns 0..3, and insert only
 * grows the list one level at a time, so next/prev[4..] were unused.
 */
#define SKIPLIST_MAXLEVEL 4

typedef u64 keyType;

typedef struct nodeStructure skiplist_node;

struct nodeStructure {
	keyType key;
	skiplist_node *next[SKIPLIST_MAXLEVEL];
	skiplist_node *prev[SKIPLIST_MAXLEVEL];
};

typedef struct listStructure {
	int entries;
	int level;	/* Maximum level of the list
			(1 more than the number of levels in the list) */
	u64 best_key;	/* Earliest queued key; lockless hint for EDT */
	skiplist_node *header; /* pointer to header */
} skiplist;

void skiplist_init(skiplist_node *slnode);
void skiplist_cache_init(void);
skiplist *new_skiplist(skiplist_node *slnode);
void skiplist_free(skiplist *l);
void free_skiplist(skiplist *l);
void skiplist_node_init(skiplist_node *node);
void skiplist_insert(skiplist *l, skiplist_node *node, keyType key, unsigned int randseed);
bool skiplist_delete(skiplist *l, skiplist_node *node);

static inline bool skiplist_node_empty(skiplist_node *node) {
	return (!node->next[0]);
}
#endif /* _LINUX_SKIP_LISTS_H */
