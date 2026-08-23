// SPDX-License-Identifier: GPL-2.0
/*
 * I/O aware scheduling support for MuQSS.
 *
 * The CPU scheduler can see how much CPU a task uses but nothing of what it
 * costs the storage device. This attaches an rq_qos policy that attributes
 * block device time back to the task that caused the I/O, so that cost is
 * visible to the scheduler.
 *
 * Two different quantities are recorded and deliberately kept apart:
 *
 * Latency is what a waiting task experiences, measured per bio from
 * bio->issue_time_ns, which is stamped in blk_mq_submit_bio() before merging
 * and request allocation. It therefore includes plug, merge and queue wait as
 * well as service.
 *
 * Occupancy is what the task cost everybody else, measured per request from
 * rq->io_start_time_ns at dispatch to rq->muqss_done_ns at completion. This is
 * the quantity the scheduler charges for.
 *
 * Attribution has two halves. Reads and synchronous writes are submitted by
 * the originating task, so the owner is simply current. Writeback is not: the
 * dirtier is recorded on the folio when it is dirtied and recovered when the
 * flusher thread submits the I/O. See the owner table below.
 *
 * The owner table earns its keep a second time over. A kworker running a work
 * item declares itself to be acting for whoever queued that work, which both
 * attributes the I/O the work item issues and lets the CPU time it costs be
 * charged back as kern_debt_ns. See muqss_kerntime_begin().
 *
 * The occupancy is charged one for one against the deadline, with nothing to
 * tune it by: the accounting below and the scheduling it feeds are one
 * feature, and only CONFIG_MUQSS_IOTIME=n removes either. See
 * consume_iotime_penalty() in kernel/sched/MuQSS.c and
 * MuQSS-iotime-design.md.
 */
#include <linux/kernel.h>
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/workqueue_types.h>
#include <linux/muqss_iotime.h>

#include "blk.h"
#include "blk-mq.h"
#include "blk-mq-debugfs.h"
#include "blk-rq-qos.h"
#include "blk-stat.h"

struct blk_iotime {
	struct rq_qos		rqos;

	/* Unattributed device busy time, see comment above. */
	atomic64_t		occupancy_ns;
	atomic64_t		requests;

	/*
	 * Bios that completed with no owner recorded. Counted so the size of
	 * the attribution gap stays visible rather than silently skewing the
	 * per task numbers.
	 */
	atomic64_t		unattributed;

	/* Requests disowned because a merge brought in another task's bio. */
	atomic64_t		mixed;

	/*
	 * Requests that reached completion with no captured timestamp, and so
	 * could not be measured at all. See muqss_iotime_done().
	 */
	atomic64_t		unstamped;

	/* Bios that completed with an owner but no submit timestamp. */
	atomic64_t		nostamp_bio;
};

static inline struct blk_iotime *BLK_IOTIME(struct rq_qos *rqos)
{
	return container_of(rqos, struct blk_iotime, rqos);
}

/*
 * Owner table.
 *
 * Reads are attributable at submit time because the submitting task is the
 * originating one. Writeback is not: the folio is dirtied by one task and
 * written out much later by a flusher thread. The dirtier is therefore
 * recorded on the folio itself, in spare page->flags bits, as an index into
 * this table.
 *
 * Only MUQSS_IOWNER_WIDTH bits are available, so the table is small and the
 * index carries no generation. Slot 0 is reserved to mean "no owner". A slot
 * is held from the task's first dirty until it exits, and the table holds a
 * reference for that whole period, so a resolved pointer is always a live
 * task_struct.
 *
 * Slots are released on exit and never stolen from a live task, so a resolved
 * index is never charged to the wrong task. The cost of that choice is that
 * beyond IOWNER_SLOTS concurrent dirtiers, further tasks get no slot and their
 * writeback goes unattributed; iowner_exhausted counts it.
 *
 * Writeback still in flight for a task that has exited resolves to nothing,
 * which is correct: there is no longer anybody to charge.
 */
#define IOWNER_SLOTS		(1U << MUQSS_IOWNER_SHIFT)

static struct task_struct *iowner_table[IOWNER_SLOTS];
static DEFINE_SPINLOCK(iowner_lock);

static atomic64_t iowner_exhausted;

static atomic64_t iowner_proxied;

/* Work items charged back to a task, and the CPU time they cost. */
static atomic64_t kernwork_charged;
static atomic64_t kernwork_ns;

