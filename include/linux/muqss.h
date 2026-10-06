/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Scheduler-neutral accessors for task fields that MuQSS keeps somewhere
 * other than the mainline scheduling entities.
 *
 * Kept out of <linux/sched.h> so that enabling MuQSS costs that header only
 * the task_struct members themselves. Include this after task_struct is
 * defined; <linux/sched.h> already does so on your behalf.
 */
#ifndef _LINUX_MUQSS_H
#define _LINUX_MUQSS_H

#ifdef CONFIG_SCHED_MUQSS

/* MuQSS accounts runtime and RT timeouts directly on the task. */
#define tsk_seruntime(t)	((t)->sched_time)
#define tsk_rttimeout(t)	((t)->rt_timeout)

/*
 * The rtmutex PI chain sorts equal-priority waiters by SCHED_DEADLINE
 * deadline. MuQSS implements no deadline class, and that tie-break is only
 * consulted when dl_prio(prio) holds - i.e. prio < MAX_DL_PRIO, which is 0 -
 * so it is unreachable here. Report 0 rather than carry a dl entity.
 */
#define tsk_dl_deadline(t)	(0)

/* No deadline class: no budget to overrun and no bandwidth to migrate. */
#define tsk_dl_overrun(t)	(0)
#define tsk_dl_bw(t)		(0)

static inline bool iso_task(struct task_struct *p)
{
	return (p->policy == SCHED_ISO);
}

#else /* CONFIG_SCHED_MUQSS */

#define tsk_seruntime(t)	((t)->se.sum_exec_runtime)
#define tsk_rttimeout(t)	((t)->rt.timeout)
#define tsk_dl_deadline(t)	((t)->dl.deadline)
#define tsk_dl_overrun(t)	((t)->dl.dl_overrun)
#define tsk_dl_bw(t)		((t)->dl.dl_bw)

static inline bool iso_task(struct task_struct *p)
{
	return false;
}

#endif /* CONFIG_SCHED_MUQSS */

#endif /* _LINUX_MUQSS_H */
