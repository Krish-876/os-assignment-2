#!/bin/bash
# Test 3: USER_A gold, USER_B bronze. Bronze must still accumulate CPU time.
. "$(dirname "$0")/common.sh"
need_root; need_kernel
tier reset
start_hogs
tier set "$USER_A" gold
tier set "$USER_B" bronze
echo "== gold vs bronze =="; cpu_snapshot
echo "TIME of the bronze job must be greater than 0 and still growing:"
ps -u "$USER_B" -o pid,ni,pcpu,time,comm | grep -E "PID|yes"
sleep 3
ps -u "$USER_B" -o pid,ni,pcpu,time,comm | grep -E "PID|yes"
stop_hogs