/*
 * Slot for @tsk, allocating one on first use.
 *
 * @tsk is not necessarily current: an io-wq worker allocates on behalf of the
 * task that queued the request it is running, so that pages it dirties are
 * tagged with the real owner. The caller holds a reference to @tsk.
 */
static unsigned int iotime_task_slot(struct task_struct *tsk)
{
	unsigned int slot, scan;

	if (!MUQSS_IOWNER_WIDTH)
		return 0;

	slot = READ_ONCE(tsk->io_owner_slot);
	if (slot)
		return slot;

	if (tsk->flags & (PF_KTHREAD | PF_EXITING))
		return 0;

	spin_lock(&iowner_lock);

	/*
	 * Recheck PF_EXITING under the lock. It is set well before
	 * muqss_iotime_release_slot() runs, and that also takes this lock, so
	 * a task seen here as not exiting either has not reached exit or will
	 * release the slot we are about to hand it. Without the recheck a slot
	 * claimed for a remote task racing through exit would never be freed.
	 */
	if (tsk->flags & PF_EXITING)
		goto out;

	/* Raced with another dirty on the same task. */
	if (tsk->io_owner_slot) {
		slot = tsk->io_owner_slot;
		goto out;
	}

	/* Prefer a free slot. */
	for (scan = 1; scan < IOWNER_SLOTS; scan++) {
		if (!iowner_table[scan]) {
			slot = scan;
			goto claim;
		}
	}

	/*
	 * Table full of live dirtiers. Leave this task unattributed rather
	 * than evicting somebody still using their slot: a stolen slot would
	 * silently charge one task's writeback to another, which is worse
	 * than not charging it at all.
	 */
	atomic64_inc(&iowner_exhausted);
	slot = 0;
	goto out;

claim:
	iowner_table[slot] = get_task_struct(tsk);
	WRITE_ONCE(tsk->io_owner_slot, slot);
out:
	spin_unlock(&iowner_lock);
	return slot;
}

unsigned int muqss_iotime_owner_slot(void)
{
	struct task_struct *tsk = current;
	struct task_struct *owner;

	/*
	 * A thread running inside a proxy window dirties pages on behalf of
	 * the task that window belongs to, so tag them with that task. Doing
	 * otherwise would both misattribute the writeback and, for io-wq
	 * workers, let a churn of short lived workers consume the whole slot
	 * table.
	 *
	 * An io-wq worker outside a window has nobody to speak for and must
	 * not claim a slot of its own. A kworker outside a window needs no
	 * such test: iotime_task_slot() refuses PF_KTHREAD outright.
	 */
	owner = READ_ONCE(tsk->io_owner_override);
	if (owner)
		tsk = owner;
	else if (tsk->flags & PF_IO_WORKER)
		return 0;

	return iotime_task_slot(tsk);
}

struct task_struct *muqss_iotime_proxy_begin(struct task_struct *owner)
{
	struct task_struct *prev = current->io_owner_override;

	WRITE_ONCE(current->io_owner_override, owner);
	return prev;
}

void muqss_iotime_proxy_end(struct task_struct *prev)
{
	WRITE_ONCE(current->io_owner_override, prev);
}

void muqss_work_set_owner(struct work_struct *work)
{
	work->muqss_owner_slot = muqss_iotime_owner_slot();
}

void muqss_kerntime_begin(struct muqss_kern_window *w, unsigned int slot)
{
	w->prev = NULL;
	w->start = 0;

	/*
	 * Resolving the slot is what costs here, so it gates everything else.
	 * Work queued by kernel threads on their own account carries slot 0
	 * and leaves this function having done one branch, which matters:
	 * timers, readahead, RCU and any amount of driver housekeeping run
	 * through work items that belong to nobody.
	 */
	w->owner = muqss_iotime_owner_task(slot);
	if (!w->owner)
		return;

	w->prev = muqss_iotime_proxy_begin(w->owner);
	w->start = muqss_task_runtime_live();
}

