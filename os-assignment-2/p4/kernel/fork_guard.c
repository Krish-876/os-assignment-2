// SPDX-License-Identifier: GPL-2.0
/*
 * kernel/fork_guard.c
 *
 * Fork storm containment (escalation ladder), tiered CPU scheduling and
 * observability for CS F372 Assignment 2, Problem 4. Written against Linux 6.8.
 *
 * Hooks (see p4/hooks):
 *   kernel/fork.c        kernel_clone()  -> fg_check_fork()   (top of function)
 *   kernel/sched/core.c  sched_fork()    -> fg_tier_nice()    (before dl_prio check)
 *   kernel/Makefile      obj-y += fork_guard.o
 *
 * Design rule: fg_check_fork() never looks at the tier table, so a GOLD user
 * is contained exactly like anybody else. Safety overrides privilege.
 */

#include <linux/fork_guard.h>

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/sched/prio.h>
#include <linux/cred.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/pid.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/sysctl.h>
#include <linux/uaccess.h>
#include <linux/jiffies.h>
#include <linux/delay.h>
#include <linux/string.h>
#include <linux/math64.h>
#include <linux/minmax.h>

/* ------------------------------------------------------------------ */
/* Constants                                                          */
/* ------------------------------------------------------------------ */

#define FG_MAX_USERS		64

/* time between ladder stages while the user keeps exceeding the limit */
#define FG_STAGE_GAP_MS		150
/* delay between SIGSTOP and the follow-up SIGKILL (delayed work) */
#define FG_REAP_DELAY_MS	200

/*
 * Tier nice values. nice -7 -> CFS weight 4904, nice 0 -> 1024,
 * nice +5 -> 335. Against each other GOLD:BRONZE is 4904:335 so BRONZE
 * gets 335 / (4904 + 335) = about 6.4% of a contended core, never zero.
 *
 * FG_NICE_FLOOR / FG_NICE_CEIL are the clamp: whatever a tier maps to, the
 * result is forced into [-7, +5]. A more extreme nice (say -20 vs +19,
 * weights 88761 : 15, i.e. 0.017%) would effectively starve BRONZE.
 */
#define FG_NICE_GOLD		(-7)
#define FG_NICE_SILVER		0
#define FG_NICE_BRONZE		5
#define FG_NICE_FLOOR		(-7)
#define FG_NICE_CEIL		5

enum fg_tier {
	FG_SILVER = 0,
	FG_GOLD,
	FG_BRONZE,
};

static const char * const fg_tier_names[] = { "silver", "gold", "bronze" };

/* ------------------------------------------------------------------ */
/* Sysctl-backed tunables                                             */
/* ------------------------------------------------------------------ */

static unsigned int fg_enabled = 1;
static unsigned int fg_threshold = 200;
static unsigned int fg_window_ms = 1000;
static unsigned int fg_throttle_ms = 40;
static unsigned int fg_cooldown_s = 10;

static unsigned int fg_zero;
static unsigned int fg_one = 1;
static unsigned int fg_thr_min = 1, fg_thr_max = 1000000;
static unsigned int fg_win_min = 10, fg_win_max = 60000;
static unsigned int fg_thl_max = 5000;
static unsigned int fg_cd_min = 1, fg_cd_max = 3600;

static struct ctl_table fg_sysctl_table[] = {
	{
		.procname	= "fg_enabled",
		.data		= &fg_enabled,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= proc_douintvec_minmax,
		.extra1		= &fg_zero,
		.extra2		= &fg_one,
	},
	{
		.procname	= "fg_threshold",
		.data		= &fg_threshold,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= proc_douintvec_minmax,
		.extra1		= &fg_thr_min,
		.extra2		= &fg_thr_max,
	},
	{
		.procname	= "fg_window_ms",
		.data		= &fg_window_ms,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= proc_douintvec_minmax,
		.extra1		= &fg_win_min,
		.extra2		= &fg_win_max,
	},
	{
		.procname	= "fg_throttle_ms",
		.data		= &fg_throttle_ms,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= proc_douintvec_minmax,
		.extra1		= &fg_zero,
		.extra2		= &fg_thl_max,
	},
	{
		.procname	= "fg_cooldown_s",
		.data		= &fg_cooldown_s,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= proc_douintvec_minmax,
		.extra1		= &fg_cd_min,
		.extra2		= &fg_cd_max,
	},
	{ }	/* empty sentinel, 6.8 still needs it */
};

