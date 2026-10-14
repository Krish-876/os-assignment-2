#!/bin/bash
# Test 2: promote USER_A to gold while both hogs are already running.
. "$(dirname "$0")/common.sh"
need_root; need_kernel
tier reset
start_hogs
echo "== Before =="; cpu_snapshot
tier set "$USER_A" gold
echo "== After promote (workload not restarted) =="; cpu_snapshot
tier show
stop_hogs
