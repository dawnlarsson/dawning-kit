#include "../src/library.c"
#include "../src/spark.c"

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

#define label TERM_BOLD "[World]" TERM_RESET " "

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

#define ERROR_EXISTS (-17)

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
        string_format(log, label "%s\n", text);
        log_flush();
}

fn world_fail(string_address text, bipolar code)
{
        string_format(log, label "%s: %b\n", text, code);
        log_flush();
}

static bipolar world_mkdir(string_address path)
{
        bipolar made = system_call_3(syscall(mkdirat), AT_FDCWD, (positive)path, 0755);

        return made == ERROR_EXISTS ? 0 : made;
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

        memory_copy(into, root + start, count);
        into[count] = 0;
}

b32 main()
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

        return (b32)((ended >> 8) & 0xff);
}
