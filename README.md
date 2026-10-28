# CS F372 Operating Systems - Assignment 2

## 1. Group details

| # | Name | ID number |
|---|------|-----------|
| 1 | Krishna | 2024A7PS0175H |
| 2 | Sudhanva | 2024A7PS0085H |

## 2. Environment

- Ubuntu 24.04 VM on Oracle VirtualBox, 4 cores, 6 GB RAM or more (4 GB is enough for P1-P3), 50 GB disk.
- gcc, POSIX C. All user-space C is built with `gcc -Wall -Wextra -O2 -pthread` (zero warnings).
- Problem 4: Linux kernel 6.8, `-forkguard` local version.

```
README.md
p1/  report_card.c  input.txt  Makefile
p2/  workload_cpu.h workload_io.h prog_a_cpu.c prog_b_io.c run_experiments.sh Makefile
p3/  sudoku_analyzer.c  Makefile  grids/
p4/  kernel/fork_guard.c  include/linux/fork_guard.h  hooks/  tools/tier.c
      build_kernel.sh  tests/
```

## 3. Problems

### Problem 1 - Report card generator (`p1/`)

Build / run:

```
cd p1 && make && ./report_card          # reads input.txt (or ./report_card other.txt)
```

Sample (sort key -1, k = 3), matches the assignment sample:

```
Enter sort key (-3=max, -2=min, -1=avg, 0..N-1=subject index): -1
Enter number of sorting threads (k): 3
--- Report Card Menu ---
Enter command (1 <id> = view student, 2 = view all, 0 = exit): 1 2
Student ID : 2
Average    : 58.75
...
Enter command (...): 2
s3 90.00
s1 85.00
s5 79.00
s0 66.75
s4 66.75
s2 58.75
```

Invalid sort key, k not dividing the student count, unknown ids and bad commands are reported and re-prompted.
(The sample PDF prints `s3 90.00` with one space; that is what is implemented.)

### Problem 2 - Round-Robin vs FIFO (`p2/`)

Files: `workload_cpu.h`, `workload_io.h` (exactly as in header.pdf), `prog_a_cpu.c`, `prog_b_io.c`,
`run_experiments.sh`.

```
cd p2 && make
chmod +x run_experiments.sh
./run_experiments.sh             # needs sudo; writes results.txt
# single run by hand:
sudo taskset -c 0 chrt -r 20 ./prog_a_cpu "Time Quantum: 50ms"
sudo timeout 30 taskset -c 0 chrt -f 20 ./prog_b_io "Scheduling Policy: SCHED_FIFO"
```

The script reads the original `sched_rr_timeslice_ms` first, runs both programs for quanta 1, 10, 50, 150, 300, 500 ms,
then one FIFO run each (`TIMED OUT` is recorded if `timeout` fires), and restores the original value on exit.

Output format (per run):

```
Program-A (CPU-Bound)
--------------------------------------------------
Time Quantum: 50ms
Involuntary Context Switches: 42
Voluntary Context Switches: 8
Average Response Time:      10.73ms
Maximum Response Time:      22.42ms
Average Turnaround Time:    1027.44ms
Maximum Turnaround Time:    1041.92ms
Throughput:                 7.68 children/s
Total Elapsed Time:         1.04s
```

(numbers above are only an example of the layout.)

Notes:
- Real RR timeslice granularity depends on the kernel tick (`CONFIG_HZ`): the kernel converts `sched_rr_timeslice_ms`
  to jiffies, so with HZ=250 the smallest effective quantum is 4 ms and 1 ms is rounded up. Very small quanta
  therefore behave similarly.
- Real-time throttling (`/proc/sys/kernel/sched_rt_runtime_us`, default 950000 of 1000000 us) can distort long
  runs: the RT tasks are forced off the CPU for 50 ms per second, which adds time to turnaround/elapsed numbers.
  Set it to -1 to disable it for measurements if desired (and restore it).

### Problem 3 - Sudoku conflict analyzer (`p3/`)