/* ------------------------------------------------------------------ */
/* Per-user state                                                     */
/* ------------------------------------------------------------------ */

/*
 * Sliding window estimate with two buckets: "cur" counts forks since
 * win_start, "prev" is the count of the previous full window. The estimate
 * is cur + prev * (window - elapsed) / window, i.e. the previous bucket
 * fades out linearly as the current one fills. This approximates a true
 * sliding window with O(1) state per user.
 */
struct fg_user {
	bool		used;
	kuid_t		uid;

	bool		tiered;		/* explicit tier assignment exists */
	enum fg_tier	tier;

	unsigned int	cur;
	unsigned int	prev;
	unsigned long	win_start;

	int		stage;		/* 0..3, highest level reached */
	unsigned long	stage_since;
	unsigned long	last_over;

	unsigned int	n_throttle;
	unsigned int	n_freeze;
	unsigned int	n_reap;
};

static struct fg_user fg_users[FG_MAX_USERS];
static DEFINE_SPINLOCK(fg_lock);	/* protects fg_users[] */
static bool fg_initialized;

/* caller holds fg_lock */
static struct fg_user *fg_find(kuid_t uid, bool create)
{
	struct fg_user *free_slot = NULL, *victim = NULL;
	int i;

	for (i = 0; i < FG_MAX_USERS; i++) {
		struct fg_user *u = &fg_users[i];

		if (u->used && uid_eq(u->uid, uid))
			return u;
		if (!u->used && !free_slot)
			free_slot = u;
		if (u->used && !u->tiered && u->stage == 0 && !victim)
			victim = u;
	}
	if (!create)
		return NULL;
	if (!free_slot)
		free_slot = victim;	/* recycle an idle, untiered entry */
	if (!free_slot)
		return NULL;

	memset(free_slot, 0, sizeof(*free_slot));
	free_slot->used = true;
	free_slot->uid = uid;
	free_slot->win_start = jiffies;
	return free_slot;
}

/* advance the two buckets to "now" (caller holds fg_lock) */
static void fg_roll(struct fg_user *u, unsigned long now, unsigned long w)
{
	unsigned long el = now - u->win_start;

	if (el >= 2 * w) {
		u->prev = 0;
		u->cur = 0;
		u->win_start = now;
	} else if (el >= w) {
		u->prev = u->cur;
		u->cur = 0;
		u->win_start += w;
	}
}

static unsigned int fg_estimate(const struct fg_user *u, unsigned long now,
				unsigned long w)
{
	unsigned long el = now - u->win_start;

	if (el > w)
		el = w;
	return u->cur + (unsigned int)div_u64((u64)u->prev * (w - el), w);
}

/* lazy de-escalation (caller holds fg_lock) */
static void fg_decay(struct fg_user *u, unsigned long now, unsigned long cd)
{
	if (u->stage > 0 && time_after(now, u->last_over + cd)) {
		pr_info("fork_guard: uid=%u forgiven after cooldown (stage %d -> 0)\n",
			from_kuid(&init_user_ns, u->uid), u->stage);
		u->stage = 0;
	}
}

/* ------------------------------------------------------------------ */
/* Mechanism 1: escalation ladder                                     */
/* ------------------------------------------------------------------ */

struct fg_reap_work {
	struct delayed_work	dwork;
	struct pid		*pgrp;		/* reference held */
	kuid_t			uid;
};

/*
 * The group must never be init's group or the group of the session
 * leader (that would be the user's login shell / terminal itself).
 */
static bool fg_pgrp_safe(struct pid *pgrp)
{
	struct pid *sess;
	bool safe;

	if (!pgrp || pid_nr(pgrp) <= 1)
		return false;
	rcu_read_lock();
	sess = task_session(current);
	safe = (pgrp != sess);
	rcu_read_unlock();
	return safe;
}

