/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_WORKQUEUE_TYPES_H
#define _LINUX_WORKQUEUE_TYPES_H

#include <linux/atomic.h>
#include <linux/lockdep_types.h>
#include <linux/timer_types.h>
#include <linux/types.h>

struct workqueue_struct;

struct work_struct;
typedef void (*work_func_t)(struct work_struct *work);
void delayed_work_timer_fn(struct timer_list *t);

struct work_struct {
	atomic_long_t data;
	struct list_head entry;
	work_func_t func;
#ifdef CONFIG_MUQSS_IOTIME
	/*
	 * Index in the I/O owner table of the task that queued this work, so
	 * the CPU time the kworker spends running it can be charged back to
	 * whoever asked for it. 0 means nobody to charge, which is the case
	 * for the great deal of work queued by kernel threads on their own
	 * account.
	 *
	 * A slot index rather than a task pointer deliberately: work items
	 * are cancelled and disabled through half a dozen paths that clear
	 * the pending bit without ever running the work, and none of them
	 * would drop a reference taken here. A stale slot simply resolves to
	 * NULL and the work is charged to nobody.
	 */
	unsigned int muqss_owner_slot;
#endif
#ifdef CONFIG_LOCKDEP
	struct lockdep_map lockdep_map;
#endif
};

#ifdef CONFIG_MUQSS_IOTIME
static inline unsigned int muqss_work_owner(struct work_struct *work)
{
	return work->muqss_owner_slot;
}

/*
 * Every queueing path stamps the owner, so this only matters for a work item
 * initialised on memory that was never zeroed and then reaching execution by
 * some path that does not. It is here so that such a path, if one exists,
 * charges nobody rather than charging whichever unlucky task holds the slot
 * the garbage happens to name.
 */
static inline void muqss_work_init_owner(struct work_struct *work)
{
	work->muqss_owner_slot = 0;
}
#else
static inline unsigned int muqss_work_owner(struct work_struct *work)
{
	return 0;
}

static inline void muqss_work_init_owner(struct work_struct *work) { }
#endif

#endif /* _LINUX_WORKQUEUE_TYPES_H */
