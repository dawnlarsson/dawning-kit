/*
        Bowl runs a distribution package on the Moonwater kernel.

        Bowl is the mounts and the exec, not a userspace. POSIX tools are
        Moonwater applets; a package wins by running against those, not by
        replacing them.

        There are two entry shapes because package management and ordinary
        commands want different things:

          fast        bind only the loader and libc directories the guest
                      binary needs, then exec it by its path under the bowl
                      root; Moonwater /bin stays; no supervisor fork.

          isolated    pivot into the complete distribution root with private
                      PID, UTS and IPC views; use this for apt, pacman, apk,
                      dnf and nix while they fill a tree.

        `bowl setup <name>` is the new-install command: it becomes root,
        installs /bowl, lands that distribution, and puts its manager on
        PATH. Typing pacman afterwards is isolated by the program name, not
        by a flag the person has to remember.

        Neither is instruction emulation or a syscall proxy. Once setup is
        complete, the program is an ordinary native process on this kernel.
*/

#define bowl_label TERM_BOLD "[Bowl]" TERM_RESET " "

static const p8 bowl_usage_text[] = bowl_label
    "usage: bowl setup <name>\n"
    bowl_label "       bowl [--fast|--isolated] <root> [program [argument...]]\n"
    bowl_label "       bowl expose <root> <program> [name]\n";

#define BOWL_NATIVE_SHELL "/shell"
/* Where bowl roots live, said once. Everything else that needs to know --
   including the shell's own path handling -- spells it from here. */
#define BOWL_ROOT_DIRECTORY "/bowls"
#define BOWL_ROOT_PREFIX BOWL_ROOT_DIRECTORY "/"
#define BOWL_EXPOSE_DIRECTORY BOWL_ROOT_PREFIX "bin"
#define BOWL_DEFAULT_PATH "/bin:/usr/bin:" BOWL_EXPOSE_DIRECTORY ":/"
#define BOWL_EXPOSE_PREFIX "#!/bowl @"
#define BOWL_PROGRAM "/bowl"
#define BOWL_PATH_LIMIT 4096
#define BOWL_SHEBANG_LIMIT 256
#define BOWL_ACCESS_EXECUTE 1
#define BOWL_WRAP_ARGV 96

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

