#!/bin/bash
# Test 4: tier reset returns the split to roughly 50/50.
. "$(dirname "$0")/common.sh"
need_root; need_kernel
start_hogs
tier set "$USER_A" gold
tier set "$USER_B" bronze
echo "== tiered =="; cpu_snapshot
tier reset
echo "== after reset =="; cpu_snapshot
tier show
stop_hogs
