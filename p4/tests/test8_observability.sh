#!/bin/bash
# Test 8: show /proc stats, tier table and the kernel log lines.
. "$(dirname "$0")/common.sh"
need_root; need_kernel
tier set "$USER_A" gold
tier set "$USER_B" bronze
echo "== /proc/fork_guard =="; cat /proc/fork_guard
echo "== /proc/sched_tier =="; cat /proc/sched_tier
echo "== tier show =="; tier show
echo "== sysctl =="; sysctl -a 2> /dev/null | grep 'kernel.fg_'
echo "== dmesg =="; dmesg | grep fork_guard | tail -30
tier reset
