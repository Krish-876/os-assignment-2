#!/bin/bash
# Test 1: two CPU hogs on CPU 0, one per user. Expect roughly 50/50.
. "$(dirname "$0")/common.sh"
need_root; need_kernel
tier reset
start_hogs
echo "== Baseline (both SILVER) =="
cpu_snapshot
stop_hogs
