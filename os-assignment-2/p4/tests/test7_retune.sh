#!/bin/bash
# Test 7: lower fg_threshold at runtime; containment must trigger sooner.
. "$(dirname "$0")/common.sh"
need_root; need_kernel
tier reset
ORIG_THRESHOLD=$(sysctl -n kernel.fg_threshold)
ORIG_COOLDOWN=$(sysctl -n kernel.fg_cooldown_s)
echo "WARNING: using low thresholds (150 then 30) to protect the VM."
sysctl -w kernel.fg_cooldown_s=2 > /dev/null
for thr in 150 30; do
    sysctl -w kernel.fg_threshold=$thr
    sleep 3                      # let the previous offender be forgiven
    dmesg -C 2> /dev/null
    echo "== threshold=$thr =="
    launch_bomb "$USER_A"
    dmesg | grep -E "fork_guard: (THROTTLE|FREEZE|REAP)" | head -5
done
sysctl -w kernel.fg_threshold="$ORIG_THRESHOLD"
sysctl -w kernel.fg_cooldown_s="$ORIG_COOLDOWN" > /dev/null