/*
 * Runs ~FG_REAP_DELAY_MS after FREEZE. A SIGSTOPped group cannot fork any
 * more, so nothing would ever trigger stage 3 from kernel_clone(); this
 * work item is what completes the ladder.
 *
 * Why STOP then KILL is safe: SIGSTOP freezes the group so it stops
 * spawning while we decide. SIGKILL cannot be blocked, caught or ignored,
 * and the signal code wakes TASK_STOPPED tasks to deliver it, so a stopped
 * process still dies. The reverse order would gain nothing.
 */
static void fg_reap_fn(struct work_struct *w)
{
	struct fg_reap_work *rw =
		container_of(to_delayed_work(w), struct fg_reap_work, dwork);
	unsigned long now = jiffies;
	unsigned long cd = (unsigned long)READ_ONCE(fg_cooldown_s) * HZ;
	unsigned long flags;
	struct fg_user *u;
	bool do_kill = false;

	spin_lock_irqsave(&fg_lock, flags);
	u = fg_find(rw->uid, false);
	if (u && u->stage == 2 && time_before(now, u->last_over + cd)) {
		u->stage = 3;
		u->stage_since = now;
		u->n_reap++;
		do_kill = true;
	}
	spin_unlock_irqrestore(&fg_lock, flags);

	if (do_kill) {
		kill_pgrp(rw->pgrp, SIGKILL, 1);
		pr_alert("fork_guard: REAP uid=%u pgid=%d (SIGKILL process group)\n",
			 from_kuid(&init_user_ns, rw->uid), pid_nr(rw->pgrp));
	}
	put_pid(rw->pgrp);
	kfree(rw);
}

int fg_check_fork(void)
{
	enum { A_NONE, A_THROTTLE, A_FREEZE, A_REAP } act = A_NONE;
	bool first_throttle = false;
	unsigned int thr, win_ms, thl_ms;
	unsigned long w, gap, cd, now, flags;
	struct fg_user *u;
	kuid_t uid;
	unsigned int est = 0;
	struct pid *pgrp;
	int uidn;

	if (!READ_ONCE(fg_initialized) || !READ_ONCE(fg_enabled))
		return 0;
	if (current->flags & PF_KTHREAD)
		return 0;
	uid = current_uid();
	if (uid_eq(uid, GLOBAL_ROOT_UID))
		return 0;			/* root is exempt */

	thr = READ_ONCE(fg_threshold);
	win_ms = READ_ONCE(fg_window_ms);
	thl_ms = READ_ONCE(fg_throttle_ms);
	w = max_t(unsigned long, msecs_to_jiffies(win_ms), 1);
	cd = (unsigned long)READ_ONCE(fg_cooldown_s) * HZ;
	gap = msecs_to_jiffies(FG_STAGE_GAP_MS);
	now = jiffies;

	spin_lock_irqsave(&fg_lock, flags);
	u = fg_find(uid, true);
	if (!u) {
		spin_unlock_irqrestore(&fg_lock, flags);
		return 0;		/* table full of live offenders: fail open */
	}
	fg_decay(u, now, cd);
	fg_roll(u, now, w);
	u->cur++;			/* every attempt counts, refused ones too */
	est = fg_estimate(u, now, w);

	if (est > thr) {
		u->last_over = now;
		switch (u->stage) {
		case 0:
			u->stage = 1;
			u->stage_since = now;
			act = A_THROTTLE;
			first_throttle = true;
			break;
		case 1:
			if (time_after_eq(now, u->stage_since + gap)) {
				u->stage = 2;
				u->stage_since = now;
				u->n_freeze++;
				act = A_FREEZE;
			} else {
				act = A_THROTTLE;
			}
			break;
		case 2:
			/* normally the delayed work reaps; this is the fallback */
			if (time_after_eq(now, u->stage_since + gap +
					  msecs_to_jiffies(FG_REAP_DELAY_MS))) {
				u->stage = 3;
				u->stage_since = now;
				u->n_reap++;
				act = A_REAP;
			} else {
				act = A_THROTTLE;
			}
			break;
		default:
			if (time_after_eq(now, u->stage_since + gap)) {
				u->stage_since = now;
				u->n_reap++;
				act = A_REAP;
			} else {
				act = A_THROTTLE;
			}
			break;
		}
		if (act == A_THROTTLE)
			u->n_throttle++;
	}
	spin_unlock_irqrestore(&fg_lock, flags);

	if (act == A_NONE)
		return 0;

	uidn = from_kuid(&init_user_ns, uid);

	if (act == A_THROTTLE) {
		if (first_throttle)
			pr_warn("fork_guard: THROTTLE uid=%u pgid=%d (>%u forks in %u ms)\n",
				uidn, task_pgrp_vnr(current), thr, win_ms);
		else
			pr_warn_ratelimited("fork_guard: THROTTLE uid=%u pgid=%d (>%u forks in %u ms)\n",
					    uidn, task_pgrp_vnr(current), thr, win_ms);
		if (thl_ms)
			msleep(thl_ms);	/* penalty delay, outside the spinlock */
		return -EAGAIN;
	}

	/* FREEZE or REAP: need the process group */
	rcu_read_lock();
	pgrp = get_pid(task_pgrp(current));
	rcu_read_unlock();

	if (!fg_pgrp_safe(pgrp)) {
		pr_warn("fork_guard: uid=%u pgid=%d is init/session-leader group, refusing forks only\n",
			uidn, pid_nr(pgrp));
		put_pid(pgrp);
		return -EAGAIN;
	}

	if (act == A_FREEZE) {
		struct fg_reap_work *rw;

		kill_pgrp(pgrp, SIGSTOP, 1);
		pr_warn("fork_guard: FREEZE uid=%u pgid=%d (SIGSTOP process group)\n",
			uidn, pid_nr(pgrp));

		rw = kzalloc(sizeof(*rw), GFP_KERNEL);
		if (rw) {
			rw->pgrp = get_pid(pgrp);
			rw->uid = uid;
			INIT_DELAYED_WORK(&rw->dwork, fg_reap_fn);
			schedule_delayed_work(&rw->dwork,
					      msecs_to_jiffies(FG_REAP_DELAY_MS));
		}
	} else {
		kill_pgrp(pgrp, SIGKILL, 1);
		pr_alert("fork_guard: REAP uid=%u pgid=%d (SIGKILL process group)\n",
			 uidn, pid_nr(pgrp));
	}
	put_pid(pgrp);
	return -EAGAIN;
}

