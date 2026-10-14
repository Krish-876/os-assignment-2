#!/usr/bin/env python3
"""
apply_hooks.py - install fork_guard into a Linux 6.8 source tree.

Usage: python3 apply_hooks.py /path/to/linux-6.8

What it does (idempotent, safe to re-run):
  1. copies kernel/fork_guard.c and include/linux/fork_guard.h into the tree
  2. kernel/Makefile      : appends  obj-y += fork_guard.o
  3. kernel/fork.c        : adds the include and the fg_check_fork() call at the
                            top of kernel_clone() (after its declarations)
  4. kernel/sched/core.c  : adds the include and the tier hook in sched_fork()
                            just before the dl_prio() check

If an anchor cannot be found, the script prints the exact manual edit
(also described in MANUAL_HOOKS.md) and exits with status 1.
"""
import os
import re
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
P4 = os.path.dirname(HERE)

MARK = "fork_guard"

FORK_CALL = (
    "\t/* fork_guard: fork storm containment, must run first and must not\n"
    "\t * depend on the tier logic */\n"
    "\tif (fg_check_fork())\n"
    "\t\treturn -EAGAIN;\n"
    "\n"
)

SCHED_CALL = (
    "\t/*\n"
    "\t * fork_guard: apply the owner's tier as a nice value. Looked up by uid,\n"
    "\t * so children inherit the user's tier, not the parent's nice.\n"
    "\t */\n"
    "\tif (p->policy == SCHED_NORMAL || p->policy == SCHED_BATCH) {\n"
    "\t\tint fg_nice;\n"
    "\n"
    "\t\tif (fg_tier_nice(p->real_cred->uid, &fg_nice)) {\n"
    "\t\t\tp->static_prio = NICE_TO_PRIO(fg_nice);\n"
    "\t\t\tp->prio = p->normal_prio = p->static_prio;\n"
    "\t\t\tset_load_weight(p, false);\n"
    "\t\t}\n"
    "\t}\n"
    "\n"
)

failures = []


def manual(path, text):
    failures.append(path)
    print("\n*** MANUAL EDIT NEEDED in %s ***" % path)
    print(text)


def read(path):
    with open(path, "r", encoding="utf-8") as f:
        return f.read()


def write(path, data):
    with open(path, "w", encoding="utf-8") as f:
        f.write(data)


def add_include(src):
    """insert #include <linux/fork_guard.h> after the first <linux/...> include"""
    if "<linux/fork_guard.h>" in src:
        return src, True
    m = re.search(r"^#include <linux/[^>]+>\n", src, re.M)
    if not m:
        return src, False
    return src[:m.end()] + "#include <linux/fork_guard.h>\n" + src[m.end():], True


def patch_fork(tree):
    path = os.path.join(tree, "kernel/fork.c")
    src = read(path)
    if "fg_check_fork" in src:
        print("fork.c: already patched")
        return
    src, ok = add_include(src)
    m = re.search(r"^pid_t kernel_clone\(struct kernel_clone_args \*args\)\n\{\n", src, re.M)
    if ok and m:
        # skip the declaration block: it ends at the first blank line
        blank = src.find("\n\n", m.end())
        if blank != -1 and blank - m.end() < 600:
            pos = blank + 2
            src = src[:pos] + FORK_CALL + src[pos:]
            write(path, src)
            print("fork.c: patched")
            return
    manual(path,
           "1. Add near the other includes:   #include <linux/fork_guard.h>\n"
           "2. In kernel_clone(), after the local variable declarations\n"
           "   (after 'pid_t nr;') and before the CLONE_PIDFD comment, add:\n\n"
           + FORK_CALL)


def patch_sched(tree):
    path = os.path.join(tree, "kernel/sched/core.c")
    src = read(path)
    if "fg_tier_nice" in src:
        print("core.c: already patched")
        return
    src, ok = add_include(src)
    m = re.search(r"^int sched_fork\(unsigned long clone_flags, struct task_struct \*p\)\n\{",
                  src, re.M)
    if ok and m:
        anchor = "\tif (dl_prio(p->prio))\n\t\treturn -EAGAIN;\n"
        pos = src.find(anchor, m.end())
        if pos != -1 and pos - m.end() < 3000:
            src = src[:pos] + SCHED_CALL + src[pos:]
            write(path, src)
            print("core.c: patched")
            return
    manual(path,
           "1. Add near the other includes:   #include <linux/fork_guard.h>\n"
           "2. In sched_fork(), right before the line 'if (dl_prio(p->prio))'\n"
           "   (after the sched_reset_on_fork block), add:\n\n" + SCHED_CALL)


def patch_makefile(tree):
    path = os.path.join(tree, "kernel/Makefile")
    src = read(path)
    if "fork_guard.o" in src:
        print("kernel/Makefile: already patched")
        return
    if not src.endswith("\n"):
        src += "\n"
    src += "\n# fork_guard (CS F372 Assignment 2, problem 4)\nobj-y += fork_guard.o\n"
    write(path, src)
    print("kernel/Makefile: patched")


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    tree = sys.argv[1]
    for rel in ("kernel/fork.c", "kernel/sched/core.c", "kernel/Makefile"):
        if not os.path.isfile(os.path.join(tree, rel)):
            print("not a kernel tree (missing %s)" % rel)
            return 2

    shutil.copy(os.path.join(P4, "kernel/fork_guard.c"), os.path.join(tree, "kernel/fork_guard.c"))
    shutil.copy(os.path.join(P4, "include/linux/fork_guard.h"),
                os.path.join(tree, "include/linux/fork_guard.h"))
    print("copied fork_guard.c and fork_guard.h")

    patch_makefile(tree)
    patch_fork(tree)
    patch_sched(tree)

    if failures:
        print("\nSome hooks need manual editing (see above): " + ", ".join(failures))
        return 1
    print("\nAll hooks applied.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
