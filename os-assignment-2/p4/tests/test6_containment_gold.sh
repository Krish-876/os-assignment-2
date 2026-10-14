#!/bin/bash
# Test 6: same as test 5 but the offender is GOLD. Outcome must be identical.
. "$(dirname "$0")/common.sh"
need_root; need_kernel
tier reset
tier set "$USER_A" gold
low_threshold_warning
dmesg -C 2> /dev/null
launch_bomb "$USER_A"
echo "== dmesg (expect THROTTLE, FREEZE, REAP despite GOLD) =="
dmesg | grep fork_guard
cat /proc/fork_guard
restore_threshold
tier reset