/* ------------------------------------------------------------------ */
/* Mechanism 2: tiers                                                 */
/* ------------------------------------------------------------------ */

static int fg_tier_to_nice(enum fg_tier t)
{
	int n = FG_NICE_SILVER;

	if (t == FG_GOLD)
		n = FG_NICE_GOLD;
	else if (t == FG_BRONZE)
		n = FG_NICE_BRONZE;
	return clamp(n, FG_NICE_FLOOR, FG_NICE_CEIL);
}

bool fg_tier_nice(kuid_t uid, int *nice)
{
	struct fg_user *u;
	unsigned long flags;
	bool ret = false;

	if (!READ_ONCE(fg_initialized))
		return false;

	spin_lock_irqsave(&fg_lock, flags);
	u = fg_find(uid, false);
	if (u && u->tiered) {
		*nice = fg_tier_to_nice(u->tier);
		ret = true;
	}
	spin_unlock_irqrestore(&fg_lock, flags);
	return ret;
}

/*
 * sched_fork() only sees new tasks, so tasks that already exist are
 * re-niced here. set_user_nice() recomputes the CFS weight and requeues
 * the task, so the change is effective immediately. Called with no
 * spinlock held.
 */
static void fg_retier_user(kuid_t uid, int nice)
{
	struct task_struct *g, *t;

	rcu_read_lock();
	for_each_process_thread(g, t) {
		if (t->flags & PF_KTHREAD)
			continue;
		if (!uid_eq(task_uid(t), uid))
			continue;
		if (t->policy != SCHED_NORMAL && t->policy != SCHED_BATCH)
			continue;	/* leave RT / deadline / idle alone */
		set_user_nice(t, nice);
	}
	rcu_read_unlock();
}