```
cd p3 && make
./sudoku_analyzer grids/grid_valid.txt
./sudoku_analyzer grids/grid_invalid.txt
./sudoku_analyzer grids/grid_blanks.txt
./sudoku_analyzer grids/grid_wrong_count.txt      # 80 cells: rejected with a diagnostic
valgrind --leak-check=full ./sudoku_analyzer grids/grid_invalid.txt
```

Sample (`grid_invalid.txt`, identical to the assignment sample): INVALID, `Validity Score: 88.89%`, heatmap with
`2` at (0,0), `3` at (2,0), `1` at (2,3), one hotspot `(2,0) value=1`, and Row 2 / Column 0 / Sub-grid 0 each
`missing {7}  blanks=0`.

### Problem 4 - Fork storm containment + tiered scheduling (`p4/`)

Files:

| File | Purpose |
|------|---------|
| `kernel/fork_guard.c`, `include/linux/fork_guard.h` | whole implementation (new files) |
| `hooks/apply_hooks.py` | copies the files and patches `kernel/fork.c`, `kernel/sched/core.c`, `kernel/Makefile` |
| `hooks/MANUAL_HOOKS.md` | exact manual edits if the script fails |
| `tools/tier.c` | user-space `tier set / reset / show` |
| `build_kernel.sh` | steps from Q4_Help_Doc.pdf (deps, download, config, build, install) |
| `tests/test1..test8_*.sh` | one script per assignment test case, plus `common.sh` |

Build and install (take a VM snapshot first):

```
cd p4 && chmod +x build_kernel.sh tests/*.sh && ./build_kernel.sh
sudo reboot
uname -r                          # must end in -forkguard
```

Use:

```
sudo tier set alice gold          # user alice (uid 1001) -> gold
sudo tier set bob bronze
tier show
sudo tier reset                   # all users reset to SILVER
cat /proc/fork_guard
cat /proc/sched_tier
sysctl kernel.fg_threshold kernel.fg_window_ms kernel.fg_throttle_ms kernel.fg_cooldown_s kernel.fg_enabled
sudo sysctl -w kernel.fg_threshold=50
dmesg -w | grep fork_guard
```

Defaults: `fg_enabled=1`, `fg_threshold=200`, `fg_window_ms=1000`, `fg_throttle_ms=40`, `fg_cooldown_s=10`.
The tests set a low threshold (default 50) and print a warning; run test files as root with `USER_A` / `USER_B`
set to two ordinary accounts (defaults `alice`, `bob`).

`/proc/fork_guard`:

```
enabled=1 threshold=200 window_ms=1000 throttle_ms=40 cooldown_s=10

UID TIER FORKS STAGE THROTTLE FREEZE REAP
1001 gold 0 0 12 1 1
```

## 4. Viva notes

### P1
- No synchronisation is needed because every location has one writer: worker `i` writes only `student[i]` (reads only
  column `i` of a matrix that is read-only after loading); sort thread `t` touches only its own contiguous segment;
  the parent merges only after `pthread_join()`, which orders all the workers' writes before the parent's reads.
- Thread arguments point into persistent arrays (`worker_ids[]`, `params[]`), never at the loop variable.
- Average is `integer sum / subjects`, so equal sums give identical doubles and the tie-break by lower id is exact.
- Lookups use `student_id`, not array position, since sorting moves entries.

### P2 (leave your measurements in the table)
- CPU-bound: smaller quantum -> more involuntary context switches (every expiry preempts), response time of the
  later children drops (they get the CPU sooner), turnaround of all children goes up and converges at about the
  total work (all finish near the end). Large quantum -> fewer switches, behaves more like FIFO.
- I/O-bound: children block in `fsync()`, so they yield voluntarily; the quantum has little effect and voluntary
  switches dominate over involuntary ones.
- FIFO: no timeslicing, each child runs to completion/block in order; fewest involuntary switches, lowest average
  turnaround for CPU-bound work (shortest-job-first-like with equal jobs), but the worst response time for the last
  child and no fairness. `timeout 30` protects against a non-terminating run.
