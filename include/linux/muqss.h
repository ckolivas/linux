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

static inline bool iso_task(struct task_struct *p)
{
	return (p->policy == SCHED_ISO);
}

#else /* CONFIG_SCHED_MUQSS */

#define tsk_seruntime(t)	((t)->se.sum_exec_runtime)
#define tsk_rttimeout(t)	((t)->rt.timeout)

static inline bool iso_task(struct task_struct *p)
{
	return false;
}

#endif /* CONFIG_SCHED_MUQSS */

#endif /* _LINUX_MUQSS_H */