static int fg_do_set(kuid_t uid, enum fg_tier tier)
{
	struct fg_user *u;
	unsigned long flags;
	bool demoted = false;
	kuid_t old = INVALID_UID;
	int i;

	spin_lock_irqsave(&fg_lock, flags);
	if (tier == FG_GOLD) {
		for (i = 0; i < FG_MAX_USERS; i++) {
			struct fg_user *o = &fg_users[i];

			if (o->used && o->tiered && o->tier == FG_GOLD &&
			    !uid_eq(o->uid, uid)) {
				o->tier = FG_SILVER;
				old = o->uid;
				demoted = true;
			}
		}
	}
	u = fg_find(uid, true);
	if (!u) {
		spin_unlock_irqrestore(&fg_lock, flags);
		return -ENOSPC;
	}
	u->tiered = true;
	u->tier = tier;
	spin_unlock_irqrestore(&fg_lock, flags);

	if (demoted) {
		pr_info("fork_guard: tier uid=%u demoted gold -> silver (new gold user)\n",
			from_kuid(&init_user_ns, old));
		fg_retier_user(old, fg_tier_to_nice(FG_SILVER));
	}
	pr_info("fork_guard: tier uid=%u -> %s (nice %d)\n",
		from_kuid(&init_user_ns, uid), fg_tier_names[tier],
		fg_tier_to_nice(tier));
	fg_retier_user(uid, fg_tier_to_nice(tier));
	return 0;
}

static void fg_do_reset(void)
{
	kuid_t ids[FG_MAX_USERS];
	unsigned long flags;
	int n = 0, i;

	spin_lock_irqsave(&fg_lock, flags);
	for (i = 0; i < FG_MAX_USERS; i++) {
		struct fg_user *u = &fg_users[i];

		if (u->used && u->tiered) {
			ids[n++] = u->uid;
			u->tiered = false;
			u->tier = FG_SILVER;
		}
	}
	spin_unlock_irqrestore(&fg_lock, flags);

	for (i = 0; i < n; i++)
		fg_retier_user(ids[i], FG_NICE_SILVER);
	pr_info("fork_guard: tier reset (%d user(s) back to silver)\n", n);
}

