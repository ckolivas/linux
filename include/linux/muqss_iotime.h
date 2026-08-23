/* SPDX-License-Identifier: GPL-2.0 */
/*
 * I/O aware scheduling support for MuQSS.
 *
 * Attributes block device time back to the task that caused the I/O, so the
 * CPU scheduler can see what a task costs the storage device rather than only
 * what it costs the CPU.
 *
 * The attributed time is charged against the task's virtual deadline one for
 * one, a nanosecond of deadline for each nanosecond of device time, so that
 * I/O and CPU cost a task the same.
 *
 * The same attribution carries CPU time as well as device time. A kworker
 * running a work item declares itself to be acting for whoever queued that
 * work, so the time it spends is charged back to them rather than lost to a
 * kernel thread. That half is charged one for one: a nanosecond of CPU time
 * costs the same wherever the kernel chose to spend it, and work a task does
 * in its own context already costs it exactly that.
 *
 * See MuQSS-iotime-design.md.
 */
#ifndef _LINUX_MUQSS_IOTIME_H
#define _LINUX_MUQSS_IOTIME_H

struct bio;
struct folio;
struct request;
struct gendisk;
struct task_struct;
struct work_struct;

#ifdef CONFIG_MUQSS_IOTIME

/*
 * A window during which current is running on behalf of another task, opened
 * by a kworker around one work item. Lives on the stack of the thread doing
 * the work; see muqss_kerntime_begin().
 */
struct muqss_kern_window {
	struct task_struct *owner;	/* who to charge, NULL for nobody */
	struct task_struct *prev;	/* override to restore on close */
	u64 start;			/* current's runtime when opened */
};

/*
 * Tag @bio with the task that caused it. Called from the block layer submit
 * path, where current is the originating task for reads, readahead and
 * synchronous writes alike. Takes a reference that is dropped by
 * muqss_iotime_put_owner().
 */
void muqss_iotime_set_owner(struct bio *bio);

/*
 * Drop the owner reference taken above. Idempotent, so it can be called from
 * every site that releases bio state without tracking which ran first.
 */
void muqss_iotime_put_owner(struct bio *bio);

/*
 * Propagate ownership from @bio_src to @bio. Stacked drivers (md, dm, loop)
 * resubmit clones from their own threads, where current is a kernel thread
 * rather than the originating task, so without this attribution is lost on
 * every RAID and LUKS setup.
 */
void muqss_iotime_clone_owner(struct bio *bio, struct bio *bio_src);

/* Attach the accounting policy to a newly registered queue. */
void muqss_iotime_enable(struct gendisk *disk);

/* Zero a new task's counters so nothing is inherited across fork. */
void muqss_iotime_task_init(struct task_struct *p);

/*
 * Owner table, used to attribute writeback. A dirtying task is given a slot,
 * whose index is small enough to store in spare page->flags bits; writeback
 * issued later by a flusher thread reads the index back off the folio and
 * resolves it to the task that dirtied it.
 */

/* Slot for current, allocating one on first use. 0 if none is available. */
unsigned int muqss_iotime_owner_slot(void);

/* Resolve a slot to its task, taking a reference. NULL if the slot is stale. */
struct task_struct *muqss_iotime_owner_task(unsigned int slot);

/* Give up a task's slot. Called from exit. */
void muqss_iotime_release_slot(struct task_struct *p);

/* Record current as the dirtier of @folio, if it can be attributed. */
void muqss_iotime_dirty_folio(struct folio *folio);

/*
 * Declare that current is about to do I/O on behalf of @owner, and undo it.
 * Used by io_uring's io-wq workers, which issue requests that another task
 * queued. Returns the previous value, to be handed back to _proxy_end().
 *
 * The caller must keep a reference to @owner across the window.
 */
struct task_struct *muqss_iotime_proxy_begin(struct task_struct *owner);
void muqss_iotime_proxy_end(struct task_struct *prev);

/*
 * Two requests are being merged into one. Disowns @rq if @next belongs to a
 * different task, so the survivor is not charged for both.
 */
void muqss_iotime_merge_requests(struct request *rq, struct request *next);

/*
 * Record who queued @work, so the kworker that eventually runs it can be
 * charged to them. Called from the queueing task's own context, which rules
 * out the deferred re-entries into __queue_work() made by the delayed work
 * timer and the rcu_work callback: those run in interrupt and softirq
 * context, where current is whoever was unlucky enough to be interrupted.
 */
void muqss_work_set_owner(struct work_struct *work);

/*
 * Open and close a window in which current is running work on behalf of the
 * task in @slot. The CPU time current consumes across the window is charged
 * to that task, and any I/O the work issues is attributed to it too.
 *
 * Cheap when there is nobody to charge, which is the common case: an unowned
 * slot costs one comparison and reads no clocks.
 */
void muqss_kerntime_begin(struct muqss_kern_window *w, unsigned int slot);
void muqss_kerntime_end(struct muqss_kern_window *w);

/*
 * The CPU time current has consumed so far, including the part not yet banked
 * into ->sched_time. Implemented by the scheduler; see MuQSS.c.
 */
u64 muqss_task_runtime_live(void);

#else  /* !CONFIG_MUQSS_IOTIME */

struct muqss_kern_window { };

static inline void muqss_iotime_set_owner(struct bio *bio) { }
static inline void muqss_iotime_put_owner(struct bio *bio) { }
static inline void muqss_iotime_clone_owner(struct bio *bio,
					    struct bio *bio_src) { }
static inline void muqss_iotime_enable(struct gendisk *disk) { }
static inline void muqss_iotime_task_init(struct task_struct *p) { }
static inline unsigned int muqss_iotime_owner_slot(void) { return 0; }
static inline struct task_struct *muqss_iotime_owner_task(unsigned int slot)
{
	return NULL;
}
static inline void muqss_iotime_release_slot(struct task_struct *p) { }
static inline void muqss_iotime_dirty_folio(struct folio *folio) { }
static inline struct task_struct *
muqss_iotime_proxy_begin(struct task_struct *owner)
{
	return NULL;
}
static inline void muqss_iotime_proxy_end(struct task_struct *prev) { }
static inline void muqss_iotime_merge_requests(struct request *rq,
					       struct request *next) { }
static inline void muqss_work_set_owner(struct work_struct *work) { }
static inline void muqss_kerntime_begin(struct muqss_kern_window *w,
					unsigned int slot) { }
static inline void muqss_kerntime_end(struct muqss_kern_window *w) { }

#endif /* CONFIG_MUQSS_IOTIME */

#endif /* _LINUX_MUQSS_IOTIME_H */