- Throughput is about the same for all policies for CPU-bound work (same total work on one core); differences are
  switching overhead.

| Program | Policy / quantum | Invol CS | Vol CS | Avg resp (ms) | Max resp (ms) | Avg turn (ms) | Max turn (ms) | Throughput (c/s) | Elapsed (s) |
|---|---|---|---|---|---|---|---|---|---|
| A | RR 1 ms | 1413 | 8 | 11.70 | 25.36 | 5747.35 | 5780.37 | 1.38 | 5.78 |
| A | RR 10 ms | 430 | 8 | 42.66 | 84.74 | 5077.57 | 5110.35 | 1.57 | 5.11 |
| A | RR 50 ms | 99 | 8 | 181.82 | 363.62 | 4599.05 | 4664.33 | 1.72 | 4.66 |
| A | RR 150 ms | 46 | 8 | 556.25 | 1096.18 | 4258.26 | 4686.86 | 1.71 | 4.69 |
| A | RR 300 ms | 18 | 8 | 1048.55 | 2098.17 | 3610.26 | 4533.87 | 1.76 | 4.53 |
| A | RR 500 ms | 28 | 8 | 1761.13 | 3516.58 | 4521.64 | 4926.27 | 1.62 | 4.93 |
| A | FIFO | 12 | 8 | 2060.90 | 4080.86 | 2642.18 | 4652.62 | 1.72 | 4.65 |
| B | RR 1 ms | 22 | 8 | 12.19 | 25.83 | 83.66 | 88.50 | 90.28 | 0.09 |
| B | RR 10 ms | 0 | 8 | 19.19 | 36.65 | 24.09 | 41.17 | 193.38 | 0.04 |
| B | RR 50 ms | 0 | 8 | 19.56 | 35.86 | 24.27 | 40.00 | 198.97 | 0.04 |
| B | RR 150 ms | 0 | 8 | 20.60 | 38.58 | 25.57 | 42.81 | 186.05 | 0.04 |
| B | RR 300 ms | 0 | 8 | 20.86 | 39.31 | 26.04 | 43.82 | 181.15 | 0.04 |
| B | RR 500 ms | 0 | 8 | 20.66 | 38.82 | 25.76 | 43.24 | 184.18 | 0.04 |
| B | FIFO | 0 | 8 | 20.43 | 38.60 | 25.53 | 43.17 | 184.51 | 0.04 |

### P3
- `grid`: written by `main` only while loading, read-only afterwards.
- `validity[27]`: thread `u` writes only index `u`; read by the parent after joining.
- Result structs: allocated by the worker, handed over via `pthread_exit()`, used and freed by the parent after the
  join of that thread.
- `dup_masks[]` (and missing/blank copies): filled by the parent after all joins and before the heatmap threads are
  created, read-only afterwards.
- `heatmap[9][9]`: each thread writes only its own disjoint 3x3 block; the parent reads after joining all nine.
- `valgrind --leak-check=full ./sudoku_analyzer grids/grid_invalid.txt` must report "All heap blocks were freed".
  (Not run on the development machine: valgrind was not available there. Please run it on the VM.)

### P4
- **Ladder**: per-uid state (sliding window, stage, last_over, counters) in a 64-entry table under one spinlock
  (`spin_lock_irqsave`, never held across sleeping calls). The window is a two-bucket weighted estimate
  (`cur + prev * (window - elapsed) / window`), O(1) state. Every fork attempt is counted, refused ones too, so the
  ladder keeps climbing. Over the threshold: stage 1 THROTTLE (`-EAGAIN` + `msleep(fg_throttle_ms)` outside the lock);
  still over after 150 ms -> stage 2 FREEZE (`kill_pgrp(SIGSTOP)`); a delayed work item fires 200 ms later and sends
  SIGKILL (stage 3 REAP) if the user is still at stage 2 and inside the cooldown. Total about 350-400 ms.
