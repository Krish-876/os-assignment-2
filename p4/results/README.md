# Problem 4: Fork Storm Containment & Tiered CPU Scheduling

**Kernel:** Linux 6.8.0-q4lab  
**Testing Environment:** Ubuntu Virtual Machine

## Problem Statement

Modify the Linux kernel to implement a graduated fork-rate containment system and tiered CPU scheduling (GOLD, SILVER, BRONZE), ensuring that scheduling privileges never override fork-containment policies.

---

## Test 1: Baseline

**Test Case (Assignment):** Baseline. Run `taskset -c 0 yes > /dev/null &` as both users and watch `top`. Expect a roughly 50/50 split.

**Objective:** Verify that two CPU-bound processes belonging to different users receive approximately equal CPU time when both have the default SILVER tier.

![Test 1.1](Test-1.1.jpg)

![Test 1.2](Test-1.2.jpg)

**Observation:** Both users had nice value `0` (SILVER) and received approximately 50% CPU time each, confirming fair CPU allocation under equal scheduling weights.

---

## Test 2: Promote

**Test Case (Assignment):** Promote. From the root terminal, with both workloads and `top` still running, run `tier set <user> gold`. The promoted user's share must rise without restarting the workload.

**Objective:** Verify that promoting an existing user to GOLD immediately increases their CPU share without requiring the running process to restart.

![Test 2.1](Test-2.1.jpg)

![Test 2.2](Test-2.2.jpg)

![Test 2.3](Test-2.3.jpg)

**Observation:** Alice's nice value changed from `0` to `-7`, increasing her CPU share to approximately 83%, while Bob received approximately 17%. The change took effect without restarting either workload.

---

## Test 3: Demote

**Test Case (Assignment):** Demote. Set the other user to bronze. The gap widens. Confirm from `top` that the bronze user still accumulates CPU time — it must not be starved to zero.

**Objective:** Verify that assigning BRONZE to a user reduces their CPU share compared to GOLD, while ensuring that the BRONZE process continues making progress.

![Test 3.1](Test-3.1.jpg)

![Test 3.2](Test-3.2.jpg)

**Observation:** Bob's nice value changed to `+5` (BRONZE), while Alice remained GOLD (`-7`). Alice received the majority of CPU time, but Bob continued making measurable progress, confirming that BRONZE was not starved.

---

## Test 4: Reset

**Test Case (Assignment):** Reset. Run `tier reset` and confirm the split returns to roughly 50/50.

**Objective:** Verify that resetting tier assignments restores the default scheduling priorities and approximately equal CPU sharing between users.

![Test 4.1](Test-4.1.jpg)

![Test 4.2](Test-4.2.jpg)

**Observation:** Both users returned to nice value `0`, and CPU utilization returned to approximately 50% each. This confirmed that tier assignments could be reset without restarting processes.

---

## Test 5: Containment, Untiered

**Test Case (Assignment):** Containment, untiered. Run `sudo forkstat` on the root terminal, then launch `:(){ :|:& };:` as the ordinary user. Watch the ladder run: the bomb must stop by itself and the shell must come back.

**Objective:** Verify that the kernel detects excessive process creation by an ordinary user and automatically contains the fork bomb through THROTTLE, FREEZE, and REAP.

![Test 5.1](Test-5.1.jpg)

![Test 5.2](Test-5.2.jpg)

![Test 5.3](Test-5.3.jpg)

**Observation:** The kernel detected excessive fork attempts and escalated through THROTTLE → FREEZE → REAP. Fork attempts were rejected with `EAGAIN`, the offending process group was terminated, and the shell prompt returned automatically.

---

## Test 6: Containment, GOLD

**Test Case (Assignment):** Containment, GOLD. Repeat test 5 with the offending user crowned GOLD first. The outcome must be identical.

**Objective:** Verify that GOLD scheduling privileges do not exempt a user from fork-containment restrictions.

![Test 6.1](Test-6.1.jpg)

![Test 6.2](Test-6.2.jpg)

**Observation:** Despite being assigned GOLD with nice value `-7`, the user triggered the same THROTTLE → FREEZE → REAP sequence. This confirmed that higher CPU scheduling privileges did not bypass fork-containment protection.

---

## Test 7: Retune at Runtime

**Test Case (Assignment):** Retune at runtime. Lower `fg_threshold` via `sysctl`, repeat test 5, and show that containment triggers sooner — with no kernel rebuild.

**Objective:** Verify that fork-containment parameters can be modified dynamically and that the updated configuration takes effect without recompiling or rebooting the kernel.

![Test 7.1](Test-7.1.jpg)

![Test 7.2](Test-7.2.jpg)

**Observation:** The fork threshold was reduced from `200` to `50`, and the monitoring window was changed from `1000 ms` to `500 ms`. Kernel logs confirmed that containment triggered using the updated configuration. The original values were subsequently restored without a kernel rebuild.

---

## Test 8: Observability

**Test Case (Assignment):** Observability. `cat` your `/proc` stats file and show the escalation counters and tier assignments. Then check `dmesg` for the fork-containment and tier log lines.

**Objective:** Verify that the kernel exposes fork-containment statistics, scheduling tier assignments, runtime configuration, and event logs through the required interfaces.

![Test 8](Test-8.jpg)

**Observation:** `/proc/fork_guard` displayed the active configuration and cumulative THROTTLE, FREEZE, and REAP counters. `/proc/sched_tier` showed no active assignments after reset, and `dmesg` recorded the containment and tier transitions. The VM was running the custom kernel `6.8.0-q4lab`.

---

## Conclusion

All eight tests demonstrated the expected kernel behavior. Tiered scheduling successfully differentiated CPU shares while preserving progress for BRONZE users. Fork containment detected and terminated excessive process creation for both ordinary and GOLD users. Runtime configuration and kernel observability interfaces also functioned as intended.