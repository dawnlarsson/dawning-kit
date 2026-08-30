/*
        The commands that are the system.

        init is what the kernel execs, and world is another distribution's root
        running on this kernel. Neither is a shell thing in any deep sense --
        they are here because they are commands, and the commands are all in
        one binary now.
*/

// init -----------------------------------------------------------
#define init_label TERM_BOLD "[Init]" TERM_RESET " "
#define init_program "/shell"

// A shell that dies immediately would otherwise be restarted as fast as the
// machine can fork, forever. Backing off turns that into something a person
// can read and interrupt rather than a spin.
#define RESTART_BACKOFF_NS 250000000
#define RESTART_BACKOFF_MAX_NS 4000000000
#define RESTART_BACKOFF_AFTER 3

// A shell that stayed up this long was doing its job, so whatever came before
// it is not a crash loop and the backoff starts over.
#define SHELL_SETTLED_NS 2000000000

#define CLOCK_MONOTONIC 1

// Raw kernel return values: there is no errno here, a failed call comes back
// as the negated error itself.
#define ERROR_INTERRUPTED (-4)
#define ERROR_NO_CHILDREN (-10)

// PID 1 has two jobs: start the first program, and reap every orphan the
// system ever produces. It must not exec into the shell -- doing that makes
// the shell PID 1, so the kernel panics the moment the shell exits, and
// nothing is left to reap anything.

string_address init_argv[] = {init_program, null};
string_address init_envp[] = {null};

/*
        The network, brought up by the system rather than by whoever logs in.

        ip watch configures whatever is plugged in and then waits on a netlink
        socket for a link to gain or lose carrier, so a machine that boots with
        a cable in is on the network before the prompt appears, and one whose
        cable is moved follows it without anybody typing anything.

        It is started here rather than from the shell's profile because it is
        not a shell thing: a machine with no interactive session should still
        be reachable, and a shell that exits should not take the network with
        it.

        /ip is the shell under another name, like every other utility, so this
        costs no second binary.
*/
#define network_program "/ip"

string_address network_argv[] = {(string_address)network_program,
                                 (string_address) "watch", null};

//      A watcher that dies this quickly is not going to work on the next try
//      either -- a missing /ip, most likely -- so say so once and stop.
#define NETWORK_SETTLED_NS 1000000000
#define NETWORK_GIVE_UP 3

/*
        Enter another name of this already mapped program.

        /init, /shell and /ip are links to one Spark image. The boot init has
        already paid to map that image, so making its first two children exec
        it again builds a new address space, faults the same pages and runs the
        loader only to reach bytes that are resident beside this function.

        A clone gives the child private copy-on-write state and the right
        parent, descriptors, credentials and working directory. Republish the
        child's argv and runtime ownership, then enter the ordinary multicall
        main: /shell takes the shell path and /ip takes the utility path.
        The old two exec calls supplied an empty environment. Linux gives PID
        1 HOME and TERM, so the child ends that inherited vector with one
        store before publishing it through environ. This is deliberately used
        only by the real /init entry below. An `init`
        command run from an interactive shell has live parser, variables and
        traps in its image and must retain the clean exec path.
*/
static DEAD_END fn system_image_reenter(string_address address_to arguments,
                                        b32 count)
{
        string_address address_to environment = program_environment_list();

        /* The initial vector is writable process-stack memory. Ending it in
           place is the allocation-free equivalent of execve(..., { NULL })
           and leaves the parent untouched through clone's copy-on-write. */
        if (environment)
                environment[0] = null;

        program_arguments_use(arguments, count);
        stdlib_program_starting();
        stdlib_exit_flush_hook = stream_flush_at_exit;
        stdlib_exit(moonwater_program_main());

        __builtin_unreachable();
}

static bool system_boot_image()
{
        string_address called = program_argument(0);

        // Linux supplies argv[0] as "init" even when the path it opened was
        // /init. PID 1 is the boundary: executing /init later must not reuse
        // an interactive shell image whose global parser state is live.
        return called &&
               (string_equals(called, "init") ||
                string_equals(called, "/init")) &&
               stdlib_process_identity() == 1;
}

