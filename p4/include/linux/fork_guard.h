/* SPDX-License-Identifier: GPL-2.0 */
/*
 * fork_guard.h - fork storm containment and per-user CPU tiers.
 * Implemented in kernel/fork_guard.c.
 */
#ifndef _LINUX_FORK_GUARD_H
#define _LINUX_FORK_GUARD_H

#include <linux/types.h>
#include <linux/uidgid.h>

/*
 * Called at the top of kernel_clone(). Returns 0 to allow the fork or
 * -EAGAIN to refuse it. Never looks at the tier table.
 */
int fg_check_fork(void);

/*
 * Called from sched_fork(). If the user owning the new task has an explicit
 * tier, stores the nice value to use in *nice and returns true.
 */
bool fg_tier_nice(kuid_t uid, int *nice);

#endif /* _LINUX_FORK_GUARD_H */