void muqss_kerntime_end(struct muqss_kern_window *w)
{
	s64 ns;

	if (!w->owner)
		return;

	ns = muqss_task_runtime_live() - w->start;
	muqss_iotime_proxy_end(w->prev);

	/*
	 * Runtime rather than elapsed time, so a worker preempted for a whole
	 * scheduling round does not hand the bill for it to the task it is
	 * working for. Negative or absurd deltas are possible if the two ends
	 * of the window landed either side of a clock warp, and are dropped
	 * rather than clamped: there is no sensible value to substitute.
	 */
	if (ns > 0) {
		atomic64_add(ns, &w->owner->kern_time_ns);
		atomic64_add(ns, &w->owner->kern_debt_ns);
		atomic64_inc(&kernwork_charged);
		atomic64_add(ns, &kernwork_ns);
	}

	put_task_struct(w->owner);
	w->owner = NULL;
}

struct task_struct *muqss_iotime_owner_task(unsigned int slot)
{
	struct task_struct *tsk = NULL;

	if (!slot || slot >= IOWNER_SLOTS)
		return NULL;

	spin_lock(&iowner_lock);
	if (iowner_table[slot])
		tsk = get_task_struct(iowner_table[slot]);
	spin_unlock(&iowner_lock);

	return tsk;
}

void muqss_iotime_release_slot(struct task_struct *p)
{
	unsigned int slot = READ_ONCE(p->io_owner_slot);

	if (!slot)
		return;

	spin_lock(&iowner_lock);
	if (iowner_table[slot] == p) {
		iowner_table[slot] = NULL;
		WRITE_ONCE(p->io_owner_slot, 0);
		spin_unlock(&iowner_lock);
		put_task_struct(p);
		return;
	}
	WRITE_ONCE(p->io_owner_slot, 0);
	spin_unlock(&iowner_lock);
}

void muqss_iotime_dirty_folio(struct folio *folio)
{
	unsigned int slot = muqss_iotime_owner_slot();

	if (slot)
		folio_set_io_owner(folio, slot);
}

void muqss_iotime_task_init(struct task_struct *p)
{
	atomic64_set(&p->io_latency_ns, 0);
	atomic64_set(&p->io_count, 0);
	atomic64_set(&p->io_occupancy_ns, 0);
	atomic64_set(&p->io_debt_ns, 0);
	atomic64_set(&p->kern_time_ns, 0);
	atomic64_set(&p->kern_debt_ns, 0);
	p->io_owner_slot = 0;
	p->io_owner_override = NULL;
}

void muqss_iotime_set_owner(struct bio *bio)
{
	struct task_struct *tsk = current;

	if (bio->bi_muqss_owner)
		return;

	/*
	 * Not in task context: current is whatever this interrupted, which
	 * has nothing to do with the I/O. Bios submitted from softirq or hard
	 * IRQ, such as a filesystem completing one write by starting another,
	 * would otherwise be charged to a bystander.
	 */
	if (!in_task())
		return;

	/*
	 * Passthrough requests are not issued on any task's behalf. Neither
	 * is the flush machinery's own empty bio, but REQ_PREFLUSH is also set
	 * on an ordinary write that wants a cache flush ahead of it, and that
	 * write is somebody's. Test for the payload rather than the flag.
	 */
	if (blk_op_is_passthrough(bio->bi_opf))
		return;
	if ((bio->bi_opf & REQ_PREFLUSH) && !bio_has_data(bio))
		return;

	if (tsk->flags & (PF_KTHREAD | PF_IO_WORKER)) {
		struct task_struct *owner;
		struct folio *folio;

		/*
		 * io_uring punted this submission to an io-wq worker, which
		 * published the task that queued the request. Charge that
		 * task: the worker is an anonymous thread that exists only to
		 * run somebody else's I/O.
		 *
		 * Writeback that happens to be issued from inside the same
		 * window is charged here too, which is a small misattribution
		 * within one application rather than a loss.
		 */
		owner = READ_ONCE(tsk->io_owner_override);
		if (owner) {
			atomic64_inc(&iowner_proxied);
			bio->bi_muqss_owner = get_task_struct(owner);
			return;
		}

		/*
		 * Writeback. current is a flusher thread rather than the task
		 * that dirtied the data, so recover the owner from the folio,
		 * where it was recorded at dirty time.
		 *
		 * The first page decides for the whole bio. A writeback bio
		 * normally covers one file's pages dirtied by one task, and
		 * reading every page to take a vote would put a loop on the
		 * submit path for a heuristic that does not need it.
		 *
		 * Cloned bios are skipped: their bi_io_vec belongs to the
		 * parent, and muqss_iotime_clone_owner() has already carried
		 * the owner across.
		 */
		if (op_is_write(bio->bi_opf) && bio->bi_vcnt &&
		    !bio_flagged(bio, BIO_CLONED)) {
			folio = page_folio(bio->bi_io_vec[0].bv_page);
			bio->bi_muqss_owner =
				muqss_iotime_owner_task(folio_io_owner(folio));
			return;
		}

		/*
		 * An io_uring SQPOLL thread submits for whoever shares its
		 * ring, and the request does not record which task that was;
		 * with IORING_SETUP_ATTACH_WQ it need not be a single one.
		 * The thread is a long lived schedulable task in its own
		 * right though, so charge it directly rather than lose the
		 * time: the device cost is real, and the sq thread is the
		 * entity the scheduler can actually act on.
		 *
		 * Kernel threads proper get nothing.
		 */
		if (tsk->flags & PF_IO_WORKER)
			bio->bi_muqss_owner = get_task_struct(tsk);
		return;
	}

	bio->bi_muqss_owner = get_task_struct(tsk);
}