static bipolar start_network(bool reenter)
{
        // A re-entering child would otherwise inherit and later flush bytes
        // which belong to PID 1. exec used to discard that copied buffer.
        if (reenter)
                log_flush();

        bipolar child = system_call_2(syscall(clone), SIGCHLD, 0);

        if (child == 0)
        {
                if (reenter)
                        system_image_reenter(network_argv, 2);

                system_call_3(syscall(execve), (positive)network_program,
                              (positive)network_argv, (positive)init_envp);

                system_call_1(syscall(exit), 127);
        }

        return child;
}

positive now_ns()
{
        timespec now = {0, 0};

        // A clock that will not answer leaves every lifetime measured as zero,
        // which counts every restart as a quick one: the backoff comes on
        // early rather than never.
        if (system_call_2(syscall(clock_gettime), CLOCK_MONOTONIC,
                          (positive)address_of now))
                return 0;

        return (positive)now.tv_sec * 1000000000 + (positive)now.tv_nsec;
}

fn pause_for(positive nanoseconds)
{
        timespec span = {nanoseconds / 1000000000, nanoseconds % 1000000000};

        sleep(address_of span);
}

bipolar start_shell(b32 device, bool reenter)
{
        if (reenter)
        {
                bipolar child;

                log_flush();
                child = system_call_2(syscall(clone), SIGCHLD, 0);

                if (child == 0)
                        system_image_reenter(init_argv, 1);

                if (child > 0)
                        return child;
        }

        if (device >= 0)
        {
                struct spawn request;
                p8 argv_block[64];
                positive length = string_length(init_program) + 1;

                memory_copy(argv_block, init_program, length);

                request.path = (unsigned long)init_program;
                request.argv = (unsigned long)argv_block;
                request.argv_bytes = length;
                request.argv_count = 1;
                request.envp = 0;
                request.envp_bytes = 0;
                request.envp_count = 0;

                bipolar spawned = system_call_3(syscall(ioctl), device,
                                                SPARK_IOCTL_SPAWN,
                                                (positive)address_of request);

                if (spawned > 0)
                        return spawned;
        }

        // No spark device, or it refused: fall back to the portable path.
        // clone takes (flags, child_stack); passing neither leaves both as
        // whatever happened to be in those registers.
        bipolar child = system_call_2(syscall(clone), SIGCHLD, 0);

        if (child == 0)
        {
                system_call_3(syscall(execve), (positive)init_program,
                              (positive)init_argv, (positive)init_envp);

                // Only reached if exec failed.
                system_call_1(syscall(exit), 127);
        }

        return child;
}

// Reading a wait status as an exit code alone reports a crash as a clean exit
// of zero -- which is the one thing this line exists to make visible.
fn report_exit(positive status)
{
        positive signal = status & 0x7f;

        if (signal)
                string_format(log, init_label "%s killed by signal %p, restarting\n",
                              init_program, signal);
        else
                string_format(log, init_label "%s exited (%p), restarting\n",
                              init_program, status >> 8 & 0xff);

        log_flush();
}

/*
        A pty needs somewhere for its other end to appear.

        Not the kernel's job here even though the kernel does the other
        mounts: devpts registers itself with module_init, which for built-in
        code runs at the same initcall level the compositor starts at, and
        kernel/ links before fs/. By the time there is an init there is a
        devpts, and this is where a system mounts it anyway.
*/
fn mount_devpts()
{
        bipolar made = system_call_3(syscall(mkdirat), AT_FDCWD,
                                     (positive)"/dev/pts", 0755);

        if (made < 0 && made != -ERROR_EXISTS)
        {
                string_format(log, init_label "/dev/pts could not be created: %b\n", made);
                log_flush();
        }

        bipolar mounted = system_call_5(syscall(mount), (positive)"devpts",
                                        (positive)"/dev/pts",
                                        (positive)"devpts", 0, 0);

        // Nothing needs a pty this early, so this is not worth refusing to
        // boot over. It is worth saying: without it the terminal fails much
        // later, and for a reason that looks nothing like this one.
        if (mounted < 0)
        {
                string_format(log, init_label "devpts mount failed: %b, terminals will not work\n",
                              mounted);
                log_flush();
        }
}

