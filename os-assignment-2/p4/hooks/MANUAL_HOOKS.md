# Manual hook instructions (Linux 6.8)

Use these if `apply_hooks.py` cannot find an anchor. Paths are relative to the
kernel source root (`linux-6.8/`). These anchors were verified against the
real v6.8 tree.

## 0. New files
Copy `p4/kernel/fork_guard.c` to `kernel/fork_guard.c` and
`p4/include/linux/fork_guard.h` to `include/linux/fork_guard.h`.

## 1. kernel/Makefile
Append at the end of the file:

    obj-y += fork_guard.o

## 2. kernel/fork.c
Add with the other includes (e.g. after `#include <linux/anon_inodes.h>`):

    #include <linux/fork_guard.h>

In `kernel_clone()`, after the local declarations (`pid_t nr;`) and before the
`CLONE_PIDFD` comment, add:

    if (fg_check_fork())
        return -EAGAIN;

## 3. kernel/sched/core.c
Add with the other includes (e.g. after `#include <linux/highmem.h>`):

    #include <linux/fork_guard.h>

In `sched_fork()`, after the `if (unlikely(p->sched_reset_on_fork)) { ... }`
block and right before `if (dl_prio(p->prio))`, add:

    if (p->policy == SCHED_NORMAL || p->policy == SCHED_BATCH) {
        int fg_nice;

        if (fg_tier_nice(p->real_cred->uid, &fg_nice)) {
            p->static_prio = NICE_TO_PRIO(fg_nice);
            p->prio = p->normal_prio = p->static_prio;
            set_load_weight(p, false);
        }
    }

`set_load_weight(struct task_struct *p, bool update_load)` is the 6.8
signature and is defined earlier in the same file.
