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

## Where a spawn's time actually goes

The module has counted the phases of its own spawn since the spark loader
was written and nothing read them until the `spawn` applet was added.
Measured in the guest over three thousand spawns of the image itself
(`while [ $i -lt 3000 ]; do /shell -c :; done`, two vCPUs, nokaslr):

| phase | ns each | share |
| --- | --- | --- |
| task, user_mode_thread | 20 825 | 47.8% |
| exec prologue | 13 336 | 30.6% |
| loader, less its mapping | 5 903 | 13.5% |
| mapping, three do_mmap | 3 537 | 8.1% |
| **kernel side, total** | **43 602** | |

Read that top down. Half the cost is creating the task, before a single
byte of the new program is looked at. Another third is the generic
prologue inside kernel_execve: allocating a bprm, walking the path to the
image and opening it, building the stack that holds argv and then moving
it. Our own loader, the part this repository wrote, is a fifth of the
total, and the three mappings it exists to perform are a twelfth.

A kernel profile of the same loop agrees from the other side, and is
worth reading for what is absent: copy_process, dup_fd, the fault
handler, page clearing, the mapping and the teardown all appear, and
copy_page_range does not appear at all. The spark path already never
copies a page table. Nothing in the profile rises above three per cent,
which is what a cost spread across the scheduler, the memory manager,
the descriptor table and the allocator looks like: there is no hot
function to fix, only a process to stop building so much of.

Three thousand forks through the same shell moved the spawn counters by
three, which is the other half of the picture: a subshell, a command
substitution and every stage of a pipeline fork, and the device never
sees them. Only a plain external command spawns.

## What can be made cheaper, and what it is worth

Ranked by the measured share it attacks. Pooling parked instances is
deliberately not on this list: it does not make a spawn cheap, it avoids
performing one, and every program that is not spawned from the pool keeps
paying full price.

1. **The pipeline forks (a new ioctl).** A pipeline whose stages are all
   external commands is N forks and N execs today, and each fork copies
   the shell's page tables so the child can throw them away microseconds
   later at execve. One ioctl that takes N commands, makes the N-1 pipes
   in the kernel, and starts N tasks with their descriptors already
   installed removes every one of those forks and the shell's own pipe
   bookkeeping with them. A stage that is a builtin, a function or a
   compound command still has to fork, because the child continues
   interpreting the parent's state; the shell decides per stage and the
   common `a | b | c` of external tools takes the fork-free path.

2. **The path walk per spawn (prologue, 30.6%).** Every spawn resolves
   the image's path and opens it again. The image is one file the module
   already knows; holding the opened file and handing it to the bprm
   directly removes a full path resolution, the permission checks and the
   security hooks from every spawn. This is the largest single cut inside
   the prologue and it changes no semantics, because the file is the same
   file the walk would have found.

3. **The argument stack's move (loader rest, 13.5%).** setup_arg_pages
   builds the argument stack at the top of the address space and then
   moves it down to the real stack address, which is a page-table move per
   spawn. It is zero work when the two addresses agree, and they agree
   when the stack is not randomized. That is a security property traded
   for a measured cost, so it belongs behind the profile's own switch
   rather than being taken silently.

4. **What copy_process copies (task, 47.8%).** The biggest phase and the
   hardest. The child is created from the shell making the ioctl, so it
   inherits a duplicate of that shell's descriptor table, its filesystem
   context and its signal handlers, and then execve throws nearly all of
   it away. Everything the shell holds open costs every spawn it makes;
   the cheap half of this is keeping the shell's own table small and
   close-on-exec, and the expensive half is a spawn path that builds the
   child's table from the request instead of copying the caller's.

5. **One mapping instead of two hundred.** The image's text is 784 KB,
   mapped a page at a time, faulted in a page at a time, reverse-mapped
   and then torn down the same way. Every spark program maps the identical
   file at the identical address, so a single large folio behind it would
   turn that into one mapping, one fault and one teardown. The kernel is
   built with transparent huge pages on madvise only and without the
   read-only file path, and the image lives in an initramfs, so nothing
   can give it one today; a tmpfs mounted for huge pages with the image on
   it is the shape that could.

## What must not move

- **The interpreter itself.** A shell running scripts in ring 0 makes every
  parser or expansion bug a kernel compromise; scripts, arguments and
  environment are attacker input (see the memory note on ring 0 being the
  include graph, and printk content being attacker input). Everything
  above moves the dispatch and leaves the interpreting in userspace.
- **Tools sharing one address space concurrently.** The tools keep static
  state (the arenas above); two stages of one pipeline running as threads
  in one process would race on it. Running stages in place sequentially
  with buffered pipes changes semantics for streaming producers
  (`yes | head`) and is not bash's behaviour. The pipeline ioctl starts a
  process per stage for the same reason: one tool, one address space.

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

The ioctl in item 1 is the one with a shape of its own; the rest are cuts
inside a path that already exists. Items 2 and 3 are small and measurable
today with the `spawn` applet, which prints the phases above; take them
first and watch the prologue and loader rows fall. Item 4 is a design
question about who builds the child's descriptor table and should not be
started before 1 and 2 have moved the numbers they claim. Item 5 needs a
kernel configuration change and a decision about where the image lives,
and it is the only one whose win is proportional to the image rather than
fixed per spawn.

Measure every step the same way: `spawn` before and after a loop of three
thousand, in the guest, with the phases read off rather than inferred from
wall clock.