static b32 system_init()
{
        bool reenter = system_boot_image();

        system_call(syscall(setsid));
        mount_devpts();

        b32 device = system_call_4(syscall(openat), AT_FDCWD,
                                   (positive)SPARK_DEVICE, FILE_READ_WRITE, 0);

        positive quick_exits = 0;
        positive backoff = 0;
        positive started = now_ns();
        bipolar wait_error = 0;

        bipolar network = start_network(reenter);
        positive network_started = now_ns();
        positive network_failures = 0;

        bipolar shell = start_shell(device, reenter);

        // Returning from PID 1 panics the kernel, which on a machine with no
        // serial console says nothing at all. Retrying at a bounded rate keeps
        // the reason on screen instead.
        while (shell < 0)
        {
                string_format(log, init_label "could not start %s: %b\n",
                              init_program, shell);
                log_flush();
                pause_for(RESTART_BACKOFF_MAX_NS);

                started = now_ns();
                shell = start_shell(device, reenter);
        }

        while (1)
        {
                positive status = 0;

                // -1 reaps any child, not just the shell: as PID 1 every
                // orphan on the system is eventually ours to collect.
                bipolar reaped = system_call_4(syscall(wait4), -1,
                                               (positive)address_of status, 0, 0);

                if (reaped == ERROR_INTERRUPTED)
                        continue;

                // Retrying a failing wait as fast as the CPU allows is the one
                // way PID 1 can spin with nothing to show for it. Say it once
                // per run of the same error, then slow down.
                if (reaped < 0 && reaped != ERROR_NO_CHILDREN)
                {
                        if (reaped != wait_error)
                        {
                                wait_error = reaped;
                                string_format(log, init_label "wait failed: %b\n", reaped);
                                log_flush();
                        }

                        pause_for(RESTART_BACKOFF_NS);
                        continue;
                }

                if (reaped > 0)
                {
                        wait_error = 0;

                        if (reaped == network)
                        {
                                if (now_ns() - network_started >= NETWORK_SETTLED_NS)
                                        network_failures = 0;
                                else
                                        network_failures++;

                                if (network_failures >= NETWORK_GIVE_UP)
                                {
                                        string_format(log, init_label
                                                      "%s keeps exiting; leaving the "
                                                      "network alone\n", network_program);
                                        log_flush();
                                        network = -1;
                                        continue;
                                }

                                network_started = now_ns();
                                network = start_network(reenter);
                                continue;
                        }

                        if (reaped != shell)
                                continue;
                }

                // ECHILD means there is nothing left to wait for at all, so
                // the shell is gone whether or not anyone reported it.
                if (reaped == ERROR_NO_CHILDREN)
                {
                        string_format(log, init_label "nothing left to wait for, restarting %s\n",
                                      init_program);
                        log_flush();
                }
                else
                        report_exit(status);

                if (now_ns() - started >= SHELL_SETTLED_NS)
                {
                        quick_exits = 0;
                        backoff = 0;
                }
                else if (++quick_exits > RESTART_BACKOFF_AFTER)
                {
                        backoff = backoff ? backoff * 2 : RESTART_BACKOFF_NS;

                        if (backoff > RESTART_BACKOFF_MAX_NS)
                                backoff = RESTART_BACKOFF_MAX_NS;

                        string_format(log, init_label "%p restarts in a row, waiting %p ms\n",
                                      quick_exits, backoff / 1000000);
                        log_flush();

                        pause_for(backoff);
                }

                started = now_ns();
                shell = start_shell(device, reenter);

                // Retried here rather than by falling back into wait4,
                // which would have no children to wait for and would report
                // the shell as having died when it never started.
                while (shell < 0)
                {
                        string_format(log, init_label "could not start %s: %b\n",
                                      init_program, shell);
                        log_flush();
                        pause_for(RESTART_BACKOFF_MAX_NS);

                        started = now_ns();
                        shell = start_shell(device, reenter);
                }
        }
}

// world -----------------------------------------------------------
/*
        A world: another distribution's root, running on this kernel.

        Moonwater is one kernel and one userspace, and there is a great deal of
        software that only ever ships against Arch's root or Debian's. Rather
        than port it, give it the root it expects. A world is a directory tree
        holding one of those, entered with its own mount table, its own process
        numbers and its own hostname, so what runs inside sees an ordinary
        Linux system and what runs outside is untouched.

                world /worlds/arch              the shell in that root
                world /worlds/debian /bin/ls    one command in it

        This is not a sandbox and does not pretend to be: it is the isolation a
        second distribution needs in order to coexist, not the isolation a
        hostile program needs in order to be contained. The network and the
        user list are shared on purpose -- a world is part of the machine, not
        a guest on it.
*/