static ssize_t fg_tier_write(struct file *file, const char __user *ubuf,
			     size_t len, loff_t *ppos)
{
	char buf[64];
	char *p, *cmd, *uidstr, *tierstr;
	unsigned int uidn;
	enum fg_tier tier;
	kuid_t kuid;
	int ret;

	if (!uid_eq(current_euid(), GLOBAL_ROOT_UID))
		return -EPERM;
	if (len == 0 || len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';

	p = strim(buf);
	cmd = strsep(&p, " ");
	if (cmd && !strcmp(cmd, "reset")) {
		fg_do_reset();
		return len;
	}
	if (!cmd || strcmp(cmd, "set"))
		return -EINVAL;

	uidstr = p ? strsep(&p, " ") : NULL;
	tierstr = p ? strsep(&p, " ") : NULL;
	if (!uidstr || !tierstr)
		return -EINVAL;
	if (kstrtouint(uidstr, 10, &uidn))
		return -EINVAL;

	if (!strcmp(tierstr, "gold"))
		tier = FG_GOLD;
	else if (!strcmp(tierstr, "silver"))
		tier = FG_SILVER;
	else if (!strcmp(tierstr, "bronze"))
		tier = FG_BRONZE;
	else
		return -EINVAL;

	kuid = make_kuid(&init_user_ns, uidn);
	if (!uid_valid(kuid))
		return -EINVAL;

	ret = fg_do_set(kuid, tier);
	return ret ? ret : (ssize_t)len;
}

static int fg_tier_show(struct seq_file *m, void *v)
{
	struct fg_user *u;
	unsigned long flags;
	int i;

	seq_puts(m, "UID TIER NICE\n");
	spin_lock_irqsave(&fg_lock, flags);
	for (i = 0; i < FG_MAX_USERS; i++) {
		u = &fg_users[i];
		if (u->used && u->tiered)
			seq_printf(m, "%u %s %d\n",
				   from_kuid(&init_user_ns, u->uid),
				   fg_tier_names[u->tier],
				   fg_tier_to_nice(u->tier));
	}
	spin_unlock_irqrestore(&fg_lock, flags);
	return 0;
}

static int fg_tier_open(struct inode *inode, struct file *file)
{
	return single_open(file, fg_tier_show, NULL);
}

static const struct proc_ops fg_tier_proc_ops = {
	.proc_open	= fg_tier_open,
	.proc_read	= seq_read,
	.proc_write	= fg_tier_write,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

/* ------------------------------------------------------------------ */
/* Mechanism 3: /proc/fork_guard                                      */
/* ------------------------------------------------------------------ */

struct fg_row {
	unsigned int	uid;
	enum fg_tier	tier;
	unsigned int	forks;
	int		stage;
	unsigned int	n_throttle, n_freeze, n_reap;
};

static int fg_stats_show(struct seq_file *m, void *v)
{
	struct fg_row *rows;
	unsigned long flags, now = jiffies;
	unsigned long w = max_t(unsigned long,
				msecs_to_jiffies(READ_ONCE(fg_window_ms)), 1);
	unsigned long cd = (unsigned long)READ_ONCE(fg_cooldown_s) * HZ;
	int n = 0, i;

	rows = kmalloc_array(FG_MAX_USERS, sizeof(*rows), GFP_KERNEL);
	if (!rows)
		return -ENOMEM;

	spin_lock_irqsave(&fg_lock, flags);
	for (i = 0; i < FG_MAX_USERS; i++) {
		struct fg_user *u = &fg_users[i];

		if (!u->used)
			continue;
		fg_decay(u, now, cd);		/* lazy de-escalation */
		fg_roll(u, now, w);
		rows[n].uid = from_kuid(&init_user_ns, u->uid);
		rows[n].tier = u->tiered ? u->tier : FG_SILVER;
		rows[n].forks = fg_estimate(u, now, w);
		rows[n].stage = u->stage;
		rows[n].n_throttle = u->n_throttle;
		rows[n].n_freeze = u->n_freeze;
		rows[n].n_reap = u->n_reap;
		n++;
	}
	spin_unlock_irqrestore(&fg_lock, flags);

	seq_printf(m, "enabled=%u threshold=%u window_ms=%u throttle_ms=%u cooldown_s=%u\n\n",
		   READ_ONCE(fg_enabled), READ_ONCE(fg_threshold),
		   READ_ONCE(fg_window_ms), READ_ONCE(fg_throttle_ms),
		   READ_ONCE(fg_cooldown_s));
	seq_puts(m, "UID TIER FORKS STAGE THROTTLE FREEZE REAP\n");
	for (i = 0; i < n; i++)
		seq_printf(m, "%u %s %u %d %u %u %u\n", rows[i].uid,
			   fg_tier_names[rows[i].tier], rows[i].forks,
			   rows[i].stage, rows[i].n_throttle, rows[i].n_freeze,
			   rows[i].n_reap);

	kfree(rows);
	return 0;
}

static int fg_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, fg_stats_show, NULL);
}

static const struct proc_ops fg_stats_proc_ops = {
	.proc_open	= fg_stats_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

/* ------------------------------------------------------------------ */
/* Init                                                               */
/* ------------------------------------------------------------------ */

static int __init fork_guard_init(void)
{
	if (!register_sysctl("kernel", fg_sysctl_table))
		pr_warn("fork_guard: sysctl registration failed\n");
	if (!proc_create("fork_guard", 0444, NULL, &fg_stats_proc_ops))
		pr_warn("fork_guard: /proc/fork_guard creation failed\n");
	if (!proc_create("sched_tier", 0644, NULL, &fg_tier_proc_ops))
		pr_warn("fork_guard: /proc/sched_tier creation failed\n");

	WRITE_ONCE(fg_initialized, true);
	pr_info("fork_guard: initialised (threshold=%u/%ums, gold nice=%d)\n",
		fg_threshold, fg_window_ms, FG_NICE_GOLD);
	return 0;
}
late_initcall(fork_guard_init);