void muqss_iotime_put_owner(struct bio *bio)
{
	if (bio->bi_muqss_owner) {
		put_task_struct(bio->bi_muqss_owner);
		bio->bi_muqss_owner = NULL;
	}
}

void muqss_iotime_clone_owner(struct bio *bio, struct bio *bio_src)
{
	muqss_iotime_put_owner(bio);

	if (bio_src->bi_muqss_owner)
		bio->bi_muqss_owner = get_task_struct(bio_src->bi_muqss_owner);
}

/*
 * Per bio completion. The owner reference is not dropped here: bio_endio()
 * and bio_uninit() release it alongside the blkg reference, which keeps the
 * lifetime rules identical to the ones the block layer already follows.
 */
static void muqss_iotime_done_bio(struct rq_qos *rqos, struct bio *bio)
{
	struct blk_iotime *bit = BLK_IOTIME(rqos);
	struct task_struct *tsk = bio->bi_muqss_owner;
	u64 now;

	if (!tsk) {
		atomic64_inc(&bit->unattributed);
		return;
	}
	if (!bio->issue_time_ns) {
		atomic64_inc(&bit->nostamp_bio);
		return;
	}

	now = blk_time_get_ns();
	if (now <= bio->issue_time_ns)
		return;

	atomic64_add(now - bio->issue_time_ns, &tsk->io_latency_ns);
	atomic64_inc(&tsk->io_count);
}

/*
 * A request is being built from its first bio. Take our own reference for the
 * request: the bio's reference is released by bio_endio() during
 * blk_update_request(), which runs before rq_qos_done(), so borrowing it would
 * leave a dangling pointer at completion.
 */
static void muqss_iotime_track(struct rq_qos *rqos, struct request *rq,
			       struct bio *bio)
{
	if (bio->bi_muqss_owner)
		rq->muqss_owner = get_task_struct(bio->bi_muqss_owner);
}

/*
 * A further bio is merging into an existing request. Occupancy is measured
 * per request and cannot be split between owners, so a request that ends up
 * serving more than one task is disowned rather than charged to whichever
 * task happened to be first.
 */
static void muqss_iotime_merge(struct rq_qos *rqos, struct request *rq,
			       struct bio *bio)
{
	if (!rq->muqss_owner || rq->muqss_owner == bio->bi_muqss_owner)
		return;

	put_task_struct(rq->muqss_owner);
	rq->muqss_owner = NULL;
	atomic64_inc(&BLK_IOTIME(rqos)->mixed);
}

/*
 * Two whole requests are being merged: next's bios are about to be appended to
 * rq and next freed. The rq_qos ->merge hook does not cover this, it only sees
 * a bio being merged into a request, so without this the survivor keeps its own
 * owner and is charged for both requests' device time.
 *
 * Called from attempt_merge() rather than being an rq_qos op because the
 * framework has no request-to-request callback to hang it on.
 */
void muqss_iotime_merge_requests(struct request *rq, struct request *next)
{
	struct rq_qos *rqos;

	if (!rq->muqss_owner || rq->muqss_owner == next->muqss_owner)
		return;

	put_task_struct(rq->muqss_owner);
	rq->muqss_owner = NULL;

	rqos = rq_qos_id(rq->q, RQ_QOS_MUQSS_IOTIME);
	if (rqos)
		atomic64_inc(&BLK_IOTIME(rqos)->mixed);
}

