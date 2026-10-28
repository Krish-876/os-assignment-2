#!/bin/bash
# Runs Program A then Program B for 6 RR quanta and one FIFO run.
# Needs sudo (sched_rr_timeslice_ms and chrt). Appends to results.txt.
# Usage: ./run_experiments.sh

cd "$(dirname "$0")" || exit 1

TS_FILE=/proc/sys/kernel/sched_rr_timeslice_ms
OUT=results.txt
QUANTA="1 10 50 150 300 500"
PRIO=20

for p in prog_a_cpu prog_b_io; do
    if [ ! -x "./$p" ]; then
        echo "./$p not built. Run 'make' first." >&2
        exit 1
    fi
done

# remember the original value before touching anything
ORIG_TS=$(cat "$TS_FILE")
restore() {
    echo "$ORIG_TS" | sudo tee "$TS_FILE" > /dev/null
    echo "Restored sched_rr_timeslice_ms to $ORIG_TS"
}
trap restore EXIT

: > "$OUT"
echo "Original sched_rr_timeslice_ms = $ORIG_TS" | tee -a "$OUT"
echo "" >> "$OUT"

for prog in prog_a_cpu prog_b_io; do
    for q in $QUANTA; do
        echo "$q" | sudo tee "$TS_FILE" > /dev/null
        echo "Running $prog with quantum ${q}ms ..."
        sudo taskset -c 0 chrt -r $PRIO "./$prog" "Time Quantum: ${q}ms" >> "$OUT"
        echo "" >> "$OUT"
    done

    echo "Running $prog under SCHED_FIFO ..."
    tmp=$(mktemp)
    sudo timeout 30 taskset -c 0 chrt -f $PRIO "./$prog" "Scheduling Policy: SCHED_FIFO" > "$tmp"
    rc=$?
    if [ $rc -eq 124 ]; then
        # make sure no child outlives the timeout
        sudo pkill -x "$prog" 2> /dev/null
        {
            [ "$prog" = prog_a_cpu ] && echo -e "\n\nProgram-A (CPU-Bound)" || echo -e "\n\nProgram-B (I/O-Bound)"
            echo "--------------------------------------------------"
            echo "Scheduling Policy: SCHED_FIFO"
            echo "TIMED OUT (killed after 30s)"
        } >> "$OUT"
    else
        cat "$tmp" >> "$OUT"
    fi
    rm -f "$tmp"
    echo "" >> "$OUT"
done

echo "Done. Results in $OUT"
