#!/bin/bash
# Test 7: retune parameters at runtime and verify containment.
. "$(dirname "$0")/common.sh"
need_root; need_kernel
tier reset

cleanup() {
    echo "Restoring original sysctl parameters..."
    sysctl -w kernel.fg_threshold=200 kernel.fg_window_ms=1000 > /dev/null
}
trap cleanup EXIT INT TERM

echo "Setting test parameters: threshold=50 window_ms=500"
sysctl -w kernel.fg_threshold=50 kernel.fg_window_ms=500

dmesg -C 2> /dev/null
launch_bomb "$USER_A"
dmesg | grep -E "fork_guard: (THROTTLE|FREEZE|REAP)" | head -5

sysctl -w kernel.fg_threshold=200 kernel.fg_window_ms=1000