- **Why a process group**: a fork bomb is a tree of processes forking faster than you can kill them individually;
  killing one process leaves its siblings and children forking. The bomb launched from an interactive shell lives in
  one job process group that is not the shell's, so the shell survives and the prompt returns. Sanity check: we never
  signal pgid <= 1 or the session leader's group (then we only refuse forks).
- **STOP then KILL**: SIGSTOP halts the whole group at once so it stops spawning and can be inspected. SIGKILL cannot
  be blocked or ignored and the signal code wakes stopped tasks to deliver it, so a stopped group still dies. The
  delayed work is needed because a stopped group no longer calls `fork`, so nothing in `kernel_clone()` would ever
  trigger REAP.
- **Safety independent of tiers**: `fg_check_fork()` runs at the top of `kernel_clone()` and never reads the tier
  table; the tier is only consulted later in `sched_fork()`. A GOLD offender is contained identically.
- **Exemptions**: root (`uid_eq(current_uid(), GLOBAL_ROOT_UID)`) and kernel threads.
- **De-escalation** is lazy: on the next fork attempt of that uid and on every `/proc/fork_guard` read, if
  `now - last_over > fg_cooldown_s` the stage returns to 0 (logged).
- **Tiers and clamp**: GOLD nice -7 (weight 4904), SILVER 0 (1024), BRONZE +5 (335). Contended shares:
  GOLD vs SILVER 82.7 / 17.3 %, SILVER vs BRONZE 75.3 / 24.7 %, GOLD vs BRONZE 93.6 / 6.4 %. BRONZE keeps about 6 %
  of a core against GOLD, so it always makes measurable progress. The bounds are the named constants `FG_NICE_FLOOR`
  (-7) and `FG_NICE_CEIL` (+5), applied to every tier value. A more extreme pair like -20 / +19 (weights 88761 / 15)
  would give BRONZE 0.017 %, which is starvation in practice.
- **At most one GOLD**: setting GOLD on a second user demotes the incumbent to SILVER and re-tiers its tasks (logged).
- **Existing tasks**: `sched_fork()` only sees new tasks, so on `set`/`reset` we walk
  `for_each_process_thread()` under `rcu_read_lock()` and call `set_user_nice()` for every matching non-kernel,
  non-RT task (outside any spinlock). Weight changes immediately, the job is not restarted.
- **Children inherit**: `sched_fork()` looks up the tier by the new task's uid (`p->real_cred->uid`) and overrides
  `static_prio`, `prio`, `normal_prio` and the load weight, so inheritance does not depend on the parent's nice.
  Only users with an explicit assignment are touched; everyone else keeps normal nice inheritance.

## 5. Known limitations / untested parts

- Development was on Windows with WSL. P1, P2 and P3 were compiled and run there with `gcc -Wall -Wextra -O2
  -pthread` with no warnings (P2 was not run under `chrt`/SCHED_RR, only plain execution; valgrind was not run).
- P4 was **not booted**. What was verified: the 6.8 source was fetched, `apply_hooks.py` applied cleanly to it, and
  `kernel/fork.o`, `kernel/fork_guard.o` and `kernel/sched/core.o` compile (x86_64 defconfig) without warnings.
  Runtime behaviour (ladder timing, tier shares, shell recovery, boot) is untested and needs the VM.
- The `-EAGAIN` penalty `msleep` runs in the forking task, so a throttled bomb is also slowed by its own sleeps.
- Thread creation (`clone`) counts as a fork attempt, so a user running a very thread-heavy job can hit the
  threshold; raise `kernel.fg_threshold` for such workloads.
- The per-user table holds 64 uids; when full of tiered/escalated users, new uids are not tracked (fail open).
- Setting a tier overrides the user's own `nice` on their running tasks.
- A fork bomb whose processes share the process group of the session leader (non-interactive shell without job
  control) is only throttled, never frozen/killed, by design (so the terminal is not killed).