/* Per request completion. This is where device occupancy is attributed. */
static void muqss_iotime_done(struct rq_qos *rqos, struct request *rq)
{
	struct blk_iotime *bit = BLK_IOTIME(rqos);
	struct task_struct *tsk = rq->muqss_owner;
	u64 now, occupancy;

	if (!(rq->rq_flags & RQF_STATS) || !rq->io_start_time_ns)
		goto out;

	/*
	 * Use the timestamp captured when the request completed, not one
	 * taken here: rq_qos_done() runs after blk_update_request() has
	 * completed every bio, so measuring now would fold the completion
	 * path into what is supposed to be device service time. That bias is
	 * roughly constant per request, which would quietly turn part of the
	 * charge into a per-I/O-count charge and penalise small random I/O.
	 *
	 * Consuming the stamp also makes this idempotent. rq_qos_done() runs
	 * twice for a request with an end_io handler that frees it: once from
	 * __blk_mq_end_request() and again from blk_mq_free_request(). Every
	 * policy has to tolerate that. Clearing muqss_owner alone stopped the
	 * task being charged twice but left the device totals below counting
	 * the same request on both passes.
	 *
	 * A request that never went through blk_mq_end_request() has no stamp
	 * and is not measured. Timing it here instead would report the
	 * completion path as device time, which is the bias this exists to
	 * avoid; unstamped counts them so the loss stays visible.
	 */
	now = rq->muqss_done_ns;
	if (!now) {
		atomic64_inc(&bit->unstamped);
		goto out;
	}
	rq->muqss_done_ns = 0;

	if (now <= rq->io_start_time_ns)
		goto out;

	occupancy = now - rq->io_start_time_ns;
	atomic64_add(occupancy, &bit->occupancy_ns);
	atomic64_inc(&bit->requests);

	if (tsk) {
		atomic64_add(occupancy, &tsk->io_occupancy_ns);
		atomic64_add(occupancy, &tsk->io_debt_ns);
	}
out:
	if (tsk) {
		put_task_struct(tsk);
		rq->muqss_owner = NULL;
	}
}

static void muqss_iotime_exit(struct rq_qos *rqos)
{
	struct blk_iotime *bit = BLK_IOTIME(rqos);

	/*
	 * QUEUE_FLAG_BIO_ISSUE_TIME is deliberately left set. blk-iolatency
	 * uses the same flag, and the queue is being torn down here anyway.
	 */
	blk_stat_disable_accounting(rqos->disk->queue);
	kfree(bit);
}

#ifdef CONFIG_BLK_DEBUG_FS
static int muqss_iotime_occupancy_show(void *data, struct seq_file *m)
{
	struct rq_qos *rqos = data;

	seq_printf(m, "%llu\n",
		   (u64)atomic64_read(&BLK_IOTIME(rqos)->occupancy_ns));
	return 0;
}

static int muqss_iotime_requests_show(void *data, struct seq_file *m)
{
	struct rq_qos *rqos = data;

	seq_printf(m, "%llu\n",
		   (u64)atomic64_read(&BLK_IOTIME(rqos)->requests));
	return 0;
}

static int muqss_iotime_unattributed_show(void *data, struct seq_file *m)
{
	struct rq_qos *rqos = data;

	seq_printf(m, "%llu\n",
		   (u64)atomic64_read(&BLK_IOTIME(rqos)->unattributed));
	return 0;
}

static int muqss_iotime_mixed_show(void *data, struct seq_file *m)
{
	struct rq_qos *rqos = data;

	seq_printf(m, "%llu\n", (u64)atomic64_read(&BLK_IOTIME(rqos)->mixed));
	return 0;
}

static int muqss_iotime_unstamped_show(void *data, struct seq_file *m)
{
	struct rq_qos *rqos = data;

	seq_printf(m, "%llu\n",
		   (u64)atomic64_read(&BLK_IOTIME(rqos)->unstamped));
	return 0;
}

static int muqss_iotime_nostamp_bio_show(void *data, struct seq_file *m)
{
	struct rq_qos *rqos = data;

	seq_printf(m, "%llu\n",
		   (u64)atomic64_read(&BLK_IOTIME(rqos)->nostamp_bio));
	return 0;
}

