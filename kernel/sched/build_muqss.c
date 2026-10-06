// SPDX-License-Identifier: GPL-2.0-only
/*
 * The scheduler support files MuQSS retains from mainline, built in a single
 * compilation unit for the same build-efficiency reasons as build_policy.c
 * and build_utility.c.
 *
 * MuQSS replaces core.c, fair.c, rt.c, deadline.c, syscalls.c and stop_task.c
 * outright, so neither of the mainline aggregates can be used as-is. The
 * files gathered here are the subset of build_utility.c and build_policy.c
 * that survives, and they are #included rather than built individually
 * because several of them (clock.c, completion.c, psi.c) carry no includes of
 * their own and only compile as part of an aggregate.
 *
 * Deliberately absent, and why:
 *
 *   fair.c rt.c deadline.c ext.c stop_task.c  policy replaced by MuQSS
 *   cpudeadline.c cpupri.c pelt.c            supporting data for those classes
 *   syscalls.c                               MuQSS implements the syscalls
 *   core_sched.c                             SCHED_CORE gated off
 *   debug.c                                  CFS/EEVDF runqueue introspection
 *   loadavg.c                                MuQSS keeps its own load average
 *   autogroup.c cpuacct.c                    gated off in Kconfig.MuQSS
 */

/* Headers: */
#include <linux/sched/clock.h>
#include <linux/sched/cputime.h>
#include <linux/sched/debug.h>
#include <linux/sched/hotplug.h>
#include <linux/sched/isolation.h>
#include <linux/sched/loadavg.h>
#include <linux/sched/nohz.h>
#include <linux/sched/mm.h>
#include <linux/sched/posix-timers.h>
#include <linux/sched/rseq_api.h>
#include <linux/sched/rt.h>
#include <linux/sched/task_stack.h>

#include <linux/cpufreq.h>
#include <linux/cpuidle.h>
#include <linux/cpumask_api.h>
#include <linux/cpuset.h>
#include <linux/ctype.h>
#include <linux/debugfs.h>
#include <linux/energy_model.h>
#include <linux/hashtable_api.h>
#include <linux/irq.h>
#include <linux/jiffies.h>
#include <linux/kobject_api.h>
#include <linux/livepatch.h>
#include <linux/membarrier.h>
#include <linux/memblock.h>
#include <linux/mempolicy.h>
#include <linux/nmi.h>
#include <linux/nospec.h>
#include <linux/percpu-rwsem.h>
#include <linux/pm.h>
#include <linux/proc_fs.h>
#include <linux/psi.h>
#include <linux/ptrace_api.h>
#include <linux/rhashtable.h>
#include <linux/sched_clock.h>
#include <linux/security.h>
#include <linux/seq_buf.h>
#include <linux/seqlock_api.h>
#include <linux/slab.h>
#include <linux/spinlock_api.h>
#include <linux/suspend.h>
#include <linux/swait_api.h>
#include <linux/sysrq.h>
#include <linux/timex.h>
#include <linux/tsacct_kern.h>
#include <linux/utsname.h>
#include <linux/vtime.h>
#include <linux/wait_api.h>
#include <linux/workqueue_api.h>

#include <uapi/linux/prctl.h>
#include <uapi/linux/sched/types.h>

#include <asm/switch_to.h>

#include <trace/events/power.h>

#include "sched.h"
#include "stats.h"

/* psi.c needs wq_worker_last_func(); mainline gets this via sched.h. */
#include "../workqueue_internal.h"

/* Source code modules: */

#include "clock.c"

#ifdef CONFIG_CPU_FREQ
# include "cpufreq.c"
#endif

#ifdef CONFIG_CPU_FREQ_GOV_SCHEDUTIL
# include "cpufreq_schedutil.c"
#endif

#ifdef CONFIG_SCHEDSTATS
# include "stats.c"
#endif

#include "completion.c"
#include "swait.c"
#include "wait_bit.c"
#include "wait.c"

#include "cputime.c"
#include "idle.c"

#ifdef CONFIG_SMP
# include "topology.c"
#endif

#ifdef CONFIG_PSI
# include "psi.c"
#endif

#ifdef CONFIG_MEMBARRIER
# include "membarrier.c"
#endif

#ifdef CONFIG_CPU_ISOLATION
# include "isolation.c"
#endif

#include "skip_list.c"
