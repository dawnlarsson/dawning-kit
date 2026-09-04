# Spark 2.0: the bring-up stack

What a spark program costs between the shell asking for it and its first
instruction, and what the next version of the contract has to change to
make that cheaper. Written 2026-09-04 against measurements taken with the
`spawn` applet and a guest kernel profile; every number below is from
three thousand spawns in the guest on two vCPUs.

## Where the time is, and what assembly can reach

| phase | ns each | share | whose code |
| --- | --- | --- | --- |
| task, user_mode_thread | 20 825 | 47.8% | Linux |
| exec prologue | 13 336 | 30.6% | Linux |
| loader, less its mapping | 5 903 | 13.5% | Linux, called by us |
| mapping, three do_mmap | 3 537 | 8.1% | Linux, called by us |
| **kernel side** | **43 602** | | |
| userspace entry | ~10 instructions | | ours, already assembly |

Say the uncomfortable part first. There is no hot function here to
rewrite: a kernel profile of a spawn loop has nothing above three per
cent, because the cost is a process being constructed across the
scheduler, the memory manager, the descriptor table and the allocator.
Hand-written assembly cannot make copy_process cheaper. What assembly
can do is carry the data, and what a plan can do is make the kernel
construct less.

The userspace half is already finished and worth stating so nobody
reopens it. `_start` compares r12 against the start magic, takes the pid
out of r14 and the already-normalised CPU feature bytes out of r13 with
two stores and a shift, and falls into main; the CPUID and XGETBV
fallback for an ordinary ELF sits behind the non-returning exit path so
a spark program never pays a branch for it. That is the floor. The
three system calls a spark binary appears to make at startup on a stock
kernel -- two rt_sigaction and a getpid -- are the fallback path, and
under Moonwater the getpid is already gone because the loader handed the
answer over in a register.

So Spark 2.0 is not a rewrite in assembly. It is a contract that hands
the program more of what the kernel already knew, and a load path that
stops building the same thing twice.

## The waste, named

**The arguments are walked three times.** The module copies argv and
envp out of the caller into kernel memory. kernel_execve's
copy_strings_kernel then copies those strings into the new stack a page
at a time. Then `spark_stack` walks that stack again with strnlen_user
per string, to recover the lengths the module had at the first step and
threw away, and writes each pointer with put_user. Three passes and two
user-access windows per string, to produce a layout the module could
have written once.

**The image's path is resolved on every spawn.** Every spark program is
the same file. Each spawn walks that path, opens it, and runs the
permission and security hooks again for an answer that cannot change.

**The argument stack is built at the wrong address and moved.**
setup_arg_pages assembles it at the top of the address space and then
shifts it down to the real stack, which is a page-table move per spawn.

**The child inherits a table it does not want.** The task is created
from the shell making the ioctl, so it duplicates that shell's whole
descriptor table, filesystem context and signal handlers, and execve
discards nearly all of it. Everything the shell holds open is a cost on
every program it starts.

**The text is mapped a page at a time.** 784 KB of identical text at an
identical address for every spark program, mapped, faulted,
reverse-mapped and torn down four kilobytes at a time.

## What version two changes

Ranked by the share each attacks, not by how interesting it is.

1. **One stack image, one copy.** The module composes the complete
   initial stack in kernel memory -- argc, the argv and envp pointer
   arrays, the strings, and the handoff words below -- in the exact
   layout the program will see, at the address it will see it, and
   copies it into the new mm with a single bulk copy. That copy is
   `memcpy`, which this repository already claims for the kernel, so the
   one part of bring-up that moves bytes is our floored assembly and
   nothing else in the path touches a byte twice. It removes
   copy_strings_kernel, the strnlen_user and put_user loop in
   spark_stack, and the reason setup_arg_pages has anything to shift.

2. **Hold the image open.** The module keeps the opened file for the
   spark image and hands it to the bprm, so a spawn never resolves a
   path. No semantic changes: it is the same file the walk would find.

3. **A pipeline in one call.** One ioctl takes N commands, creates the
   N-1 pipes in the kernel, and starts N tasks with their descriptors
   already installed. A pipeline of external tools becomes N spawns and
   zero forks, and every fork removed is a page-table copy the child
   would have thrown away microseconds later. A stage that is a builtin,
   a function or a compound command still forks, because that child
   continues interpreting its parent's state; the shell chooses per
   stage.

4. **A descriptor table built from the request.** The child starts with
   an empty table and receives exactly the descriptors the request
   names, rather than duplicating the caller's. This is the largest
   phase and the hardest change, and it should not be started until 1
   and 2 have moved the numbers they claim.

5. **A wider handoff.** The kernel already gives the magic, the CPU
   features and the pid. It also already knows, for a tool spawn, which
   tool was asked for, and the program rediscovers that by searching a
   hundred-entry name table at startup. Hand over the resolved index.
   The rule for what belongs here: anything the module knew before the
   program existed and the program would otherwise recompute or ask for.

6. **One mapping for the text.** Every spark program maps the same
   784 KB at the same address. A single large folio behind it collapses
   two hundred page mappings, their faults, their reverse mappings and
   their teardown into one of each. It needs transparent huge pages
   beyond madvise and the image on a filesystem that can hold such a
   folio, so it is a configuration decision as much as a code one, and
   it is the only item whose win grows with the image rather than being
   fixed per spawn.

## The one that is not on the list

Replacing kernel_execve outright -- a spark_exec that installs a fresh
mm, maps the three regions, drops the prebuilt stack in and sets the
registers -- would take the whole 30.6% prologue rather than trimming
it. It is the largest structural win available and it is left off the
plan on purpose: begin_new_exec is where credentials are installed,
threads are killed, ptrace is settled and the security hooks run, and a
path that skips those is a privilege bug rather than an optimisation.
Item 1 gets most of the same bytes without touching any of that. If the
prologue is still the largest phase after 1 and 2, revisit this with the
hooks kept and only the argument machinery replaced.

## How each step is judged

`spawn` before and after a loop of three thousand, in the guest, reading
the phases off rather than inferring them from wall clock. A step that
does not move the row it claims to attack has not worked, whatever the
wall clock says. The boot and canvas lanes must stay green, because the
loader is what starts init.