static int muqss_iotime_exhausted_show(void *data, struct seq_file *m)
{
	seq_printf(m, "%llu\n", (u64)atomic64_read(&iowner_exhausted));
	return 0;
}

static int muqss_iotime_proxied_show(void *data, struct seq_file *m)
{
	seq_printf(m, "%llu\n", (u64)atomic64_read(&iowner_proxied));
	return 0;
}

static int muqss_iotime_owner_width_show(void *data, struct seq_file *m)
{
	seq_printf(m, "%u\n", MUQSS_IOWNER_WIDTH);
	return 0;
}

static int muqss_iotime_kernwork_show(void *data, struct seq_file *m)
{
	seq_printf(m, "%llu\n", (u64)atomic64_read(&kernwork_charged));
	return 0;
}

static int muqss_iotime_kernwork_ns_show(void *data, struct seq_file *m)
{
	seq_printf(m, "%llu\n", (u64)atomic64_read(&kernwork_ns));
	return 0;
}

static const struct blk_mq_debugfs_attr muqss_iotime_debugfs_attrs[] = {
	{"occupancy_ns", 0400, muqss_iotime_occupancy_show},
	{"requests", 0400, muqss_iotime_requests_show},
	{"unattributed", 0400, muqss_iotime_unattributed_show},
	{"mixed", 0400, muqss_iotime_mixed_show},
	{"unstamped", 0400, muqss_iotime_unstamped_show},
	{"nostamp_bio", 0400, muqss_iotime_nostamp_bio_show},
	{"owner_slots_exhausted", 0400, muqss_iotime_exhausted_show},
	{"proxied", 0400, muqss_iotime_proxied_show},
	{"owner_tag_bits", 0400, muqss_iotime_owner_width_show},
	{"kernwork", 0400, muqss_iotime_kernwork_show},
	{"kernwork_ns", 0400, muqss_iotime_kernwork_ns_show},
	{},
};
#endif

static const struct rq_qos_ops muqss_iotime_ops = {
	.track		= muqss_iotime_track,
	.merge		= muqss_iotime_merge,
	.done		= muqss_iotime_done,
	.done_bio	= muqss_iotime_done_bio,
	.exit		= muqss_iotime_exit,
#ifdef CONFIG_BLK_DEBUG_FS
	.debugfs_attrs	= muqss_iotime_debugfs_attrs,
#endif
};

void muqss_iotime_enable(struct gendisk *disk)
{
	struct request_queue *q = disk->queue;
	unsigned int memflags;
	struct blk_iotime *bit;
	int ret;

	if (!queue_is_mq(q))
		return;

	/*
	 * Only attach to queues backed by real hardware.
	 *
	 * A stacking driver that uses blk-mq -- loop is the common one --
	 * serves its requests by doing I/O to another block device, which runs
	 * this policy too. Accounting both charges the task twice for one
	 * trip to the disk, and with the deadline charge live that is a real
	 * distortion rather than a cosmetic one.
	 *
	 * Drivers for physical devices pass their parent through
	 * device_add_disk(): nvme passes ctrl->device, sd its scsi_device,
	 * virtio_blk its virtio device. Virtual ones call add_disk(), which
	 * is device_add_disk(NULL, ...) -- loop, brd, zram, nbd. So the
	 * absence of a parent is a serviceable test for "not a device whose
	 * busy time means anything".
	 *
	 * Bio based stacking drivers (dm, md) never reach here at all, having
	 * failed queue_is_mq() above.
	 */
	if (!disk_to_dev(disk)->parent)
		return;

	bit = kzalloc_obj(*bit);
	if (!bit)
		return;

	mutex_lock(&q->rq_qos_mutex);
	ret = rq_qos_add(&bit->rqos, disk, RQ_QOS_MUQSS_IOTIME,
			 &muqss_iotime_ops);
	mutex_unlock(&q->rq_qos_mutex);
	if (ret) {
		kfree(bit);
		return;
	}

	/* Needed for rq->io_start_time_ns to be stamped at dispatch. */
	blk_stat_enable_accounting(q);
	/* Needed for bio->issue_time_ns to be stamped at submit. */
	blk_queue_flag_set(QUEUE_FLAG_BIO_ISSUE_TIME, q);

	memflags = blk_debugfs_lock(q);
	blk_mq_debugfs_register_rq_qos(q);
	blk_debugfs_unlock(q, memflags);
}