/*
        Filesystems expected by a complete isolated root.

        sysfs is read-only: a guest reads what devices there are and changes
        nothing about them, and the kernel's own knobs under /sys/kernel,
        /sys/power and /sys/module are the host's.
*/
static struct bowl_mount_point bowl_isolated_mounts[] = {
    {"proc", "/proc", "proc", MS_NOSUID | MS_NOEXEC | MS_NODEV},
    {"sysfs", "/sys", "sysfs", MS_NOSUID | MS_NOEXEC | MS_NODEV | MS_RDONLY},
    {"devtmpfs", "/dev", "devtmpfs", MS_NOSUID},
    {"devpts", "/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC},
    {"tmpfs", "/dev/shm", "tmpfs", MS_NOSUID | MS_NODEV},
    {"tmpfs", "/run", "tmpfs", MS_NOSUID | MS_NODEV},
    {"tmpfs", "/tmp", "tmpfs", MS_NOSUID | MS_NODEV},
    {null, null, null, 0},
};

/*
        Fast view: libraries, not commands.

        The guest binary is executed at /bowls/NAME/usr/bin/jq. ld.so still
        looks for the interpreter and DT_NEEDED names under /lib and /usr/lib,
        which an empty Moonwater host does not have, so those trees are bound
        read-only from the bowl. /bin, /usr/bin and /opt stay Moonwater's:
        overlaying them hid every native applet.

        /etc and /var stay Moonwater's, as do /home, /root, /tmp, /run, /dev,
        /proc and /sys, except the /etc trees desktop and TLS programs read:
        /etc/xdg for weston.ini, /etc/fonts for fontconfig, /etc/ssl and
        /etc/pki for the guest's own certificate store. Overlaying all of
        /etc hid Moonwater's own names.

        Each launch unshares a mount namespace before these binds, so a
        Debian glibc bowl cannot replace Arch's loader in another process.
        The host /lib is untouched.

        /usr/share is the data tree desktop programs read: Weston, GTK
        schemas, icons, mime. ncurses as root ignores $TERMINFO and reads
        only the directory it was built with, so /usr/share/terminfo is
        still listed for a bowl that has the database but no broader share
        tree. /usr/libexec is weston-desktop-shell and the rest of the
        helpers compiled next to the libraries.
*/
static struct bowl_layer bowl_fast_layers[] = {
    {"/lib", false},
    {"/lib64", false},
    {"/usr/lib", false},
    {"/usr/lib64", false},
    {"/usr/share", false},
    {"/usr/share/terminfo", false},
    {"/usr/libexec", false},
    {"/usr/local/lib", false},
    {"/usr/local/share", false},
    {"/etc/xdg", false},
    {"/etc/fonts", false},
    {"/etc/ssl", false},
    {"/etc/pki", false},
    {null, false},
};

static bipolar bowl_mkdir(string_address path)
{
        bipolar made = system_make_directory_at(AT_FDCWD, path, 0755);

        return made == -EEXIST ? 0 : made;
}

// A mount point below a directory Moonwater itself may not have.
static bipolar bowl_mkdir_parents(string_address path)
{
        p8 prefix[BOWL_PATH_LIMIT];
        positive length = string_length(path);

        if (length >= sizeof(prefix))
                return -ENAMETOOLONG;

        for (positive i = 1; i < length; i++)
        {
                bipolar failed;

                if (path[i] != '/')
                        continue;

                memory_copy(prefix, path, i);
                prefix[i] = 0;
                failed = bowl_mkdir((string_address)prefix);
                if (failed < 0)
                        return failed;
        }

        return bowl_mkdir(path);
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

        /* A shebang file keeps a newline; the kernel does not pass it. */
        for (string_address at = encoded; *at; at++)
                if (*at == '\n' || *at == '\r')
                {
                        *at = end;
                        break;
                }

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

static bool bowl_shebang_target(string_address line, p8 address_to root,
                                positive room,
                                string_address address_to program_out)
{
        if (!line)
                return false;

        while (*line && *line != '@')
                line++;

        return bowl_launcher(line, root, room, program_out);
}

static bool bowl_needs_isolated(string_address program)
{
        static string_address managers[] = {
            "pacman", "pacman-key", "pacman-conf", "makepkg", "repo-add",
            "repo-remove", "apt", "apt-get", "apt-cache", "apt-cdrom",
            "apt-config", "apt-key", "apt-mark", "aptitude", "dpkg",
            "dpkg-deb", "dpkg-query", "dpkg-reconfigure", "dpkg-divert",
            "apk", "dnf", "dnf5", "rpm", "yum", "nix", "nix-env",
            "nix-build", "nix-shell"};
        p8 name[256];

        if (!program || program[0] != '/')
                return false;

        path_tail_copy(name, sizeof(name), program);
        return string_table_find(name, managers, sizeof(managers[0]),
                                 array_count(managers)) < array_count(managers);
}

static b32 bowl_usage(void)
{
        log((address_any)bowl_usage_text, sizeof(bowl_usage_text) - 1);
        log_flush();
        return 1;
}

static b32 bowl_fail(string_address what, bipolar failed)
{
        string_format(log, bowl_label "%s: %b\n", what, failed);
        log_flush();
        return 1;
}

static b32 bowl_refuse(string_address message)
{
        string_format(log, bowl_label "%s", message);
        log_flush();
        return 1;
}

/*
        Bind a tree over a host path and remount it read-only.

        One mount with MS_RDONLY is ignored on bind; the kernel takes the
        readonly bit from a remount of the same target.
*/
static bipolar bowl_bind_ro(string_address source, string_address target)
{
        bipolar failed = system_mount(source, target, 0, MS_BIND | MS_REC, 0);

        if (failed)
                return failed;

        return system_mount(0, target, 0,
                            MS_BIND | MS_REC | MS_REMOUNT | MS_RDONLY, 0);
}

/*
        Install one kernel-interpreted launcher.

        #!/bowl @/bowls/debian/usr/bin/jq

        Linux passes the encoded target, then the launcher path, then the
        caller's original arguments to /bowl. There is no intermediate shell,
        generated ELF file or per-command runtime. The launcher is the whole
        system-wide installation and is intentionally created O_EXCL.
*/
static b32 bowl_expose_program(string_address root, string_address program,
                               string_address name, bool exclusive)
{
        p8 installed[BOWL_PATH_LIMIT];
        p8 launcher[BOWL_PATH_LIMIT];
        p8 inferred[256];
        p8 line[BOWL_SHEBANG_LIMIT];
        positive prefix_length = sizeof(BOWL_EXPOSE_PREFIX) - 1;
        positive root_length;
        positive program_length;
        positive name_length;
        positive line_length;
        bipolar handle;
        bipolar failed;

        if (!bowl_named_root(root) || !program || program[0] != '/' ||
            !program[1])
                return bowl_refuse("expose needs /bowls/NAME and an absolute "
                                   "program path\n");

        for (string_address at = program; *at; at++)
                if (*at <= ' ')
                        return bowl_refuse("whitespace cannot be encoded in "
                                           "an exposed path\n");

        if (!name || !name[0])
        {
                path_tail_copy(inferred, sizeof(inferred), program);
                name = inferred;
        }

        if (!bowl_name(name, true))
                return bowl_refuse("invalid command name\n");

        root_length = string_length(root);
        program_length = string_length(program);
        name_length = string_length(name);
        line_length = prefix_length + root_length + program_length + 1;

        if (line_length >= sizeof(line) ||
            !bowl_root_path(installed, sizeof(installed), root, program) ||
            sizeof(BOWL_EXPOSE_DIRECTORY) + name_length >= sizeof(launcher))
                return bowl_refuse("exposed path is too long\n");

        failed = system_access_at(AT_FDCWD, installed, BOWL_ACCESS_EXECUTE);
        if (failed < 0)
                return bowl_fail(installed, failed);

        failed = bowl_mkdir(BOWL_ROOT_DIRECTORY);
        if (!failed)
                failed = bowl_mkdir(BOWL_EXPOSE_DIRECTORY);
        if (failed < 0)
                return bowl_fail(BOWL_EXPOSE_DIRECTORY, failed);

        path_join(launcher, sizeof(launcher), BOWL_EXPOSE_DIRECTORY, name);

        if (!exclusive && system_access_at(AT_FDCWD, launcher, 0) >= 0)
        {
                p8 existing_root[BOWL_PATH_LIMIT];
                p8 guest[BOWL_PATH_LIMIT];
                string_address existing_program = null;
                bipolar reader = system_open_at(AT_FDCWD, launcher,
                                                FILE_READ | O_CLOEXEC);
                bipolar got = reader < 0
                    ? reader
                    : system_read_retry((positive)reader, line,
                                        sizeof(line) - 1);

                if (reader >= 0)
                        system_close(reader);
                if (got > 0)
                        line[got] = end;
                else
                        line[0] = end;

                if (bowl_shebang_target(line, existing_root,
                                        sizeof(existing_root),
                                        address_of existing_program) &&
                    string_equals(existing_root, root))
                        return 0;

                if (bowl_shebang_target(line, existing_root,
                                        sizeof(existing_root),
                                        address_of existing_program) &&
                    bowl_root_path(guest, sizeof(guest), existing_root,
                                   existing_program) &&
                    system_access_at(AT_FDCWD, guest, BOWL_ACCESS_EXECUTE) >=
                        0)
                {
                        string_format(log,
                                      bowl_label
                                      "%s is already exposed from %s\n",
                                      name, existing_root);
                        log_flush();
                        return 1;
                }

                /* Garbage or a launcher whose guest file is gone: this root
                   may take the name. */
                system_remove_at(AT_FDCWD, launcher, 0);
        }

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
                return bowl_fail(launcher, handle);

        if (system_write_all((positive)handle, line, line_length) !=
            line_length)
        {
                system_close(handle);
                system_remove_at(AT_FDCWD, launcher, 0);
                return bowl_refuse("could not write launcher\n");
        }

        failed = system_call_2(syscall(fchmod), (positive)handle, 0755);
        system_close(handle);

        if (failed < 0)
        {
                system_remove_at(AT_FDCWD, launcher, 0);
                return bowl_fail(launcher, failed);
        }

        string_format(log, bowl_label "%s -> %s%s\n", launcher, root,
                      program);
        log_flush();
        return 0;
}

static b32 bowl_expose(positive count, string_address address_to arguments)
{
        if (count < 4 || count > 5)
                return bowl_usage();

        return bowl_expose_program(arguments[2], arguments[3],
                                   count == 5 ? arguments[4] : null, true);
}

static string_address bowl_guest_bins[] = {
    "/usr/bin", "/usr/sbin", "/bin", null};

static bool bowl_split_guest_path(string_address path, p8 address_to root,
                                  positive root_room, p8 address_to program,
                                  positive program_room)
{
        positive prefix = sizeof(BOWL_ROOT_PREFIX) - 1;
        string_address rest;
        positive root_length;
        positive program_length;

        if (!path || string_compare_max(path, BOWL_ROOT_PREFIX, prefix))
                return false;

        rest = string_first_of(path + prefix, '/');
        if (!rest || !rest[1])
                return false;

        root_length = (positive)(rest - path);
        program_length = string_length(rest);
        if (root_length >= root_room || program_length >= program_room)
                return false;

        memory_copy(root, path, root_length);
        root[root_length] = end;
        if (!bowl_named_root(root))
                return false;

        memory_copy(program, rest, program_length + 1);
        return true;
}

static bool bowl_file_elf(string_address path)
{
        p8 head[4];
        bipolar handle = system_open_at(AT_FDCWD, path, FILE_READ | O_CLOEXEC);
        bipolar got;

        if (handle < 0)
                return false;

        got = system_read_retry((positive)handle, head, 4);
        system_close(handle);
        return got == 4 && head[0] == 0x7f && head[1] == 'E' &&
               head[2] == 'L' && head[3] == 'F';
}

/*
        A bare name the PATH did not hold: look under each bowl's usual
        command directories, expose it, and hand back the launcher so the
        next lookup is an ordinary PATH hit.
*/
static bool bowl_fill_command(string_address name, p8 address_to into,
                             positive room)
{
        file_walk walk;
        struct linux_dirent64 address_to entry;
        p8 root[BOWL_PATH_LIMIT];
        p8 rel[256];
        p8 installed[BOWL_PATH_LIMIT];
        p8 fallback[BOWL_PATH_LIMIT];
        bool found = false;
        bool held = false;
        positive name_length;

        if (!name || string_first_of(name, '/') || !bowl_name(name, true))
                return false;

        name_length = string_length(name);
        if (!file_walk_open(address_of walk, AT_FDCWD, BOWL_ROOT_DIRECTORY))
                return false;

        while (!found && (entry = file_walk_next(address_of walk)))
        {
                if (file_is_dot(entry->d_name) ||
                    string_equals(entry->d_name, "bin"))
                        continue;

                if (!path_join(root, sizeof(root), BOWL_ROOT_DIRECTORY,
                               entry->d_name) ||
                    !bowl_named_root(root))
                        continue;

                for (positive at = 0; bowl_guest_bins[at]; at++)
                {
                        if (string_length(bowl_guest_bins[at]) + 1 +
                                name_length >=
                            sizeof(rel))
                                continue;

                        if (!path_join(rel, sizeof(rel), bowl_guest_bins[at],
                                       name) ||
                            !bowl_root_path(installed, sizeof(installed), root,
                                            rel) ||
                            system_access_at(AT_FDCWD, installed,
                                             BOWL_ACCESS_EXECUTE) < 0)
                                continue;

                        if (!bowl_expose_program(root, rel, name, false) &&
                            path_join(into, room, BOWL_EXPOSE_DIRECTORY,
                                      name))
                        {
                                found = true;
                                break;
                        }

                        /* Another root already owns the name, or the write
                           failed. Keep the guest path so wrap can still run
                           this binary, and keep looking for a root that can
                           own the launcher. */
                        if (!held && string_length(installed) < sizeof(fallback))
                        {
                                memory_copy(fallback, installed,
                                            string_length(installed) + 1);
                                held = true;
                        }
                }
        }

        file_walk_close(address_of walk);
        if (found)
                return true;
        if (!held || string_length(fallback) >= room)
                return false;
        memory_copy(into, fallback, string_length(fallback) + 1);
        return true;
}

static p8 bowl_wrap_root[BOWL_PATH_LIMIT];
static p8 bowl_wrap_program[BOWL_PATH_LIMIT];
static p8 bowl_wrap_absolute[BOWL_PATH_LIMIT];
static string_address bowl_wrap_vector[BOWL_WRAP_ARGV];

static bool bowl_guest_absolute(string_address path, string_address cwd,
                                p8 address_to into, positive room)
{
        if (!path || !path[0])
                return false;

        if (path[0] == '/')
        {
                positive length = string_length(path);

                if (length >= room)
                        return false;

                memory_copy(into, path, length + 1);
                return true;
        }

        if (!cwd || cwd[0] != '/')
                return false;

        return path_join(into, room, cwd, path) != 0;
}

static bool bowl_guest_elf_command(string_address path, string_address cwd)
{
        if (!bowl_guest_absolute(path, cwd, bowl_wrap_absolute,
                                 sizeof(bowl_wrap_absolute)))
                return false;

        return bowl_file_elf(bowl_wrap_absolute) &&
               bowl_split_guest_path(bowl_wrap_absolute, bowl_wrap_root,
                                     sizeof(bowl_wrap_root),
                                     bowl_wrap_program,
                                     sizeof(bowl_wrap_program));
}

/*
        A guest ELF is not a Moonwater program: execve of it looks for
        ld-linux under the host and answers -2. Run it through bowl so the
        fast view can bind the loader, whether the name was typed, hashed, or
        spelled as ./btop from the guest bin directory.
*/
static bool bowl_wrap_command(string_address path, string_address cwd,
                              string_address address_to address_to argv,
                              positive address_to argc)
{
        string_address address_to old;
        positive count;
        positive at;

        if (!argv || !argc || !bowl_guest_elf_command(path, cwd))
                return false;

        old = address_to argv;
        count = address_to argc;
        if (!old || count + 2 >= BOWL_WRAP_ARGV)
                return false;

        bowl_wrap_vector[0] = BOWL_PROGRAM;
        bowl_wrap_vector[1] = bowl_wrap_root;
        bowl_wrap_vector[2] = bowl_wrap_program;
        for (at = 1; at < count; at++)
                bowl_wrap_vector[at + 2] = old[at];
        bowl_wrap_vector[count + 2] = null;
        address_to argv = bowl_wrap_vector;
        address_to argc = count + 2;
        return true;
}

static bool bowl_wrap_words(string_address cwd,
                           string_address address_to words, positive count,
                           positive room)
{
        positive at;

        if (!words || count < 1 || count + 2 >= room ||
            !bowl_guest_elf_command(words[0], cwd))
                return false;

        for (at = count; at >= 1; at--)
                words[at + 2] = words[at];
        words[0] = BOWL_PROGRAM;
        words[1] = bowl_wrap_root;
        words[2] = bowl_wrap_program;
        words[count + 2] = null;
        return true;
}

static bipolar bowl_dev_link(string_address target, string_address name)
{
        bipolar failed = system_symbolic_link_at(target, AT_FDCWD, name);

        return (failed < 0 && failed != -EEXIST) ? failed : 0;
}

/*
        The host kernel's settings, which a fresh proc shows a guest exactly as
        the host sees them.

        A pid namespace gives the guest its own processes and nothing else, so
        /proc/sys is the running kernel's: procps's postinst runs
        sysctl --system inside the bowl and rewrote the host's core_pattern,
        sysrq, fs.protected_* and rp_filter with Debian's defaults. Each is
        bound over itself read-only, which is what a container runtime does
        with the same list; a write answers EROFS and sysctl says it could not
        set the key. /proc/sys is always there; the rest only when the kernel
        was built with what makes them.

        This keeps package scripts off the host. It is not a wall against a
        guest that means to get through: the bowl is root with every
        capability and could remount any of these.
*/
static string_address bowl_kernel_settings[] = {
    "/proc/sys", "/proc/sysrq-trigger", "/proc/irq", "/proc/bus", "/proc/fs",
    null};

static bipolar bowl_kernel_settings_seal(void)
{
        for (positive i = 0; bowl_kernel_settings[i]; i++)
        {
                string_address path = bowl_kernel_settings[i];
                bipolar failed = system_mount(path, path, 0, MS_BIND, 0);

                if (failed == -ENOENT && i)
                        continue;

                if (!failed)
                        failed = system_mount(0, path, 0,
                                              MS_BIND | MS_REMOUNT | MS_RDONLY |
                                                  MS_NOSUID | MS_NOEXEC | MS_NODEV,
                                              0);

                if (failed)
                {
                        bowl_fail(path, failed);
                        return failed;
                }
        }

        return 0;
}

static bipolar bowl_isolated_populate(void)
{
        bipolar failed = 0;

        for (positive i = 0; bowl_isolated_mounts[i].target; i++)
        {
                struct bowl_mount_point address_to point =
                    bowl_isolated_mounts + i;

                failed = bowl_mkdir(point->target);
                if (!failed)
                        failed = system_mount(point->source, point->target,
                                              point->filesystem, point->flags, 0);

                if (failed)
                {
                        bowl_fail(point->target, failed);
                        return failed;
                }
        }

        failed = bowl_kernel_settings_seal();
        if (failed)
                return failed;

        /*
                bash process substitution opens /dev/fd/N. devtmpfs does not
                create those names; pacman-key uses them while filling the
                keyring.
        */
        failed = bowl_dev_link("/proc/self/fd", "/dev/fd");
        if (!failed)
                failed = bowl_dev_link("/proc/self/fd/0", "/dev/stdin");
        if (!failed)
                failed = bowl_dev_link("/proc/self/fd/1", "/dev/stdout");
        if (!failed)
                failed = bowl_dev_link("/proc/self/fd/2", "/dev/stderr");
        if (failed)
                bowl_fail("/dev/fd", failed);

        return failed;
}

/*
        Replace the root for the isolated profile.

        The bind makes the new root a mount point. pivot_root with the same
        path twice stacks the old root there, where it can be detached without
        requiring a writable put_old directory inside the distribution.
*/
static bipolar bowl_isolated_enter(string_address root)
{
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

        failed = bowl_isolated_populate();
        if (failed)
                return failed;

        /* Populate puts a fresh tmpfs on /run, so the host copy installed
           before pivot is gone. Write the compiled database into this /run
           or $TERMINFO points at an empty directory and ncurses refuses to
           open the terminal. */
        terminal_terminfo_install();
        return 0;
}

/* Bind only the loader search paths a guest binary still spells in ELF. */
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

                failed = bowl_mkdir_parents(layer->path);
                if (failed < 0)
                        return failed;

                failed = bowl_bind_ro(source, layer->path);
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
        return bowl_fail(what, failed);
}

static fn bowl_session_prepare_from(string_address address_to environment);
static string_address address_to bowl_environment(
    string_address address_to inherited);

static DEAD_END fn bowl_inside(string_address root,
                               string_address program,
                               string_address address_to arguments,
                               string_address address_to environment,
                               bipolar native_shell, bool isolated)
{
        p8 installed[BOWL_PATH_LIMIT];
        string_address run = program;
        bipolar failed;

        failed = system_mount(0, "/", 0, MS_REC | MS_PRIVATE, 0);
        if (!failed)
                failed = isolated ? bowl_isolated_enter(root)
                                  : bowl_fast_enter(root);

        if (failed)
        {
                bowl_fail(root, failed);
                exit(1);
        }

        /* Isolated /run is a fresh tmpfs; fast keeps the host's. Either way
           the guest now sees the directories the session variables name. */
        bowl_session_prepare_from(environment);

        /* Fast does not overlay /usr, so /usr/bin/jq is still Moonwater's
           missing name. The file is the one under the bowl root. Isolated
           has already pivoted; the guest path is the guest file. */
        if (!isolated && native_shell < 0)
        {
                if (!program || program[0] != '/' ||
                    !bowl_root_path(installed, sizeof(installed), root,
                                    program))
                {
                        bowl_fail(program ? program : root, -ENAMETOOLONG);
                        exit(1);
                }
                run = installed;
        }

        failed = native_shell >= 0
            ? system_call_5(syscall(execveat), (positive)native_shell,
                             (positive)"", (positive)arguments,
                             (positive)environment, AT_EMPTY_PATH)
            : system_execute(run, arguments, environment);
        bowl_fail(program, failed);
        exit(127);
}

static b32 bowl_env_named(string_address entry, string_address name)
{
        positive i = 0;

        if (!entry)
                return false;

        while (name[i] && entry[i] == name[i])
                i++;

        return name[i] == 0 && entry[i] == '=';
}

static string_address bowl_env_payload(string_address entry, string_address name)
{
        return entry + string_length(name) + 1;
}

static string_address bowl_env_value(string_address address_to environment,
                                     string_address name)
{
        if (!environment)
                return null;

        for (; *environment; environment++)
                if (bowl_env_named(*environment, name))
                        return bowl_env_payload(*environment, name);

        return null;
}

/*
        The session a guest program expects.

        Weston, GTK, Qt, PipeWire and almost every other desktop program
        refuse to start without XDG_RUNTIME_DIR, and they want it to be a
        0700 directory owned by this user. Isolated populate puts a fresh
        tmpfs on /run, so the host copy is gone and this runs after the
        guest can see the path. An inherited environment that already names
        usable values is left alone; one that names none — bowl from a
        kernel console, bind init, or a script that cleared the block —
        gets the set, or Weston fails with "XDG_RUNTIME_DIR not set".

        getenv keeps the first assignment. An empty or relative
        XDG_RUNTIME_DIR= still counts as set, so appending a real one would
        leave Weston reading the blank. Those entries are dropped and the
        default is written instead. chmod 0700 is only for the runtime
        directory: /tmp as XDG_RUNTIME_DIR is a common wrong value, and
        making /tmp 0700 takes it away from everyone else.
*/
#define BOWL_RUNTIME_DIR "/run/user/"
#define BOWL_SESSION_HOME "/root"
#define BOWL_ENV_ROOM 512
#define BOWL_ENV_DEFAULTS 10

static p8 bowl_runtime_path[sizeof(BOWL_RUNTIME_DIR) + 20];
static p8 bowl_runtime_assignment[sizeof("XDG_RUNTIME_DIR=") +
                                 sizeof(bowl_runtime_path)];
static p8 bowl_user_assignment[sizeof("USER=") + 20];
static p8 bowl_logname_assignment[sizeof("LOGNAME=") + 20];

static b32 bowl_path_same(string_address path, string_address want)
{
        positive i = 0;

        if (!path || !want)
                return false;

        while (want[i] && path[i] == want[i])
                i++;

        if (want[i])
                return false;

        return !path[i] || (path[i] == '/' && !path[i + 1]);
}

static b32 bowl_runtime_shared(string_address path)
{
        return bowl_path_same(path, "/tmp") || bowl_path_same(path, "/var/tmp") ||
               bowl_path_same(path, "/dev/shm") || bowl_path_same(path, "/run") ||
               bowl_path_same(path, "/dev") || bowl_path_same(path, "/");
}

static b32 bowl_session_unusable(string_address entry)
{
        static string_address empty[] = {
            "TERM", "TERMINFO", "PATH", "LANG", "LC_ALL", "USER", "LOGNAME",
            "SHELL", null};
        static string_address path[] = {"HOME", "XDG_RUNTIME_DIR", "TMPDIR",
                                        null};
        positive i;
        string_address value;

        if (!entry)
                return false;

        for (i = 0; path[i]; i++)
                if (bowl_env_named(entry, path[i]))
                {
                        value = bowl_env_payload(entry, path[i]);
                        return value[0] != '/';
                }

        for (i = 0; empty[i]; i++)
                if (bowl_env_named(entry, empty[i]))
                {
                        value = bowl_env_payload(entry, empty[i]);
                        return !value[0];
                }

        return false;
}

static b32 bowl_session_default_missing(string_address name,
                                        string_address value)
{
        if (!name)
                return true;

        if (string_equals(name, "IFS") || string_equals(name, "OPTIND"))
                return !value;

        if (!value || !value[0])
                return true;

        if (string_equals(name, "HOME") ||
            string_equals(name, "XDG_RUNTIME_DIR") ||
            string_equals(name, "TMPDIR"))
                return value[0] != '/';

        return false;
}

static fn bowl_session_assign(p8 address_to into, positive room,
                              string_address name, string_address value)
{
        positive n = string_length(name);
        positive v = string_length(value);

        if (!room || n + v + 2 > room)
        {
                if (room)
                        into[0] = end;
                return;
        }

        memory_copy(into, name, n);
        into[n] = '=';
        memory_copy(into + n + 1, value, v + 1);
}

static fn bowl_session_fill(void)
{
        /* Weston stats getuid, not geteuid, against the directory owner. */
        positive uid = (positive)system_call(syscall(getuid));
        p8 digits[24];
        string_address user;

        memory_copy(bowl_runtime_path, BOWL_RUNTIME_DIR,
                    sizeof(BOWL_RUNTIME_DIR) - 1);
        bowl_runtime_path[sizeof(BOWL_RUNTIME_DIR) - 1 +
                          positive_into_string(bowl_runtime_path +
                                                   sizeof(BOWL_RUNTIME_DIR) - 1,
                                               uid)] = end;
        bowl_session_assign(bowl_runtime_assignment,
                            sizeof(bowl_runtime_assignment), "XDG_RUNTIME_DIR",
                            bowl_runtime_path);

        if (!uid)
                user = (string_address) "root";
        else
        {
                digits[positive_into_string(digits, uid)] = end;
                user = digits;
        }

        bowl_session_assign(bowl_user_assignment, sizeof(bowl_user_assignment),
                            "USER", user);
        bowl_session_assign(bowl_logname_assignment,
                            sizeof(bowl_logname_assignment), "LOGNAME", user);
}

static string_address bowl_session_runtime_assignment(void)
{
        bowl_session_fill();
        return bowl_runtime_assignment;
}

static string_address bowl_session_user_assignment(void)
{
        bowl_session_fill();
        return bowl_user_assignment;
}

static string_address bowl_session_logname_assignment(void)
{
        bowl_session_fill();
        return bowl_logname_assignment;
}

static fn bowl_chmod_directory(string_address path, positive mode)
{
        bipolar handle = system_open_at(AT_FDCWD, path,
                                        FILE_READ | O_DIRECTORY | O_NOFOLLOW |
                                            O_CLOEXEC);

        if (handle < 0)
                return;

        system_call_2(syscall(fchmod), (positive)handle, mode);
        system_close(handle);
}

static fn bowl_session_home_dirs(string_address home)
{
        static string_address names[] = {
            "/.config", "/.cache", "/.local", "/.local/share", "/.local/state",
            null};
        p8 path[BOWL_PATH_LIMIT];
        positive i;

        if (!home || home[0] != '/')
                return;

        for (i = 0; names[i]; i++)
                if (bowl_root_path(path, sizeof(path), home, names[i]))
                        bowl_mkdir_parents(path);
}

static fn bowl_session_prepare(string_address home, string_address runtime)
{
        bowl_session_fill();

        if (!runtime || runtime[0] != '/')
                runtime = bowl_runtime_path;

        bowl_mkdir_parents(runtime);
        if (!bowl_runtime_shared(runtime))
                bowl_chmod_directory(runtime, 0700);
        bowl_mkdir("/tmp");
        bowl_chmod_directory("/tmp", 01777);
        bowl_mkdir("/dev/shm");
        bowl_mkdir("/run/lock");
        bowl_mkdir("/var");
        bowl_dev_link("/run", "/var/run");
        bowl_dev_link("/run/lock", "/var/lock");
        if (!home || home[0] != '/')
                home = (string_address)BOWL_SESSION_HOME;
        bowl_session_home_dirs(home);
}

static fn bowl_session_prepare_from(string_address address_to environment)
{
        bowl_session_prepare(bowl_env_value(environment, "HOME"),
                             bowl_env_value(environment, "XDG_RUNTIME_DIR"));
}

static string_address address_to bowl_environment(
    string_address address_to inherited)
{
        static string_address mixed[BOWL_ENV_ROOM];
        positive n = 0;
        b32 skipped = false;
        b32 have_term = false;
        b32 have_terminfo = false;
        b32 have_home = false;
        b32 have_path = false;
        b32 have_lang = false;
        b32 have_user = false;
        b32 have_logname = false;
        b32 have_runtime = false;
        b32 have_shell = false;
        b32 have_tmpdir = false;

        bowl_session_fill();

        if (inherited)
        {
                for (positive at = 0; inherited[at]; at++)
                {
                        if (bowl_session_unusable(inherited[at]))
                        {
                                skipped = true;
                                continue;
                        }

                        if (bowl_env_named(inherited[at], "TERM"))
                                have_term = true;
                        else if (bowl_env_named(inherited[at], "TERMINFO"))
                                have_terminfo = true;
                        else if (bowl_env_named(inherited[at], "HOME"))
                                have_home = true;
                        else if (bowl_env_named(inherited[at], "PATH"))
                                have_path = true;
                        else if (bowl_env_named(inherited[at], "LANG") ||
                                 bowl_env_named(inherited[at], "LC_ALL"))
                                have_lang = true;
                        else if (bowl_env_named(inherited[at], "USER"))
                                have_user = true;
                        else if (bowl_env_named(inherited[at], "LOGNAME"))
                                have_logname = true;
                        else if (bowl_env_named(inherited[at],
                                                "XDG_RUNTIME_DIR"))
                                have_runtime = true;
                        else if (bowl_env_named(inherited[at], "SHELL"))
                                have_shell = true;
                        else if (bowl_env_named(inherited[at], "TMPDIR"))
                                have_tmpdir = true;

                        if (n + 1 >= BOWL_ENV_ROOM)
                                return inherited;

                        mixed[n++] = inherited[at];
                }

                if (!skipped && have_term && have_terminfo && have_home &&
                    have_path && have_lang && have_user && have_logname &&
                    have_runtime && have_shell && have_tmpdir)
                        return inherited;

                if (n + BOWL_ENV_DEFAULTS >= BOWL_ENV_ROOM)
                        return inherited;
        }

        if (!have_term)
                mixed[n++] = "TERM=" TERM_NAME;
        if (!have_terminfo)
                mixed[n++] = "TERMINFO=" TERM_INFO_DIRECTORY;
        if (!have_home)
                mixed[n++] = "HOME=" BOWL_SESSION_HOME;
        if (!have_path)
                mixed[n++] = "PATH=" BOWL_DEFAULT_PATH;
        if (!have_lang)
                mixed[n++] = "LANG=C.UTF-8";
        if (!have_user)
                mixed[n++] = bowl_user_assignment;
        if (!have_logname)
                mixed[n++] = bowl_logname_assignment;
        if (!have_runtime)
                mixed[n++] = bowl_runtime_assignment;
        if (!have_shell)
                mixed[n++] = "SHELL=/bin/sh";
        if (!have_tmpdir)
                mixed[n++] = "TMPDIR=/tmp";
        mixed[n] = null;
        return mixed;
}

/*
        Room on the filesystem that holds a bowl.

        A live session keeps /bowls on its root, a tmpfs cut to half of memory,
        and a bowl that fills it fails part way through a download, an unpack
        or a package manager's run with a message that names a file and not the
        reason. What is left is said, and what it is kept in: a tmpfs is memory
        as well as a size, and runs out of whichever is smaller.
*/
#define BOWL_TMPFS_MAGIC 0x01021994
#define BOWL_RAMFS_MAGIC 0x858458f6
#define BOWL_MEBIBYTE ((p64)1024 * 1024)

typedef struct
{
        p64 free;
        p64 total;
        p64 memory;
        bool in_memory;
} bowl_room;

// One figure from /proc/meminfo in bytes, or none when it is not there.
static p64 bowl_meminfo_bytes(string_address text, string_address name)
{
        positive length = string_length(name);

        for (positive at = 0; text[at]; at++)
        {
                p64 kilobytes = 0;

                if ((at && text[at - 1] != '\n') ||
                    string_compare_max(text + at, name, length))
                        continue;

                for (at += length; text[at] == ' '; at++)
                        ;
                for (; text[at] >= '0' && text[at] <= '9'; at++)
                        kilobytes = kilobytes * 10 + (p64)(text[at] - '0');

                return kilobytes * 1024;
        }

        return 0;
}

/*
        Asked through a descriptor rather than a path. An isolated guest's
        pivot_root moves the root of every process in its mount namespace, and
        bowl forked it inside that namespace, so after the pivot /bowls/NAME
        named nothing from here and the room could not be asked at all.
*/
#define BOWL_ROOM_OPEN (O_PATH | O_DIRECTORY | O_CLOEXEC)

static bool bowl_room_of(bipolar handle, bowl_room address_to room)
{
        file_mount_facts facts;
        p8 text[4096];

        memory_fill(room, 0, sizeof(*room));
        if (system_call_2(syscall(fstatfs), (positive)handle,
                          (positive)address_of facts) < 0)
                return false;

        p64 unit = (p64)(facts.fragment_size ? facts.fragment_size
                                             : facts.block_size);

        room->total = facts.blocks * unit;
        room->free = facts.blocks_available * unit;
        room->memory = (p64)-1;
        room->in_memory = facts.type == BOWL_TMPFS_MAGIC ||
                          facts.type == BOWL_RAMFS_MAGIC;

        // Memory can be given back by swapping, so swap counts as room too.
        if (room->in_memory &&
            file_slurp_once_at(AT_FDCWD, (string_address) "/proc/meminfo",
                               text, sizeof(text)) > 0)
                room->memory = bowl_meminfo_bytes(text, "MemAvailable:") +
                               bowl_meminfo_bytes(text, "SwapFree:");

        // ramfs has no size of its own: memory is all the room it has.
        if (room->in_memory && !facts.blocks)
                room->free = room->total = room->memory;

        return true;
}

static bool bowl_room_at(string_address path, bowl_room address_to room)
{
        bipolar handle = system_open_at(AT_FDCWD, path, BOWL_ROOM_OPEN);
        bool known;

        if (handle < 0)
                return false;

        known = bowl_room_of(handle, room);
        system_close(handle);
        return known;
}

static fn bowl_room_hint(bowl_room address_to room)
{
        if (room->in_memory)
                string_format(log, bowl_label "/bowls is kept in memory, as a live "
                                              "session keeps it; moonwater install "
                                              "DISK puts bowls on the disk's data "
                                              "partition\n");
}

/*
        After a step failed: when what is left is a sixteenth of the filesystem
        or less, capped at 256 MiB, space is the likely reason, and how much is
        said. The numbers are the filesystem's own; nothing is guessed but
        whether they are worth a line.
*/
static p64 bowl_room_low(bowl_room address_to room)
{
        return room->total / 16 < 256 * BOWL_MEBIBYTE ? room->total / 16
                                                      : 256 * BOWL_MEBIBYTE;
}

static fn bowl_room_say_low_of(bipolar handle, string_address path)
{
        bowl_room room;

        if (!bowl_room_of(handle, address_of room) ||
            room.free > bowl_room_low(address_of room))
                return;

        string_format(log, bowl_label "%s has %p MiB free of %p MiB\n", path,
                      (positive)(room.free / BOWL_MEBIBYTE),
                      (positive)(room.total / BOWL_MEBIBYTE));
        bowl_room_hint(address_of room);
        log_flush();
}

static fn bowl_room_say_low(string_address path)
{
        bipolar handle = system_open_at(AT_FDCWD, path, BOWL_ROOM_OPEN);

        if (handle < 0)
                return;

        bowl_room_say_low_of(handle, path);
        system_close(handle);
}

/*
        How low the room got while a guest ran.

        A package manager that fails for want of room has tidied up by the time
        it returns: on a 1 GB live root dpkg stopped at "No space left on
        device" and apt exited with 218 MiB free again, so what is free
        afterwards says nothing. The filesystem is looked at while the guest
        runs instead -- a statfs ten times a second, from a poll on the child's
        pidfd so that a quick command is not held up by it. The moment a write
        fails is too short to be seen, but the filling that leads to it takes
        seconds, so the lowest it saw is what is kept. The root is the
        descriptor opened before the guest pivoted.
*/
static p64 bowl_wait_watching(bipolar child, bipolar root)
{
        p64 lowest = (p64)-1;
        bipolar watch = system_call_2(syscall(pidfd_open), (positive)child, 0);

        while (watch >= 0)
        {
                file_mount_facts facts;
                system_poll_descriptor wanted = {(b32)watch, SYSTEM_POLL_READ, 0};
                timespec tenth = {0, 100000000};
                bipolar ready;

                if (system_call_2(syscall(fstatfs), (positive)root,
                                  (positive)address_of facts) >= 0 &&
                    facts.blocks)
                {
                        p64 unit = (p64)(facts.fragment_size ? facts.fragment_size
                                                             : facts.block_size);

                        if (facts.blocks_available * unit < lowest)
                                lowest = facts.blocks_available * unit;
                }

                ready = system_poll_wait(address_of wanted, 1, address_of tenth,
                                         null);
                if (ready != 0 && ready != -EINTR)
                        break;
        }

        if (watch >= 0)
                system_close(watch);
        return lowest;
}

/*
        After a guest failed: how low the room got while it ran, when that was
        low, or how low it is now. Both are the filesystem's own numbers, and a
        run that never came near filling it says nothing.
*/
static fn bowl_room_after(bipolar handle, string_address root, p64 lowest)
{
        bowl_room room;

        if (!bowl_room_of(handle, address_of room))
                return;

        p64 low = bowl_room_low(address_of room);

        if (lowest > low || lowest >= room.free)
        {
                bowl_room_say_low_of(handle, root);
                return;
        }

        string_format(log, bowl_label "%s was down to %p MiB free of %p MiB while "
                                      "this ran, and has %p MiB now\n", root,
                      (positive)(lowest / BOWL_MEBIBYTE),
                      (positive)(room.total / BOWL_MEBIBYTE),
                      (positive)(room.free / BOWL_MEBIBYTE));
        bowl_room_hint(address_of room);
        log_flush();
}

static b32 bowl_launch(string_address root, string_address program,
                       string_address address_to arguments,
                       bool isolated)
{
        string_address native_arguments[] = {BOWL_NATIVE_SHELL, null};
        string_address address_to environment =
            bowl_environment(file_environment_all());
        bipolar native_shell = -1;
        bipolar failed;
        bipolar child;
        positive status = 0;

        if (!root || root[0] != '/')
                return bowl_usage();

        terminal_terminfo_install();

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
            (isolated ? CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWPID : 0));
        if (failed)
                return bowl_launch_failed(native_shell,
                    isolated ? "cannot make isolated views"
                             : "cannot make a mount view", failed);