#define world_label TERM_BOLD "[World]" TERM_RESET " "

// The four that make a root someone else's. Not the network: a world that
// cannot reach the outside is a different feature, and one nobody has asked
// for yet.
#define CLONE_NEWNS 0x00020000
#define CLONE_NEWUTS 0x04000000
#define CLONE_NEWIPC 0x08000000
#define CLONE_NEWPID 0x20000000

#define MS_NOSUID 2
#define MS_NODEV 4
#define MS_NOEXEC 8
#define MS_BIND 4096
#define MS_REC 16384
#define MS_PRIVATE (1 << 18)

#define MNT_DETACH 2

#define WORLD_SHELL "/bin/sh"


struct mount_point
{
        string_address source;
        string_address target;
        string_address filesystem;
        positive flags;
};

/*
        What every root expects to find already mounted.

        proc is mounted from inside, after the process numbers have been
        replaced, or it shows the outside's -- which is how a world would
        discover it is one. devtmpfs rather than a bind of the machine's /dev,
        so the nodes are the kernel's own and nothing in there can unlink the
        outside's.
*/
static struct mount_point world_mounts[] = {
    {"proc", "/proc", "proc", MS_NOSUID | MS_NOEXEC | MS_NODEV},
    {"sysfs", "/sys", "sysfs", MS_NOSUID | MS_NOEXEC | MS_NODEV},
    {"devtmpfs", "/dev", "devtmpfs", MS_NOSUID},
    {"devpts", "/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC},
    {"tmpfs", "/dev/shm", "tmpfs", MS_NOSUID | MS_NODEV},
    {"tmpfs", "/run", "tmpfs", MS_NOSUID | MS_NODEV},
    {"tmpfs", "/tmp", "tmpfs", MS_NOSUID | MS_NODEV},
    {null, null, null, 0},
};

fn world_say(string_address text)
{
        string_format(log, world_label "%s\n", text);
        log_flush();
}

fn world_fail(string_address text, bipolar code)
{
        string_format(log, world_label "%s: %b\n", text, code);
        log_flush();
}

static bipolar world_mkdir(string_address path)
{
        bipolar made = system_call_3(syscall(mkdirat), AT_FDCWD, (positive)path, 0755);

        return made == -ERROR_EXISTS ? 0 : made;
}

/*
        Replaces the root without needing somewhere to put the old one.

        pivot_root takes the same path twice when the new root is a mount point
        in its own right, which is what the bind below makes it: the old root
        ends up stacked at the same place and is then detached, leaving nothing
        inside holding a reference to what it came from. The older way wants a
        writable directory inside a root that may well be read only.
*/
static bipolar world_enter(string_address root)
{
        bipolar failed;

        failed = system_call_5(syscall(mount), 0, (positive)"/", 0,
                               MS_REC | MS_PRIVATE, 0);
        if (failed)
                return failed;

        failed = system_call_5(syscall(mount), (positive)root, (positive)root, 0,
                               MS_BIND | MS_REC, 0);
        if (failed)
                return failed;

        failed = system_call_1(syscall(chdir), (positive)root);
        if (failed)
                return failed;

        failed = system_call_2(syscall(pivot_root), (positive)".", (positive)".");
        if (failed)
                return failed;

        failed = system_call_2(syscall(umount2), (positive)".", MNT_DETACH);
        if (failed)
                return failed;

        return system_call_1(syscall(chdir), (positive)"/");
}

// A root missing /sys is still a root worth being in, so a mount that will not
// take is reported and stepped over rather than refusing the whole world.
fn world_populate()
{
        for (positive i = 0; world_mounts[i].target; i++)
        {
                struct mount_point address_to point = world_mounts + i;
                bipolar failed;

                if (world_mkdir(point->target) < 0)
                        continue;

                failed = system_call_5(syscall(mount), (positive)point->source,
                                       (positive)point->target,
                                       (positive)point->filesystem, point->flags, 0);

                if (failed)
                        world_fail(point->target, failed);
        }
}

// The name a world answers to is the last element of its path, so
// /worlds/arch is "arch". Only its own namespace ever sees it.
fn world_name(string_address root, p8 address_to into, positive room)
{
        positive length = string_length(root);
        positive start = 0;
        positive count;

        for (positive i = 0; i < length; i++)
                if (string_get(root + i) == '/' && i + 1 < length)
                        start = i + 1;

        count = length - start;

        if (count >= room)
                count = room - 1;

        memory_copy_end(into, root + start, count);
}

static b32 system_world()
{
        string_address root = program_argument(1);
        string_address program = program_argument(2);
        bipolar failed, child;
        positive status = 0, ended = 0;

        if (!root)
        {
                world_say("usage: world <root> [program]");
                return 1;
        }

        if (!program)
                program = WORLD_SHELL;

        string_address world_argv[] = {program, null};
        string_address world_envp[] = {"TERM=ansi",
                                       "PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin",
                                       "HOME=/root", null};

        /*
                Unshared out here, entered in there.

                A new process number namespace does not move the process that
                asks for one -- it decides where that process's next child
                lands. So this one stays outside as something to wait on, and
                the child is what becomes the world's first process.
        */
        failed = system_call_1(syscall(unshare), CLONE_NEWNS | CLONE_NEWUTS |
                                                     CLONE_NEWIPC | CLONE_NEWPID);

        if (failed)
        {
                world_fail("cannot make a namespace", failed);
                return 1;
        }

        child = system_call_2(syscall(clone), SIGCHLD, 0);

        if (child < 0)
        {
                world_fail("cannot start", child);
                return 1;
        }

        if (child == 0)
        {
                p8 name[64];

                failed = world_enter(root);

                if (failed)
                {
                        world_fail(root, failed);
                        exit(1);
                }

                world_populate();
                world_name(root, name, sizeof(name));
                system_call_2(syscall(sethostname), (positive)name,
                              string_length((string_address)name));

                world_fail(program, system_call_3(syscall(execve), (positive)program,
                                                  (positive)world_argv,
                                                  (positive)world_envp));
                exit(127);
        }

        /*
                Everything in there is this process's to collect.

                The child is the world's first process, so orphans inside are
                reparented to it -- but once it goes they are killed and
                reparented out here instead. Waiting until there are no
                children left is what keeps them from becoming the outside's
                zombies, and only the first process's status is the world's.
        */
        for (;;)
        {
                bipolar reaped = system_call_4(syscall(wait4), -1,
                                               (positive)address_of status, 0, 0);

                if (reaped < 0)
                        break;

                if (reaped == child)
                        ended = status;
        }

        return wait_status_code(ended);
}

// edit -------------------------------------------------------------
/*
        The editor, which so far is a listing.

        A sketch, and honest about it -- but a sketch that could not be left
        was worse than a sketch: this used to loop forever with no key read and
        no way out, so the only exit was killing it, and the terminal was left
        in the alternate buffer with the cursor hidden because the restore was
        after a loop that never ended.
*/
static positive2 edit_size;
static positive2 edit_size_before;
static bool edit_dirty = true;

static fn edit_draw()
{
        static string_address listing[] = {(string_address) "ls",
                                           (string_address) ".", null};

        edit_size = term_size();

        if (edit_size.x != edit_size_before.x || edit_size.y != edit_size_before.y)
                edit_dirty = true;

        if (!edit_dirty)
                return;

        edit_dirty = false;
        edit_size_before = edit_size;

        log(str(TERM_CLEAR_SCREEN));

        // The real ls, the same body the shell and /bin/ls run. It reads its
        // words the way a program does, so it is handed some.
        program_arguments_use(listing, 2);
        file_ls();
        program_arguments_own();

        log(str("\n q to quit, any other key to redraw\n"));
}

static b32 system_edit()
{
        bool styles = shell_styles;

        shell_styles = false;
        log(str(TERM_CLEAR_SCREEN TERM_HIDE_CURSOR TERM_ALT_BUFFER TERM_RESET));

        while (1)
        {
                p8 key;
                bipolar got;

                edit_draw();
                log_flush();

                got = system_call_3(syscall(read), standard_input_descriptor, (positive)address_of key, 1);

                if (got <= 0 || key == 'q' || key == 3 || key == 4)
                        break;

                edit_dirty = true;
        }

        log(str(TERM_CLEAR_SCREEN TERM_SHOW_CURSOR TERM_MAIN_BUFFER TERM_RESET));
        log_flush();

        shell_styles = styles;

        return 0;
}
