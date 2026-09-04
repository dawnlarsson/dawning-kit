/*
        A bowl: another distribution's root, running on this kernel.

        Moonwater is one kernel and one userspace, and there is a great deal of
        software that only ever ships against Arch's root or Debian's. Rather
        than port it, give it the root it expects. A bowl is a directory tree
        holding one of those, entered with its own mount table, its own process
        numbers and its own hostname, so what runs inside sees an ordinary
        Linux system and what runs outside is untouched.

                bowl /bowls/arch              the shell in that root
                bowl /bowls/debian /bin/ls    one command in it

        This is not a sandbox and does not pretend to be: it is the isolation a
        second distribution needs in order to coexist, not the isolation a
        hostile program needs in order to be contained. The network and the
        user list are shared on purpose -- a bowl is part of the machine, not
        a guest on it.
*/

#define bowl_label TERM_BOLD "[Bowl]" TERM_RESET " "

#define MNT_DETACH 2

#define BOWL_SHELL "/bin/sh"

struct bowl_mount_point
{
        string_address source;
        string_address target;
        string_address filesystem;
        positive flags;
};

/*
        What every root expects to find already mounted.

        proc is mounted from inside, after the process numbers have been
        replaced, or it shows the outside's -- which is how a bowl would
        discover it is one. devtmpfs rather than a bind of the machine's /dev,
        so the nodes are the kernel's own and nothing in there can unlink the
        outside's.
*/
static struct bowl_mount_point bowl_mounts[] = {
    {"proc", "/proc", "proc", MS_NOSUID | MS_NOEXEC | MS_NODEV},
    {"sysfs", "/sys", "sysfs", MS_NOSUID | MS_NOEXEC | MS_NODEV},
    {"devtmpfs", "/dev", "devtmpfs", MS_NOSUID},
    {"devpts", "/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC},
    {"tmpfs", "/dev/shm", "tmpfs", MS_NOSUID | MS_NODEV},
    {"tmpfs", "/run", "tmpfs", MS_NOSUID | MS_NODEV},
    {"tmpfs", "/tmp", "tmpfs", MS_NOSUID | MS_NODEV},
    {null, null, null, 0},
};

static fn bowl_fail(string_address text, bipolar code)
{
        string_format(log, bowl_label "%s: %b\n", text, code);
        log_flush();
}

static bipolar bowl_mkdir(string_address path)
{
        bipolar made = system_make_directory_at(AT_FDCWD, path, 0755);

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
static bipolar bowl_enter(string_address root)
{
        bipolar failed;

        failed = system_mount(0, "/", 0, MS_REC | MS_PRIVATE, 0);
        if (failed)
                return failed;

        failed = system_mount(root, root, 0, MS_BIND | MS_REC, 0);
        if (failed)
                return failed;

        failed = system_change_directory(root);
        if (failed)
                return failed;

        failed = system_call_2(syscall(pivot_root), (positive)".", (positive)".");
        if (failed)
                return failed;

        failed = system_call_2(syscall(umount2), (positive)".", MNT_DETACH);
        if (failed)
                return failed;

        return system_change_directory("/");
}

// A root missing /sys is still a root worth being in, so a mount that will not
// take is reported and stepped over rather than refusing the whole bowl.
static fn bowl_populate()
{
        for (positive i = 0; bowl_mounts[i].target; i++)
        {
                struct bowl_mount_point address_to point = bowl_mounts + i;
                bipolar failed;

                if (bowl_mkdir(point->target) < 0)
                        continue;

                failed = system_mount(point->source, point->target,
                                      point->filesystem, point->flags, 0);

                if (failed)
                        bowl_fail(point->target, failed);
        }
}

static b32 bowl_main()
{
        string_address root = program_argument(1);
        string_address program = program_argument(2);
        bipolar failed, child;
        positive status = 0, ended = 0;

        if (!root)
        {
                string_format(log, bowl_label "usage: bowl <root> [program]\n");
                log_flush();
                return 1;
        }

        if (!program)
                program = BOWL_SHELL;

        string_address bowl_argv[] = {program, null};
        string_address bowl_envp[] = {"TERM=ansi",
                                      "PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin",
                                      "HOME=/root", null};

        /*
                Unshared out here, entered in there.

                A new process number namespace does not move the process that
                asks for one -- it decides where that process's next child
                lands. So this one stays outside as something to wait on, and
                the child is what becomes the bowl's first process.
        */
        failed = system_call_1(syscall(unshare), CLONE_NEWNS | CLONE_NEWUTS |
                                                     CLONE_NEWIPC | CLONE_NEWPID);

        if (failed)
        {
                bowl_fail("cannot make a namespace", failed);
                return 1;
        }

        child = system_fork();

        if (child < 0)
        {
                bowl_fail("cannot start", child);
                return 1;
        }

        if (child == 0)
        {
                p8 name[64];

                failed = bowl_enter(root);

                if (failed)
                {
                        bowl_fail(root, failed);
                        exit(1);
                }

                bowl_populate();
                // /bowls/arch answers to "arch" inside its own namespace.
                path_tail_copy(name, sizeof(name), root);
                system_call_2(syscall(sethostname), (positive)name,
                              string_length((string_address)name));

                bowl_fail(program,
                           system_execute(program, bowl_argv, bowl_envp));
                exit(127);
        }

        /*
                Everything in there is this process's to collect.

                The child is the bowl's first process, so orphans inside are
                reparented to it -- but once it goes they are killed and
                reparented out here instead. Waiting until there are no
                children left is what keeps them from becoming the outside's
                zombies, and only the first process's status is the bowl's.
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