        /* Success never returns: this process becomes the command. */
        if (!isolated)
                bowl_inside(root, program, arguments, environment,
                            native_shell, false);

        // Opened before the guest pivots this namespace's root away from it.
        bipolar room = system_open_at(AT_FDCWD, root, BOWL_ROOM_OPEN);

        /* CLONE_NEWPID places the next child, not this caller, in the view. */
        child = system_fork();
        if (child < 0)
        {
                if (room >= 0)
                        system_close(room);
                return bowl_launch_failed(native_shell, "cannot start", child);
        }

        if (child == 0)
                bowl_inside(root, program, arguments, environment,
                            native_shell, true);

        if (native_shell >= 0)
                system_close(native_shell);

        p64 lowest = room >= 0 ? bowl_wait_watching(child, room) : (p64)-1;

        failed = system_wait4_retry(child, address_of status, 0, null);

        // apt says a file could not be written; this says the root was full.
        b32 code = failed < 0 ? 1 : wait_status_code(status);

        if (failed >= 0 && code && room >= 0)
                bowl_room_after(room, root, lowest);
        if (room >= 0)
                system_close(room);
        return code;
}

#include "unpack.c"
#include "setup.c"

static b32 bowl_main()
{
        string_address address_to arguments = program_argument_list();
        positive count = (positive)program_argument_count();
        positive root_at = 1;
        string_address root;
        string_address program = null;
        string_address address_to command_arguments = null;
        bool isolated = false;
        bool isolated_told = false;
        p8 launcher_root[BOWL_PATH_LIMIT];

        if (!arguments || count < 2)
                return bowl_usage();

        if (string_equals(arguments[1], "setup"))
                return bowl_setup(count, arguments);

        if (string_equals(arguments[1], "expose"))
                return bowl_expose(count, arguments);

        if (string_equals(arguments[1], "--isolated"))
        {
                isolated = true;
                isolated_told = true;
                root_at++;
        }
        else if (string_equals(arguments[1], "--fast"))
        {
                isolated_told = true;
                root_at++;
        }
        else if (arguments[1][0] == '-' && arguments[1][1] == '-')
                return bowl_usage();

        if (root_at >= count)
                return bowl_usage();

        root = arguments[root_at];

        if (root[0] == '@')
        {
                /* argv[2] is the launcher filename inserted by binfmt_script. */
                if (root_at != 1 || count < 3 ||
                    !bowl_launcher(root, launcher_root,
                                   sizeof(launcher_root), address_of program))
                        return bowl_refuse("invalid exposed command\n");

                root = launcher_root;
                arguments[2] = program;
                command_arguments = arguments + 2;
        }
        else if (root_at + 1 < count)
        {
                program = arguments[root_at + 1];
                command_arguments = arguments + root_at + 1;
        }

        if (!isolated_told)
                isolated = bowl_needs_isolated(program);

        /* Isolated guests need the host's nameservers. Pacman/apk/apt conf
           and lock files are written when the tree is landed, not on every
           enter: clearing locks here raced a manager already running in
           that root. */
        if (isolated)
        {
                b32 failed = bowl_write_resolv(root);

                if (failed)
                        return failed;
        }

        return bowl_launch(root, program, command_arguments, isolated);
}
