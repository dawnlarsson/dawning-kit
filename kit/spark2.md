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

## What being the kernel unlocks

Everything above stays inside what a module may call. We are not a
module: kernel/patch/apply already claims symbols from lib/string.c,
displaces objects out of an architecture's Makefile and grafts our own
Kconfig into the tree. Linux's source is ours to change, and three of
the costs above only move if we change it.

First, an honest correction to the table at the top. Those 43.6
microseconds end at start_thread. The page faults that populate the new
address space and the teardown that dismantles it happen after, and are
counted in no phase. In the spawn-loop profile they are around six per
cent of everything the guest kernel does, and the scheduler placing,
running and reaping a task that lives for microseconds is another ten.
Both are outside the number, and both are reachable only from inside the
kernel.

**A. The text as one mapping instead of two hundred.** Tested and much
smaller than it looks; read the note below before building it. SPARK_BASE is
0x400000, already on a two megabyte boundary, and the text is 766 KB, so
it sits inside a single PMD's span today. What stops that span being one
mapping is that the writable data segment starts immediately after the
text inside the same two megabytes. Padding the data segment to the next
boundary in the link script gives the text a region of its own, and a
backing store able to hand out a large folio then turns two hundred page
mappings, their faults, their reverse mappings, their teardown and their
TLB entries into one of each. That is a link script line and a decision
about where the image lives, not a kernel patch, and it is the cheapest
large win on this page.

> Measured 2026-09-04, and the result is why A is no longer first.
> Raising Linux's fault-around window to the PMD's worth was tried as the
> cheapest possible version of A: one constant in mm/memory.c, applied
> through kernel/patch. It works exactly as intended -- a spawned shell's
> minor faults fall from 51 to 44 and its resident pages rise from 197 to
> 229, so the handler is mapping more per pass -- and it buys no
> measurable time at all. A spawn is 105.6 microseconds before and after.
>
> The premise was wrong. A guest page fault on a page the cache already
> holds does not leave the guest: hardware nested paging resolves it
> without an exit to the hypervisor, so a minor fault here costs well
> under a microsecond rather than the several a VM exit would. Seven
> fewer faults bought back roughly what thirty-two more mapped pages
> cost, and the change was reverted for adding resident memory and
> teardown work in exchange for nothing.
>
> What that implies for A and B: their win is the page table entries
> themselves -- installing, reverse-mapping and zapping a hundred and
> eighty-five of them -- and this experiment says those are cheap too,
> because thirty-two of them weighed about the same as seven faults.
> Neither is worthless, but neither is the ten to twenty per cent the
> ranking above assumed, and both should be sized by experiment before
> either is built.

**B. One page table for every spark process.** The ceiling version of A.
Every spark program maps the identical text at the identical address, so
the page table entries describing it are identical in every process and
are built and destroyed once per spawn for no reason. A single PMD page,
owned by the module, populated once, and pointed at by every spark
process would make mapping the text a single store and unmapping it a
single reference drop. Linux already does exactly this for hugetlb and
nowhere else. Doing it for a file mapping means teaching the reverse
map that one entry stands for many address spaces, keeping teardown from
freeing a table it does not own, and being certain reclaim never walks
it. This is a real kernel feature with real correctness risk, and it
should not be started until A has shown what the mapping is worth.

**C. Run the child where the parent already is.** The parent asks for a
spawn and then immediately blocks waiting for it. The scheduler does not
know that: it runs the wake-up balancer, picks a destination CPU by
walking the domains, and starts the child cold on another core while the
parent's cache lines sit warm on this one. For a task that lives
microseconds this is entirely loss, and the scheduler is around a tenth
of the spawn loop. Placing a spark child on its parent's CPU and
skipping the domain walk is a small patch to the path Linux would never
take by default, because no ordinary fork can promise the parent is
about to sleep. Ours can, because the module issued both halves.

**D. An empty descriptor table by construction.** The child duplicates
the caller's table and then discards it. Linux offers no clone flag for
"give me a fresh empty table" because no userspace API has ever needed
one; the flags are a public contract and this would be a private one. An
internal field in the clone arguments, honoured in copy_files, is a
handful of lines. Be honest about the size: the measured duplication is
under one per cent today, because the shell holds few descriptors. It
matters as a correctness property first -- a spawned tool should not
inherit descriptors nobody named -- and as a cost only for a caller with
a large table.

**E. A load path that never builds a second stack.** Item 1 above,
written as the kernel change it really is. Today the generic prologue
builds an argument stack because every binary format needs one; ours
does not, because the module can compose the whole thing. A flag on the
binary format handler saying "this one supplies its own stack" lets
bprm_mm_init, copy_strings_kernel and setup_arg_pages be skipped
entirely rather than trimmed, and takes the prologue with them.

**And the compounding.** These do not add up, they multiply, and the
pipeline is where that shows. One ioctl starting N stages composes the
environment block once instead of N times, because every stage of a
pipeline has the same environment. With A or B the text is mapped once
for the whole pipeline rather than once per stage. With C the stages
land on the CPU that already holds the shell's cache. The per-spawn
savings are multiplied by the stage count, and a four-stage pipeline of
tools is the shape a shell actually runs.

## Where the time really is, after all of this

A spawn is 105.6 microseconds end to end in the guest, measured from the
host across two thousand of them. The kernel's construction, which is
everything the phase table at the top counts, is 43.6 of those. The
remaining sixty are the program running and the process being taken
down, and the fault experiment above says the mapping is a small part of
that.

So the next thing to measure is not in this document. It is the sixty
microseconds a spark program spends being itself: its own startup, the
work it was asked to do, and the teardown of an address space with
twelve megabytes of bss described in it. A spawn is already cheaper than
the fork it replaced -- 105.6 against 120.6 for a forked subshell doing
the same nothing -- and the kernel side of it is close enough to the
floor that the next honest win is more likely to be in what the program
does than in how the kernel starts it.

## The one that is not on the list

Replacing begin_new_exec. Item E above removes the argument machinery
around it -- the throwaway stack, the string copy, the shift -- and that
is the whole of the prologue worth having. begin_new_exec itself is
where credentials are installed, other threads are killed, ptrace is
settled and the security hooks run, and a path that skips those is a
privilege bug rather than an optimisation. The line between E and this
is exactly the line between doing less work and doing less checking, and
it is the one line on this page that must not move.

## How each step is judged

`spawn` before and after a loop of three thousand, in the guest, reading
the phases off rather than inferring them from wall clock. A step that
does not move the row it claims to attack has not worked, whatever the
wall clock says. The boot and canvas lanes must stay green, because the
loader is what starts init.
