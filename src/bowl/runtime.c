/*
        Bowl runs another distribution's userspace on the Moonwater kernel.

        There are two entry shapes because package management and ordinary
        commands want different things:

          fast    overlay the distribution's runtime directories in a private
                  mount view, preserving Moonwater files and the current
                  directory; this is the default and has no supervisor fork.

          system  pivot into the complete distribution root with private PID,
                  UTS and IPC views; use this for apt, pacman and services.

        Neither is instruction emulation or a syscall proxy. Once setup is
        complete, the program is an ordinary native process on this kernel.
*/

#define bowl_label TERM_BOLD "[Bowl]" TERM_RESET " "

static const p8 bowl_usage_text[] = bowl_label
    "usage: bowl [--fast|--system] <root> [program [argument...]]\n"
    bowl_label "       bowl expose <root> <program> [name]\n";

#define BOWL_NATIVE_SHELL "/shell"
#define BOWL_ROOT_PREFIX "/bowls/"
#define BOWL_EXPOSE_DIRECTORY "/bowls/bin"
#define BOWL_EXPOSE_PREFIX "#!/bowl @"
#define BOWL_PATH_LIMIT 4096
#define BOWL_SHEBANG_LIMIT 256
#define BOWL_ACCESS_EXECUTE 1

#define MNT_DETACH 2

struct bowl_mount_point
{
        string_address source;
        string_address target;
        string_address filesystem;
        positive flags;
};

struct bowl_layer
{
        string_address path;
        bool required;
};

