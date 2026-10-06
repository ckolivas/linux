// SPDX-License-Identifier: GPL-2.0
/*
  Copyright (C) 2011,2016 Con Kolivas.

  Code based on example originally by William Pugh.

Skip Lists are a probabilistic alternative to balanced trees, as
described in the June 1990 issue of CACM and were invented by
William Pugh in 1987.

A couple of comments about this implementation:
The routine randomLevel has been hard-coded to generate random
levels using p=0.25. It can be easily changed.

The insertion routine has been implemented so as to use the
dirty hack described in the CACM paper: if a random level is
generated that is more than the current maximum level, the
current maximum level plus one is used instead.

Levels start at zero and go up to MaxLevel (which is equal to
MaxNumberOfLevels-1).

The routines defined in this file are:

init: defines slnode

new_skiplist: returns a new, empty list

randomLevel: Returns a random level based on a u64 random seed passed to it.
In MuQSS, the "niffy" time is used for this purpose.

insert(l, node, key): inserts node into l ordered by key. This operation
occurs in O(log n) time.

delnode(slnode, l, node): deletes the node from l. This operation occurs
in O(k) time where k is the number of levels of the node in question
(max 4). The original delete function occurred in O(log n) time and
involved a search.

MuQSS Notes: In this implementation of skiplists, there are bidirectional
next/prev pointers. The node is embedded in task_struct; the task is
recovered with container_of(). The key here is chosen by the scheduler
so as to sort tasks according to the priority list requirements. The
scheduler lookup occurs in O(1) time because it is always the first
item in the level 0 linked list. Since the task struct embeds the node,
it can also remove it much faster than the original implementation with
the aid of prev<->next pointer manipulation and no searching.

*/

#include <linux/compiler.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/skip_list.h>

static struct kmem_cache *skiplist_cache __read_mostly;

#define MaxNumberOfLevels SKIPLIST_MAXLEVEL
#define MaxLevel (MaxNumberOfLevels - 1)

void skiplist_init(skiplist_node *slnode)
{
	int i;

	slnode->key = 0xFFFFFFFFFFFFFFFF;
	for (i = 0; i < MaxNumberOfLevels; i++)
		slnode->next[i] = slnode->prev[i] = slnode;
}

void __init skiplist_cache_init(void)
{
	skiplist_cache = kmem_cache_create("skiplist", sizeof(skiplist),
					   0, SLAB_HWCACHE_ALIGN, NULL);
	BUG_ON(!skiplist_cache);
}

skiplist *new_skiplist(skiplist_node *slnode)
{
	skiplist *l = kmem_cache_zalloc(skiplist_cache, GFP_ATOMIC);

	BUG_ON(!l);
	l->header = slnode;
	l->best_key = ~0ULL;
	return l;
}

void skiplist_free(skiplist *l)
{
	kmem_cache_free(skiplist_cache, l);
}

void free_skiplist(skiplist *l)
{
	skiplist_node *p, *q;

	p = l->header;
	do {
		q = p->next[0];
		p->next[0]->prev[0] = q->prev[0];
		skiplist_node_init(p);
		p = q;
	} while (p != l->header);
	skiplist_free(l);
}

void skiplist_node_init(skiplist_node *node)
{
	memset(node, 0, sizeof(skiplist_node));
}

static inline unsigned int randomLevel(const long unsigned int randseed)
{
	/*
	 * Two bits per extra level (p = 0.25). Search 2*MaxLevel+1 bits
	 * so the result stays in 0..MaxLevel; this matches the old
	 * 8-slot arrays whose random height never exceeded 3.
	 */
	return find_first_bit(&randseed, MaxLevel * 2 + 1) / 2;
}

void skiplist_insert(skiplist *l, skiplist_node *node, keyType key, unsigned int randseed)
{
	skiplist_node *update[MaxNumberOfLevels];
	skiplist_node *p, *q;
	int k = l->level;

	p = l->header;
	do {
		while (q = p->next[k], q->key <= key)
			p = q;
		update[k] = p;
	} while (--k >= 0);

	WRITE_ONCE(l->entries, l->entries + 1);
	k = randomLevel(randseed);
	if (k > l->level) {
		k = ++l->level;
		update[k] = l->header;
	}

	node->key = key;
	do {
		p = update[k];
		node->next[k] = p->next[k];
		p->next[k] = node;
		node->prev[k] = p;
		node->next[k]->prev[k] = node;
	} while (--k >= 0);
}

/*
 * Unlink @node from @l. Returns true if it was linked and has been removed,
 * false if it was not on any list and nothing was changed - the caller's own
 * accounting has to be skipped along with the removal in that case.
 */
bool skiplist_delete(skiplist *l, skiplist_node *node)
{
	int k, m;

	/*
	 * Insert fills levels contiguously from 0 up and removal zeroes the
	 * whole node, so a NULL next[] marks the top of this node and stands
	 * in for the level count the node used to carry.
	 */
	for (k = 0; k < MaxNumberOfLevels && node->next[k]; k++) {
		node->prev[k]->next[k] = node->next[k];
		node->next[k]->prev[k] = node->prev[k];
	}
	m = k - 1;
	/*
	 * A node that was never queued has no levels linked at all. Leave
	 * before decrementing entries, which would otherwise go negative and
	 * make the emptiness tests in earliest_deadline_task() pass on an
	 * empty list, handing the caller the header as if it were a task.
	 */
	if (WARN_ON_ONCE(m < 0))
		return false;
	skiplist_node_init(node);
	if (m == l->level) {
		while (l->header->next[m] == l->header && l->header->prev[m] == l->header && m > 0)
			m--;
		l->level = m;
	}
	WRITE_ONCE(l->entries, l->entries - 1);

	return true;
}
