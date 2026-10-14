#!/bin/bash
# Test 5: fork bomb as an untiered ordinary user. The ladder must stop it.
. "$(dirname "$0")/common.sh"
need_root; need_kernel
tier reset
low_threshold_warning
dmesg -C 2> /dev/null
launch_bomb "$USER_A"
echo "== dmesg (expect THROTTLE, FREEZE, REAP) =="
dmesg | grep fork_guard
cat /proc/fork_guard
restore_threshold
