# What a shell instance costs, and what can move into the kernel

Measured 2026-09-04 on the build box (Ryzen 9 9950X, Linux host, static
freestanding build of programs/shell.c), against the question: how small
can one instance of the shell be, and which of its startup and resources
could live once, in the kernel module, instead of once per process.

## What an instance costs today

| measurement | ours | dash |
| --- | --- | --- |
| empty script, cycles | 166 k | 1 317 k |
| empty script, instructions | 152 k | 1 500 k |
| empty script, page faults | 14 | 89 |
| empty script, wall | 0.18 ms | 0.55 ms |
| syscalls after execve | 3 (rt_sigaction x2, getpid) | dozens |
| text / data / bss | 784 KB / 26 KB / 12.3 MB | |

The shell's own startup is three system calls; nothing it does before the
first command is worth caching. The 12.3 MB of bss is arenas the tools use
(awk_stack 1.5 MB, sed's three spaces 3 MB, text_list and text_line, the
ls arena and entries, regex_store) and it costs nothing until touched: an
empty script faults fourteen pages in total. A symbol profile of three
hundred empty invocations shows no shell function at all in the top forty;
it is the kernel building and tearing down the process: kernel_init_pages
(zeroing the pages it does touch), __zap_vma_range, TLB flushes, page-table
copy in the parent, the memcg accounting around each. The instance's cost
IS the process.

A two-stage pipeline of builtins (`echo x | wc -l`) costs two clones at
0.35 ms each plus a wait per stage: about 1.5 ms for work whose own
instructions are microseconds. On the Moonwater kernel the shell spawns
external commands and tools through the spark device (SPAWN_TOOL and
SPAWN_SHELL in src/core.c: user_mode_thread plus kernel_execve, no
page-table copy), but a pipeline stage that runs a builtin or a tool in
place still forks, and on a stock kernel everything forks.

## What can safely move into the kernel

Ranked by what it saves and what it risks.

1. **A warm pool of parked shell instances, owned by the module** (the
   "global kernel level shell instance"). At boot the module starts N
   shells through the existing spawn path and parks each in an ioctl
   wait. A SPAWN_TOOL / SPAWN_SHELL request then hands argv, environment
   and the fds to a parked instance (the kernel installs the fds into that
   task's table and writes the request into a shared page) and wakes it;
   the instance runs the tool or script, resets, and parks again. Cost per
   spawn: one wakeup and a context switch, tens of microseconds, against
   the 180 microseconds of execve, page zeroing and teardown today, and
   with no page-table copy ever. Every instance stays an ordinary
   userspace process with its own address space; the kernel owns only the
   queue. What makes it safe is the reset list, and it must be explicit:
   variables and functions, aliases, the parse and expansion arenas, cwd,
   umask, rlimits, signal dispositions, every fd above 2, traps, options,
   the hash table, $SECONDS and $RANDOM state. Anything an instance cannot
   prove it reset (a mapping it leaked, a changed personality) retires it
   and the module spawns a fresh one. Pipelines become N handoffs instead
   of N forks, which is the whole of the pipeline row's remaining gap.

2. **Fork-free children for what only execs.** A stage or command whose
   child does nothing but redirect and exec must not copy the parent's page
   tables: clone with CLONE_VM | CLONE_VFORK (the child touches no shell
   memory before execve) or the spark spawn with the redirections passed
   in. This is a shell-side change with no kernel work, and it removes
   copy_page_range, do_wp_page and __zap_vma_range from the profile of
   every pipeline with an external command. It does not help a stage that
   runs a builtin in place; that is what the pool is for.

3. **Shared, read-only data.** Text and rodata (784 KB) are already shared
   through the page cache; the tool table, keyword table and builtin table
   are rodata. Nothing to do.

4. **Pre-initialised data snapshots.** Not worth it: initialisation is
   three syscalls and no computed tables. A template mm cloned per spawn
   would reintroduce the page-table copy that item 2 removes.

## What must not move

- **The interpreter itself.** A shell running scripts in ring 0 makes every
  parser or expansion bug a kernel compromise; scripts, arguments and
  environment are attacker input (see the memory note on ring 0 being the
  include graph, and printk content being attacker input). The pool keeps
  the interpreter in userspace and moves only the dispatch.
- **Tools sharing one address space concurrently.** The tools keep static
  state (the arenas above); two stages of one pipeline running as threads
  in one process would race on it. Running stages in place sequentially
  with buffered pipes changes semantics for streaming producers
  (`yes | head`) and is not bash's behaviour. Handoffs to pooled instances
  keep one tool per address space.

## What was already taken off the per-instance path

Two guards landed on 2026-09-04 off a symbol profile of kit/bench_shell's
workloads, both per-command costs a script paid for a feature it was not
using. The alias pre-scan walked the assignment prefixes and redirects of
every command to ask a table with nothing in it; it returns at once now
when no alias is defined. And the background reap ran a wait4 at the top of
every complete command to be told a script that never forked has no
children; it runs now only when a background job has been started. The
parse workload went from 1.54 ms to 0.87 ms, which moved it from 14% ahead
of dash to 55%.

## Order of work

Item 2 is a day's work in src/sh/exec.c and shell.c and needs the
job-control changes merged first, since both touch the stage spawn. Item 1
is the module change: a request ring per pool entry, fd installation
through receive_fd, a park ioctl, the reset list in the shell, and a
retire path; measure it with kit/bench_shell's pipeline row and the
kernel's stat_exec_ns.
