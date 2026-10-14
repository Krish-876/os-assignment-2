# Shared settings for the test scripts. Source this file.
# Override with environment variables:
#   USER_A, USER_B     two ordinary (non-root) accounts, default alice and bob
#   FG_TEST_THRESHOLD  LOW fork threshold used by the containment tests
USER_A="${USER_A:-alice}"
USER_B="${USER_B:-bob}"
FG_TEST_THRESHOLD="${FG_TEST_THRESHOLD:-50}"

need_root() {
    if [ "$(id -u)" -ne 0 ]; then
        echo "Run this test as root (sudo)." >&2
        exit 1
    fi
}

need_kernel() {
    if [ ! -e /proc/fork_guard ]; then
        echo "/proc/fork_guard missing: not running the -forkguard kernel." >&2
        exit 1
    fi
}

# one CPU hog per user, both pinned to CPU 0
start_hogs() {
    sudo -u "$USER_A" bash -c 'taskset -c 0 yes > /dev/null' &
    sudo -u "$USER_B" bash -c 'taskset -c 0 yes > /dev/null' &
    sleep 1
}

stop_hogs() {
    pkill -u "$USER_A" -x yes 2> /dev/null
    pkill -u "$USER_B" -x yes 2> /dev/null
    wait 2> /dev/null
}

# CPU share of the hogs, sampled by top over 5 seconds
cpu_snapshot() {
    top -b -n 2 -d 5 | awk '/^ *PID/{c++} c==2' | grep -E "PID| yes" | head -6
}

low_threshold_warning() {
    echo "WARNING: lowering kernel.fg_threshold to $FG_TEST_THRESHOLD for this test"
    echo "(a bug could otherwise wedge the VM). It is restored at the end."
    ORIG_THRESHOLD=$(sysctl -n kernel.fg_threshold)
    sysctl -w kernel.fg_threshold="$FG_TEST_THRESHOLD"
}

restore_threshold() {
    if [ -n "$ORIG_THRESHOLD" ]; then
        sysctl -w kernel.fg_threshold="$ORIG_THRESHOLD"
    fi
}

# Launch the classic fork bomb as a user. -i gives job control, so the bomb
# gets its own process group exactly like in an interactive terminal.
launch_bomb() {
    local who="$1"
    echo "Launching fork bomb as $who (watch 'dmesg -w | grep fork_guard' elsewhere)"
    sudo -u "$who" bash -ic ':(){ :|:& };:' 2> /dev/null
    sleep 5
    echo "Shell is back. Processes left for $who: $(pgrep -u "$who" | wc -l)"
}