/* Filesystems expected by a complete distribution root. */
static struct bowl_mount_point bowl_system_mounts[] = {
    {"proc", "/proc", "proc", MS_NOSUID | MS_NOEXEC | MS_NODEV},
    {"sysfs", "/sys", "sysfs", MS_NOSUID | MS_NOEXEC | MS_NODEV},
    {"devtmpfs", "/dev", "devtmpfs", MS_NOSUID},
    {"devpts", "/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC},
    {"tmpfs", "/dev/shm", "tmpfs", MS_NOSUID | MS_NODEV},
    {"tmpfs", "/run", "tmpfs", MS_NOSUID | MS_NODEV},
    {"tmpfs", "/tmp", "tmpfs", MS_NOSUID | MS_NODEV},
    {null, null, null, 0},
};

/*
        The distribution portions of a fast merged view.

        /home, /root, /tmp, /run, /dev, /proc and /sys deliberately remain
        Moonwater's. Relative and absolute paths to user data therefore keep
        their meaning when a package-provided command is exposed system-wide.
        /usr is the only mandatory layer; the others vary across usr-merged
        Debian, Arch and minimal roots.
*/
static struct bowl_layer bowl_fast_layers[] = {
    {"/usr", true},
    {"/lib", false},
    {"/lib64", false},
    {"/bin", false},
    {"/sbin", false},
    {"/etc", false},
    {"/var", false},
    {"/opt", false},
    {null, false},
};

static bipolar bowl_mkdir(string_address path)
{
        bipolar made = system_make_directory_at(AT_FDCWD, path, 0755);

        return made == -EEXIST ? 0 : made;
}

static bool bowl_root_path(p8 address_to into, positive room,
                           string_address root, string_address path)
{
        positive root_length = string_length(root);
        positive path_length = string_length(path);

        if (!room || root_length >= room || path_length >= room - root_length)
                return false;

        memory_copy(into, root, root_length);
        memory_copy(into + root_length, path, path_length + 1);
        return true;
}

static bool bowl_name(string_address name, bool plus)
{
        if (!name || !*name || string_equals(name, ".") ||
            string_equals(name, ".."))
                return false;

        while (*name)
        {
                if (!byte_is_alnum(*name) && *name != '-' && *name != '_' &&
                    *name != '.' && !(plus && *name == '+'))
                        return false;
                name++;
        }

        return true;
}

/* A named root has exactly the form /bowls/NAME. */
static bool bowl_named_root(string_address root)
{
        positive prefix = sizeof(BOWL_ROOT_PREFIX) - 1;
        string_address name;

        if (!root || string_compare_max(root, BOWL_ROOT_PREFIX, prefix) ||
            !root[prefix])
                return false;

        name = root + prefix;
        return !string_equals(name, "bin") && bowl_name(name, false);
}

/*
        Install one kernel-interpreted launcher.

        #!/bowl @/bowls/debian/usr/bin/jq

        Linux passes the encoded target, then the launcher path, then the
        caller's original arguments to /bowl. There is no intermediate shell,
        generated ELF file or per-command runtime. The launcher is the whole
        system-wide installation and is intentionally created O_EXCL.
*/
static b32 bowl_expose(positive count,
                       string_address address_to arguments)
{
        p8 installed[BOWL_PATH_LIMIT];
        p8 launcher[BOWL_PATH_LIMIT];
        p8 inferred[256];
        p8 line[BOWL_SHEBANG_LIMIT];
        string_address root;
        string_address program;
        string_address name;
        positive prefix_length = sizeof(BOWL_EXPOSE_PREFIX) - 1;
        positive root_length;
        positive program_length;
        positive name_length;
        positive line_length;
        bipolar handle;
        bipolar failed;

        if (count < 4 || count > 5)
        {
                log((address_any) bowl_usage_text, sizeof(bowl_usage_text) - 1);
                log_flush();
                return 1;
        }

        root = arguments[2];
        program = arguments[3];
        name = count == 5 ? arguments[4] : inferred;

        if (!bowl_named_root(root) || !program || program[0] != '/' ||
            !program[1])
        {
                string_format(log, bowl_label
                              "expose needs /bowls/NAME and an absolute "
                              "program path\n");
                log_flush();
                return 1;
        }

        for (string_address at = program; *at; at++)
                if (*at <= ' ')
                {
                        string_format(log, bowl_label
                                      "%s: whitespace cannot be encoded in "
                                      "an exposed path\n", program);
                        log_flush();
                        return 1;
                }

        if (count != 5)
                path_tail_copy(inferred, sizeof(inferred), program);

        if (!bowl_name(name, true))
        {
                string_format(log, bowl_label "%s: invalid command name\n", name);
                log_flush();
                return 1;
        }

        root_length = string_length(root);
        program_length = string_length(program);
        name_length = string_length(name);
        line_length = prefix_length + root_length + program_length + 1;

        if (line_length >= sizeof(line) ||
            !bowl_root_path(installed, sizeof(installed), root, program) ||
            sizeof(BOWL_EXPOSE_DIRECTORY) + name_length >= sizeof(launcher))
        {
                string_format(log, bowl_label "exposed path is too long\n");
                log_flush();
                return 1;
        }

        failed = system_access_at(AT_FDCWD, installed, BOWL_ACCESS_EXECUTE);
        if (failed < 0)
        {
                string_format(log, bowl_label "%s: %b\n", installed, failed);
                log_flush();
                return 1;
        }

        failed = bowl_mkdir("/bowls");
        if (!failed)
                failed = bowl_mkdir(BOWL_EXPOSE_DIRECTORY);
        if (failed < 0)
        {
                string_format(log, bowl_label "%s: %b\n", BOWL_EXPOSE_DIRECTORY, failed);
                log_flush();
                return 1;
        }

        path_join(launcher, sizeof(launcher), BOWL_EXPOSE_DIRECTORY, name);
        memory_copy(line, BOWL_EXPOSE_PREFIX, prefix_length);
        memory_copy(line + prefix_length, root, root_length);
        memory_copy(line + prefix_length + root_length, program,
                    program_length);
        line[line_length - 1] = '\n';
        line[line_length] = end;

        handle = system_open_at_mode(
            AT_FDCWD, launcher,
            FILE_WRITE | FILE_EXCLUSIVE | O_CLOEXEC, 0755);

        if (handle < 0)
        {
                string_format(log, bowl_label "%s: %b\n", launcher, handle);
                log_flush();
                return 1;
        }

        if (system_write_all((positive)handle, line, line_length) !=
            line_length)
        {
                system_close(handle);
                system_remove_at(AT_FDCWD, launcher, 0);
                string_format(log, bowl_label "%s: could not write launcher\n",
                              launcher);
                log_flush();
                return 1;
        }

        system_close(handle);
        failed = system_change_mode_at(AT_FDCWD, launcher, 0755);

        if (failed < 0)
        {
                system_remove_at(AT_FDCWD, launcher, 0);
                string_format(log, bowl_label "%s: %b\n", launcher, failed);
                log_flush();
                return 1;
        }

        string_format(log, bowl_label "%s -> %s%s\n", launcher, root,
                      program);
        log_flush();
        return 0;
}

/* Parse @/bowls/NAME/PROGRAM from a shebang invocation. */
static bool bowl_launcher(string_address encoded, p8 address_to root,
                          positive room,
                          string_address address_to program_out)
{
        positive prefix = sizeof(BOWL_ROOT_PREFIX) - 1;
        string_address target;
        string_address program;
        positive root_length;

        if (!encoded || encoded[0] != '@')
                return false;

        target = encoded + 1;
        if (string_compare_max(target, BOWL_ROOT_PREFIX, prefix))
                return false;

        program = string_first_of(target + prefix, '/');
        if (!program || !program[1])
                return false;

        root_length = (positive)(program - target);
        if (root_length >= room)
                return false;

        memory_copy(root, target, root_length);
        root[root_length] = end;

        if (!bowl_named_root(root))
                return false;

        address_to program_out = program;
        return true;
}

/*
        Replace the root for the complete system profile.

        The bind makes the new root a mount point. pivot_root with the same
        path twice stacks the old root there, where it can be detached without
        requiring a writable put_old directory inside the distribution.
*/
static bipolar bowl_system_populate();

static bipolar bowl_system_enter(string_address root)
{
        p8 name[64];
        bipolar failed;

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

        failed = system_change_directory("/");
        if (failed)
                return failed;

        failed = bowl_system_populate();
        if (failed)
                return failed;

        path_tail_copy(name, sizeof(name), root);
        return system_call_2(syscall(sethostname), (positive)name,
                             string_length((string_address)name));
}

static bipolar bowl_system_populate()
{
        for (positive i = 0; bowl_system_mounts[i].target; i++)
        {
                struct bowl_mount_point address_to point =
                    bowl_system_mounts + i;
                bipolar failed;

                failed = bowl_mkdir(point->target);
                if (!failed)
                        failed = system_mount(point->source, point->target,
                                              point->filesystem, point->flags, 0);

                if (failed)
                {
                        string_format(log, bowl_label "%s: %b\n", point->target, failed);
                        log_flush();
                        return failed;
                }
        }

        return 0;
}

/* Overlay only the distribution directories needed to run its programs. */
static bipolar bowl_fast_enter(string_address root)
{
        p8 source[BOWL_PATH_LIMIT];
        bipolar failed;

        for (positive i = 0; bowl_fast_layers[i].path; i++)
        {
                struct bowl_layer address_to layer = bowl_fast_layers + i;

                if (!bowl_root_path(source, sizeof(source), root, layer->path))
                        return -ENAMETOOLONG;

                failed = system_access_at(AT_FDCWD, source, 0);
                if (failed < 0)
                {
                        if (layer->required)
                                return failed;
                        continue;
                }

                failed = bowl_mkdir(layer->path);
                if (failed < 0)
                        return failed;

                failed = system_mount(source, layer->path, 0,
                                      MS_BIND | MS_REC, 0);
                if (failed)
                        return failed;
        }

        return 0;
}

static b32 bowl_launch_failed(bipolar native_shell, string_address what,
                              bipolar failed)
{
        if (native_shell >= 0)
                system_close(native_shell);
        string_format(log, bowl_label "%s: %b\n", what, failed);
        log_flush();
        return 1;
}

static DEAD_END fn bowl_inside(string_address root,
                               string_address program,
                               string_address address_to arguments,
                               string_address address_to environment,
                               bipolar native_shell, bool system_profile)
{
        bipolar failed;

        failed = system_mount(0, "/", 0, MS_REC | MS_PRIVATE, 0);
        if (!failed)
                failed = system_profile ? bowl_system_enter(root)
                                        : bowl_fast_enter(root);

        if (failed)
        {
                string_format(log, bowl_label "%s: %b\n", root, failed);
                log_flush();
                exit(1);
        }

        failed = native_shell >= 0
            ? system_call_5(syscall(execveat), (positive)native_shell,
                             (positive)"", (positive)arguments,
                             (positive)environment, AT_EMPTY_PATH)
            : system_execute(program, arguments, environment);
        string_format(log, bowl_label "%s: %b\n", program, failed);
        log_flush();
        exit(127);
}

static b32 bowl_launch(string_address root, string_address program,
                       string_address address_to arguments,
                       bool system_profile)
{
        string_address native_arguments[] = {BOWL_NATIVE_SHELL, null};
        string_address fallback_environment[] = {"TERM=ansi",
                                                   "PATH=/bin:/usr/bin:/bowls/bin:/",
                                                   "HOME=/root", null};
        string_address address_to environment = file_environment_all();
        bipolar native_shell = -1;
        bipolar failed;
        bipolar child;
        positive ended = 0;
        bool child_ended = false;

        if (!root || root[0] != '/')
        {
                log((address_any) bowl_usage_text, sizeof(bowl_usage_text) - 1);
                log_flush();
                return 1;
        }

        if (!environment)
                environment = fallback_environment;

        if (!program)
        {
                native_shell = system_open_at(AT_FDCWD, BOWL_NATIVE_SHELL,
                                              FILE_READ | O_CLOEXEC);
                if (native_shell < 0)
                        return bowl_launch_failed(native_shell,
                                                  BOWL_NATIVE_SHELL,
                                                  native_shell);

                program = BOWL_NATIVE_SHELL;
                arguments = native_arguments;
        }

        failed = system_call_1(syscall(unshare), CLONE_NEWNS |
            (system_profile ? CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWPID : 0));
        if (failed)
                return bowl_launch_failed(native_shell,
                    system_profile ? "cannot make system views"
                                   : "cannot make a mount view", failed);

        /* Success never returns: this process becomes the command. */
        if (!system_profile)
                bowl_inside(root, program, arguments, environment,
                            native_shell, false);

        /* CLONE_NEWPID places the next child, not this caller, in the view. */
        child = system_fork();
        if (child < 0)
                return bowl_launch_failed(native_shell, "cannot start", child);

        if (child == 0)
                bowl_inside(root, program, arguments, environment,
                            native_shell, true);

        if (native_shell >= 0)
                system_close(native_shell);

        for (;;)
        {
                positive status = 0;
                bipolar reaped = system_wait4_retry(-1, address_of status,
                                                    0, null);
                if (reaped < 0)
                        return child_ended ? wait_status_code(ended) : 1;
                if (reaped == child)
                {
                        ended = status;
                        child_ended = true;
                }
        }
}

static b32 bowl_main()
{
        string_address address_to arguments = program_argument_list();
        positive count = (positive)program_argument_count();
        positive root_at = 1;
        string_address root;
        string_address program = null;
        string_address address_to command_arguments = null;
        bool system_profile = false;
        p8 launcher_root[BOWL_PATH_LIMIT];

        if (!arguments || count < 2)
        {
                log((address_any) bowl_usage_text, sizeof(bowl_usage_text) - 1);
                log_flush();
                return 1;
        }

        if (string_equals(arguments[1], "expose"))
                return bowl_expose(count, arguments);

        if (string_equals(arguments[1], "--system"))
        {
                system_profile = true;
                root_at++;
        }
        else if (string_equals(arguments[1], "--fast"))
                root_at++;
        else if (arguments[1][0] == '-' && arguments[1][1] == '-')
        {
                log((address_any) bowl_usage_text, sizeof(bowl_usage_text) - 1);
                log_flush();
                return 1;
        }

        if (root_at >= count)
        {
                log((address_any) bowl_usage_text, sizeof(bowl_usage_text) - 1);
                log_flush();
                return 1;
        }

        root = arguments[root_at];

        if (root[0] == '@')
        {
                /* argv[2] is the launcher filename inserted by binfmt_script. */
                if (root_at != 1 || count < 3 ||
                    !bowl_launcher(root, launcher_root,
                                   sizeof(launcher_root), address_of program))
                {
                        string_format(log, bowl_label "invalid exposed command\n");
                        log_flush();
                        return 1;
                }

                root = launcher_root;
                arguments[2] = program;
                command_arguments = arguments + 2;
        }
        else if (root_at + 1 < count)
        {
                program = arguments[root_at + 1];
                command_arguments = arguments + root_at + 1;
        }

        return bowl_launch(root, program, command_arguments, system_profile);
}
