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

static bipolar bowl_open_directory(string_address path, bool create,
                                   bool address_to made);

static bipolar bowl_mkdir(string_address path)
{
        bipolar handle = bowl_open_directory(path, true, null);

        if (handle < 0)
                return handle;
        return system_close(handle);
}

/*
        Pin every component with O_NOFOLLOW so a name such as /tmp/x -> /etc
        cannot redirect mkdir or chmod into a host tree the session denylist
        already refused by string. Missing components are created only
        relative to the directory already held.

        create says how much of a path that is not there may be made:

          BOWL_MAKE_NONE   none of it; the walk is a lookup
          BOWL_MAKE_LEAF   the last component, and only under a parent that
                           is already there
          BOWL_MAKE_CHAIN  every component, which is for a path this file
                           chose rather than one it was handed

        made says whether the last component is one this walk created, which
        is the only directory a session may go on to change the mode of: a
        path that was already there belongs to whoever put it there.
*/
#define BOWL_MAKE_NONE 0
#define BOWL_MAKE_LEAF 1
#define BOWL_MAKE_CHAIN 2

static bipolar bowl_open_directory(string_address path, p8 create,
                                   bool address_to made)
{
        p8 name[256];
        bipolar held;
        positive flags = FILE_READ | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
        string_address at = path;

        if (made)
                address_to made = false;
        if (!path || !string_get(path))
                return -22;

        held = system_open_at(AT_FDCWD,
                              path[0] == '/' ? (string_address)"/"
                                             : (string_address)".",
                              flags);
        if (held < 0)
                return held;

        if (path[0] == '/')
                at++;

        while (string_get(at))
        {
                positive n = 0;
                bipolar next;

                while (string_is(at, '/'))
                        at++;
                if (!string_get(at))
                        break;
                while (string_get(at) && string_not(at, '/') &&
                       n + 1 < sizeof(name))
                        name[n++] = string_get(at++);
                if (string_get(at) && string_not(at, '/'))
                {
                        system_close(held);
                        return -36;
                }
                name[n] = end;
                if (n == 1 && name[0] == '.')
                        continue;
                if (n == 2 && name[0] == '.' && name[1] == '.')
                {
                        system_close(held);
                        return -22;
                }
                string_address after = at;
                bool here = false;
                bool last;

                while (string_is(after, '/'))
                        after++;
                last = !string_get(after);

                next = system_open_at(held, name, flags);
                if (next < 0 && next == -ERROR_NO_ENTRY &&
                    (create == BOWL_MAKE_CHAIN ||
                     (create == BOWL_MAKE_LEAF && last)))
                {
                        bipolar fresh = system_make_directory_at(held, name,
                                                                 0755);

                        here = fresh >= 0;
                        next = fresh < 0 && fresh != -ERROR_EXISTS
                                   ? fresh
                                   : system_open_at(held, name, flags);
                }
                system_close(held);
                if (next < 0)
                        return next;
                held = next;
                /* The last component to get here is the leaf. */
                if (made)
                        address_to made = here;
        }

        return held;
}

// A mount point below a directory Moonwater itself may not have.
static bipolar bowl_mkdir_parents_made(string_address path, p8 create,
                                       bool address_to made)
{
        bipolar handle = bowl_open_directory(path, create, made);

        if (handle < 0)
                return handle;
        system_close(handle);
        return 0;
}

static bipolar bowl_mkdir_parents(string_address path)
{
        return bowl_mkdir_parents_made(path, BOWL_MAKE_CHAIN, null);
}

#define BOWL_RESOLVE_NO_MAGICLINKS 0x02
#define BOWL_RESOLVE_IN_ROOT 0x10

/*
        Open a path as though root were /. Absolute symlinks therefore remain
        inside the bowl, and relative symlinks cannot climb above it. This is
        the pathname rule isolated mode gets from pivot_root, made explicit for
        setup and fast mode where the host root is still mounted.
*/
static bipolar bowl_open_in_root(string_address root, string_address path,
                                 positive flags)
{
        struct
        {
                p64 flags;
                p64 mode;
                p64 resolve;
        } how = {
            flags,
            0,
            BOWL_RESOLVE_IN_ROOT | BOWL_RESOLVE_NO_MAGICLINKS,
        };
        bipolar root_handle = bowl_open_directory(root, false, null);
        bipolar opened;

        if (root_handle < 0)
                return root_handle;

        opened = system_call_4(syscall(openat2), (positive)root_handle,
                               (positive)path, (positive)address_of how,
                               sizeof(how));
        system_close(root_handle);
        return opened;
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

/* . or .. as a component, or //, so a path cannot walk out of its root.
   The session environment asks the same question of HOME and
   XDG_RUNTIME_DIR, and both standalone harness windows compile this one
   walk rather than each keeping a copy of it. */
static bool bowl_path_steps(string_address path)
{
        if (!path || path[0] != '/')
                return true;

        for (; *path; path++)
        {
                if (*path != '/')
                        continue;
                if (path[1] == '/')
                        return true;
                if (path[1] == '.' &&
                    (!path[2] || path[2] == '/' ||
                     (path[2] == '.' && (!path[3] || path[3] == '/'))))
                        return true;
        }

        return false;
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

        if (bowl_path_steps(program))
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
        /*
                Bowl roots are unpacked directory trees, not mount trees.
                A recursive bind would also clone any mount an operator or a
                compromised guest had placed below one of those directories.
                The bind remount below makes this mount read-only, but does not
                reliably turn every cloned child mount read-only on older mount
                APIs.  Do not import child mounts in the first place.
        */
        bipolar failed = system_mount(source, target, 0, MS_BIND, 0);

        if (failed)
                return failed;

        return system_mount(0, target, 0,
                            MS_BIND | MS_REMOUNT | MS_RDONLY, 0);
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

        if (bowl_path_steps(program))
                return bowl_refuse("an exposed path cannot contain . or ..\n");

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
static fn bowl_session_prepare_at(string_address home, string_address runtime,
                                  bool user_dirs);
static string_address address_to bowl_environment(
    string_address address_to inherited);

static DEAD_END fn bowl_inside(string_address root,
                               string_address program,
                               string_address address_to arguments,
                               string_address address_to environment,
                               bipolar native_shell, bool isolated)
{
        bipolar program_handle = -1;
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

        /*
                Fast mode has not pivoted, so executing ROOT + /program by
                pathname lets an absolute symlink inside the bowl jump back to
                the host root. Pin the program with the same in-root resolver
                used by setup, then execute that object. O_PATH deliberately
                stays open across exec: Linux needs the descriptor available
                when an execveat target is a shebang script.
        */
        if (!isolated && native_shell < 0)
        {
                if (!program || program[0] != '/')
                {
                        bowl_fail(program ? program : root, -ERROR_INVALID);
                        exit(1);
                }

                program_handle = bowl_open_in_root(root, program, O_PATH);
                if (program_handle < 0)
                {
                        bowl_fail(program, program_handle);
                        exit(1);
                }
        }

        failed = native_shell >= 0
            ? system_call_5(syscall(execveat), (positive)native_shell,
                             (positive)"", (positive)arguments,
                             (positive)environment, AT_EMPTY_PATH)
            : program_handle >= 0
                ? system_call_5(syscall(execveat), (positive)program_handle,
                                (positive)"", (positive)arguments,
                                (positive)environment, AT_EMPTY_PATH)
                : system_execute(program, arguments, environment);
        if (program_handle >= 0)
                system_close(program_handle);
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

static b32 bowl_session_host_path(string_address path)
{
        static string_address trees[] = {
            "/etc", "/usr", "/bin", "/sbin", "/boot", "/lib", "/lib64",
            "/proc", "/sys", null};
        positive i;

        if (!path)
                return false;

        if (bowl_path_same(path, "/"))
                return true;

        for (i = 0; trees[i]; i++)
        {
                positive n = string_length(trees[i]);

                if (string_compare_max(path, trees[i], n))
                        continue;
                if (!path[n] || path[n] == '/')
                        return true;
        }

        return false;
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
                        return bowl_path_steps(value) ||
                               bowl_session_host_path(value);
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
                return bowl_path_steps(value) ||
                       bowl_session_host_path(value);

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
        bipolar handle = bowl_open_directory(path, BOWL_MAKE_NONE, null);

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

        /*  The home itself has to be there: these are the directories a
            desktop keeps inside somebody's home, not a reason to invent
            the home. The list is in order, so .local stands before
            .local/share asks for it. */
        for (i = 0; names[i]; i++)
                if (bowl_root_path(path, sizeof(path), home, names[i]))
                        bowl_mkdir_parents_made(path, BOWL_MAKE_LEAF, null);
}

static fn bowl_session_prepare_at(string_address home, string_address runtime,
                                  bool user_dirs)
{
        bool made = false;

        bowl_session_fill();

        if (!runtime || bowl_path_steps(runtime) ||
            bowl_session_host_path(runtime))
                runtime = bowl_runtime_path;

        /*  0700 is Weston's requirement of the runtime directory it is
            handed, and this makes the one it creates meet it. A directory
            that was already there is not this session's to change: the name
            comes from XDG_RUNTIME_DIR, so chmodding an existing path would
            let any inherited environment take a host tree the string
            denylist does not happen to name -- /home, /var, /opt -- down to
            0700, from every shell start, not only from bowl.

            How much of it may be made apart from that. /run/user/<uid> is
            this file's own name and /run/user is ours to make, so that one
            is made in full. A name the environment chose gets its last
            directory and no more: XDG_RUNTIME_DIR=/tmp/a/b/c quietly made
            three directories at every shell start, and a path nobody has
            prepared is a typo far more often than it is a plan. Tighter
            than safe makes none of it and waits for the directory to be
            there. */
        p8 make = runtime == bowl_runtime_path ? BOWL_MAKE_CHAIN
                : MOONWATER_STRICT >= STRICT_TIGHT ? BOWL_MAKE_NONE
                                                   : BOWL_MAKE_LEAF;

        bowl_mkdir_parents_made(runtime, make, address_of made);
        if (made && !bowl_runtime_shared(runtime))
                bowl_chmod_directory(runtime, 0700);
        bowl_mkdir("/tmp");
        bowl_chmod_directory("/tmp", 01777);
        bowl_mkdir("/dev/shm");
        bowl_mkdir("/run/lock");
        bowl_mkdir("/var");
        bowl_dev_link("/run", "/var/run");
        bowl_dev_link("/run/lock", "/var/lock");
        if (!user_dirs)
                return;
        if (!home || bowl_path_steps(home) || bowl_session_host_path(home))
                home = (string_address)BOWL_SESSION_HOME;
        bowl_session_home_dirs(home);
}

static fn bowl_session_prepare(string_address home, string_address runtime)
{
        bowl_session_prepare_at(home, runtime, true);
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

                        /*  Out of room. Handing back the block as it
                            arrived is only safe while nothing has been
                            dropped from it: once an entry has been refused,
                            returning the original puts that entry back into
                            the guest, which is the whole of what the refusal
                            was for. What fit is then what is passed. */
                        if (n + 1 >= BOWL_ENV_ROOM)
                        {
                                if (!skipped)
                                        return inherited;
                                mixed[n] = null;
                                return mixed;
                        }

                        mixed[n++] = inherited[at];
                }

                if (!skipped && have_term && have_terminfo && have_home &&
                    have_path && have_lang && have_user && have_logname &&
                    have_runtime && have_shell && have_tmpdir)
                        return inherited;

                if (n + BOWL_ENV_DEFAULTS >= BOWL_ENV_ROOM)
                {
                        if (!skipped)
                                return inherited;
                        mixed[n] = null;
                        return mixed;
                }
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


/* ---- Landing a bootstrap at /bowls/NAME: extract, flatten, configure. ---- */

/*
        Land a bootstrap at /bowls/NAME.

        Setup downloads the archive; this file does not talk to pacman, apt
        or apk. It probes the compression magic, extracts, hoists a prefix
        directory when the marker is under one child (Arch root.x86_64, a
        Nix version dir), and         writes the files a first isolated install
        actually needs -- resolv.conf without a stub resolver, plus the
        manager conf that exists in that tree. Isolated launches rewrite
        resolv.conf only; they do not clear manager locks.

        gzip and xz applets are compiled in. Extract looks at the archive
        magic and `tar` unpacks gzip, xz, zstd and uncompressed streams
        in-process.
*/



#define BOWL_TEXT 131072
#define BOWL_GEO_MIRROR \
        "Server = https://geo.mirror.pkgbuild.com/$repo/os/$arch\n"

static bool bowl_has(string_address root, string_address path)
{
        bipolar found;

        if (!bowl_named_root(root) || bowl_path_steps(path))
                return false;

        /*
                In particular, /usr/bin/pacman -> /bin/true means the bowl's
                /bin/true, never the host's. A plain access(ROOT + path) would
                cross that boundary for absolute symlinks.
        */
        found = bowl_open_in_root(root, path, O_PATH | O_CLOEXEC);
        if (found < 0)
                return false;

        system_close(found);
        return true;
}

static bool bowl_root_busy(string_address root)
{
        file_walk walk;
        struct linux_dirent64 address_to entry;
        bool busy = false;

        if (!file_walk_open(address_of walk, AT_FDCWD, root))
                return false;

        while ((entry = file_walk_next(address_of walk)))
        {
                if (!file_is_dot(entry->d_name))
                {
                        busy = true;
                        break;
                }
        }

        file_walk_close(address_of walk);
        return busy;
}

#define BOWL_RESET_DEPTH 48

/* Entries a caller leaves where they are. Null ends the list, and a null
   list keeps nothing. */
static bool bowl_reset_kept(string_address address_to keep, string_address name)
{
        for (positive at = 0; keep && keep[at]; at++)
                if (string_equals(name, keep[at]))
                        return true;

        return false;
}

/*
        Everything under a directory, gone: a subdirectory emptied first and
        then removed, anything else unlinked. An entry already missing is
        somebody else having removed it, not a failure.

        A name the walk itself found is opened with O_NOFOLLOW, so a
        subdirectory swapped for a symlink between the stat and the open
        fails closed instead of walking out of the tree. A path somebody
        named is opened the way naming one means, following: that is how
        moonwater wipe reaches a /root the machine has put something on.

        keep names entries to leave, and only at this level -- the recursion
        passes none, so a kept name further down is an ordinary entry.
*/
static bipolar bowl_reset_walk_at(bipolar directory, string_address name,
                                  positive depth, bool named,
                                  string_address address_to keep)
{
        file_walk walk;
        struct linux_dirent64 address_to entry;
        bipolar failed = 0;

        if (depth >= BOWL_RESET_DEPTH)
                return -ERROR_LOOP;

        if (!(named ? file_walk_open(address_of walk, directory, name)
                    : file_walk_open_found(address_of walk, directory, name)))
                return walk.error == -ERROR_NO_ENTRY ? 0 : walk.error;

        while (!failed && (entry = file_walk_next(address_of walk)))
        {
                if (file_is_dot(entry->d_name) ||
                    bowl_reset_kept(keep, entry->d_name))
                        continue;

                if (file_is_directory(walk.handle, entry->d_name))
                {
                        /* A path somebody named is the tree's own root and
                           not a step into it, so the cap counts from below. */
                        failed = bowl_reset_walk_at(walk.handle, entry->d_name,
                                                    named ? depth : depth + 1,
                                                    false, null);
                        if (!failed)
                                failed = system_remove_at(walk.handle,
                                    entry->d_name, AT_REMOVEDIR);
                }
                else
                        failed = system_remove_at(walk.handle, entry->d_name, 0);

                if (failed == -ERROR_NO_ENTRY)
                        failed = 0;
        }

        if (!failed)
                failed = walk.error;
        file_walk_close(address_of walk);
        return failed;
}

static bipolar bowl_reset_walk(string_address path, positive depth)
{
        p8 leaf[BOWL_PATH_LIMIT];
        bipolar parent = system_open_parent_nofollow(AT_FDCWD, path, false, 0,
                                                      leaf, sizeof(leaf));
        if (parent < 0)
                return parent == -ERROR_NO_ENTRY ? 0 : parent;
        bipolar failed = bowl_reset_walk_at(parent, leaf, depth, false, null);
        system_close(parent);
        return failed;
}

static b32 bowl_reset_root(string_address root)
{
        bipolar failed = bowl_reset_walk(root, 0);

        if (failed < 0)
                return bowl_fail(root, failed);
        return 0;
}

static b32 bowl_forget_path(string_address path)
{
        p8 leaf[BOWL_PATH_LIMIT];
        bipolar parent = system_open_parent_nofollow(AT_FDCWD, path, false, 0,
                                                      leaf, sizeof(leaf));
        bipolar failed;

        if (parent < 0)
                return parent == -ERROR_NO_ENTRY ? 0 : bowl_fail(path, parent);
        if (file_is_directory(parent, leaf))
        {
                failed = bowl_reset_walk_at(parent, leaf, 0, false, null);
                if (!failed)
                        failed = system_remove_at(parent, leaf, AT_REMOVEDIR);
        }
        else
                failed = system_remove_at(parent, leaf, 0);
        system_close(parent);

        if (failed < 0 && failed != -ERROR_NO_ENTRY)
                return bowl_fail(path, failed);
        return 0;
}

static b32 bowl_recover_from(string_address root, string_address marker)
{
        p8 from[BOWL_PATH_LIMIT];

        if (!bowl_root_path(from, sizeof(from), root, ".bowl-from"))
                return bowl_refuse("bowl path is too long\n");

        if (system_access_at(AT_FDCWD, from, 0) < 0)
                return 0;

        if (bowl_has(root, marker) || bowl_root_busy(root))
                return bowl_forget_path(from);

        system_remove_at(AT_FDCWD, root, AT_REMOVEDIR);
        {
                bipolar failed = system_rename_at(AT_FDCWD, from, AT_FDCWD,
                                                  root, 0);

                if (failed < 0)
                        return bowl_fail(from, failed);
        }
        return 0;
}

static b32 bowl_write_bytes(string_address path, string_address text,
                            positive length)
{
        p8 temp[BOWL_PATH_LIMIT];
        p8 leaf[BOWL_PATH_LIMIT];
        bipolar parent = system_open_parent_nofollow(AT_FDCWD, path, false, 0,
                                                      leaf, sizeof(leaf));
        bipolar handle;
        bipolar failed = 0;

        if (parent < 0)
                return bowl_fail(path, parent);

        handle = file_temporary_open_at(parent, leaf, temp, sizeof(temp),
            ".bowl-", 6, (positive)system_call_1(syscall(getpid), 0), 128, 0644);
        if (handle < 0)
        {
                system_close(parent);
                return bowl_fail(path, handle);
        }

        if (system_write_all((positive)handle, text, length) != length)
                failed = -ERROR_INPUT_OUTPUT;
        if (!failed)
                failed = system_call_1(syscall(fsync), (positive)handle);
        system_close(handle);

        if (!failed)
                failed = system_rename_at(parent, temp, parent, leaf, 0);
        if (failed < 0)
                system_remove_at(parent, temp, 0);
        system_close(parent);

        return failed < 0 ? bowl_fail(path, failed) : 0;
}

static string_address bowl_line_word(string_address line,
                                     bool address_to commented)
{
        line += string_span(line, string_set_blanks);
        address_to commented = *line == '#';
        if (address_to commented)
                line += 1 + string_span(line + 1, string_set_blanks);
        return line;
}

static bool bowl_keyword(string_address line, string_address word)
{
        positive length = string_length(word);

        if (string_compare_max(line, word, length))
                return false;

        return !line[length] || line[length] == ' ' || line[length] == '\t' ||
               line[length] == '=' || line[length] == '\n';
}

static bool bowl_section_is(string_address line, string_address name)
{
        positive length = string_length(name);

        if (line[0] != '[')
                return false;
        if (string_compare_max(line + 1, name, length))
                return false;
        return line[1 + length] == ']';
}

static bool bowl_isolation_keyword(string_address line)
{
        return bowl_keyword(line, "DisableSandbox") ||
               bowl_keyword(line, "DisableSandboxFilesystem") ||
               bowl_keyword(line, "DisableSandboxSyscalls");
}

static bool bowl_put_isolation(byte_store address_to out)
{
        string_address text =
            "DisableSandboxFilesystem\n"
            "DisableSandboxSyscalls\n";

        return byte_store_append_exact(out, text, string_length(text));
}

static bool bowl_options_keyword(string_address text, string_address word)
{
        positive at = 0;
        positive length = string_length(text);
        bool in_options = false;

        while (at < length)
        {
                bool commented = false;
                positive stop = at + memory_span_without_byte(text + at, '\n',
                                                              length - at);
                string_address token;

                token = bowl_line_word(text + at, address_of commented);
                if (!commented && token[0] == '[')
                        in_options = bowl_section_is(token, "options");

                if (in_options && !commented && bowl_keyword(token, word))
                        return true;

                at = stop + (stop < length);
        }

        return false;
}

static bool bowl_nameserver_ok(string_address line, positive length)
{
        /* Blanks stop at the line's newline or terminator, so they stay
           inside length. */
        positive at = string_span(line, string_set_blanks);

        if (length - at < 11 || string_compare_max(line + at, "nameserver ", 11))
                return false;

        at += 11;
        at += string_span(line + at, string_set_blanks);

        if (at >= length || line[at] == '\n' || line[at] == '#')
                return false;

        return string_compare_max(line + at, "127.", 4) != 0;
}

static b32 bowl_wait_applet(bipolar child, string_address what)
{
        positive status = 0;
        bipolar failed;

        if (child < 0)
                return bowl_fail(what, child);

        failed = system_wait4_retry(child, address_of status, 0, null);
        if (failed < 0)
                return bowl_fail(what, failed);

        if (wait_status_code(status))
                return bowl_refuse(what);

        return 0;
}

/* A bootstrap is a ustar archive or one packed by a codec tar reads. */
static bool bowl_archive_known(string_address archive)
{
        p8 head[512];
        bipolar handle = system_open_at(AT_FDCWD, archive, FILE_READ | O_CLOEXEC);
        bipolar got;

        if (handle < 0)
                return false;

        got = system_read_retry((positive)handle, head, sizeof(head));
        system_close(handle);
        if (got < 6)
                return false;

        p8 pack = tar_pack_from_magic(head, (positive)got);
        return pack == TAR_PACK_GZIP || pack == TAR_PACK_XZ ||
               pack == TAR_PACK_ZSTD ||
               (got >= 262 && !string_compare_max(head + 257, "ustar", 5));
}

static b32 bowl_extract(string_address archive, string_address root)
{
        string_address tar_file[] = {
            "tar", "-x", "-f", archive, "-C", root, null};
        bipolar extract;

        log_flush();
        if (bowl_archive_known(archive))
        {
                extract = system_fork();
                if (extract == 0)
                {
                        program_arguments_use(tar_file, 6);
                        exit(file_tar_without_special());
                }

                return bowl_wait_applet(extract,
                                        "tar could not extract the archive\n");
        }

        return bowl_refuse("archive is not a bootstrap\n");
}

/* The children of root holding the marker -- none, one, or 2 for more than
   one -- with the first kept in rel as "/NAME", or negative when root does
   not open as a directory. Landing asks whether a busy root is a prefix
   waiting to be hoisted; flattening hoists only a lone one. */
static bipolar bowl_prefix_find(string_address root, string_address marker,
                                p8 address_to rel)
{
        file_walk walk;
        struct linux_dirent64 address_to entry;
        p8 from[BOWL_PATH_LIMIT];
        p8 name[258];
        bipolar found = 0;

        if (!file_walk_open(address_of walk, AT_FDCWD, root))
                return -ERROR_NOT_DIRECTORY;

        while (found < 2 && (entry = file_walk_next(address_of walk)))
        {
                if (file_is_dot(entry->d_name))
                        continue;

                name[0] = '/';
                string_copy_max_end(name + 1, entry->d_name, sizeof(name) - 2);
                if (bowl_root_path(from, sizeof(from), root, name) &&
                    bowl_has(from, marker) && !found++)
                        memory_copy(rel, name, sizeof(name));
        }

        file_walk_close(address_of walk);
        return found;
}

static b32 bowl_flatten(string_address root, string_address marker)
{
        p8 sibling[BOWL_PATH_LIMIT];
        p8 from[BOWL_PATH_LIMIT];
        p8 rel[258];
        bipolar failed;

        if (bowl_has(root, marker))
                return 0;

        failed = bowl_prefix_find(root, marker, rel);
        if (failed < 0)
                return bowl_fail(root, failed);
        if (failed != 1)
                return bowl_refuse(failed ? "archive has more than one root\n"
                                          : "archive is not a bowl bootstrap\n");

        if (!bowl_root_path(sibling, sizeof(sibling), root, ".bowl-from"))
                return bowl_refuse("bowl path is too long\n");

        if (system_access_at(AT_FDCWD, sibling, 0) >= 0)
        {
                failed = bowl_forget_path(sibling);
                if (failed)
                        return failed;
        }

        failed = system_rename_at(AT_FDCWD, root, AT_FDCWD, sibling, 0);
        if (failed < 0)
                return bowl_fail(sibling, failed);

        if (!bowl_root_path(from, sizeof(from), sibling, rel))
                return bowl_refuse("bowl path is too long\n");

        failed = system_rename_at(AT_FDCWD, from, AT_FDCWD, root, 0);
        if (failed < 0)
                return bowl_fail(from, failed);

        if (system_remove_at(AT_FDCWD, sibling, AT_REMOVEDIR) < 0)
                bowl_forget_path(sibling);
        return 0;
}

static b32 bowl_write_resolv(string_address root)
{
        p8 path[BOWL_PATH_LIMIT];
        p8 host[4096];
        p8 buffer[4096];
        byte_store out = {buffer, sizeof(buffer), 0};
        bipolar got;
        bipolar failed;
        positive at = 0;
        string_address fallback = "nameserver 1.1.1.1\n";

        if (!bowl_root_path(path, sizeof(path), root, "/etc/resolv.conf"))
                return bowl_refuse("bowl path is too long\n");

        {
                p8 etc[BOWL_PATH_LIMIT];

                if (!bowl_root_path(etc, sizeof(etc), root, "/etc"))
                        return bowl_refuse("bowl path is too long\n");
                failed = bowl_mkdir(etc);
                if (failed < 0)
                        return bowl_fail(etc, failed);
        }

        if (!byte_store_append_exact(address_of out, fallback,
                                     string_length(fallback)))
                return bowl_refuse("resolv.conf is too long\n");

        got = file_slurp("/etc/resolv.conf", host, sizeof(host));
        if (got > 0 && (positive)got >= sizeof(host))
                got = (bipolar)(sizeof(host) - 1);
        if (got > 0)
                host[got] = end;
        while (got > 0 && at < (positive)got)
        {
                positive start = at;
                positive stop = start + memory_span_without_byte(
                                            host + start, '\n', (positive)got - start);

                if (bowl_nameserver_ok(host + start, stop - start) &&
                    string_compare_max(host + start, fallback,
                                       string_length(fallback) - 1))
                {
                        if (!byte_store_append_exact(address_of out, host + start,
                                                     stop - start) ||
                            !byte_store_append_exact(address_of out, "\n", 1))
                                return bowl_refuse("resolv.conf is too long\n");
                }

                at = stop + (stop < (positive)got);
        }

        return bowl_write_bytes(path, out.bytes, out.used);
}

static b32 bowl_write_mirror(string_address root)
{
        p8 path[BOWL_PATH_LIMIT];
        p8 text[BOWL_TEXT];
        p8 buffer[BOWL_TEXT];
        byte_store out = {buffer, sizeof(buffer), 0};
        bipolar got;
        positive at = 0;
        bool live = false;

        if (!bowl_root_path(path, sizeof(path), root, "/etc/pacman.d/mirrorlist"))
                return bowl_refuse("bowl path is too long\n");

        got = file_slurp(path, text, sizeof(text));
        if (got <= 0)
                return bowl_fail(path, got < 0 ? got : -ERROR_NO_ENTRY);

        if ((positive)got >= sizeof(text) - 1)
                return bowl_refuse("mirrorlist is too long\n");
        text[got] = end;

        while (at < (positive)got)
        {
                bool commented = false;
                positive start = at;

                at += memory_span_without_byte(text + at, '\n', (positive)got - at);

                if (bowl_keyword(bowl_line_word(text + start, address_of commented),
                                 "Server") &&
                    !commented)
                        live = true;

                at = at + (at < (positive)got);
        }

        if ((!live &&
             !byte_store_append_exact(address_of out, BOWL_GEO_MIRROR,
                                      string_length(BOWL_GEO_MIRROR))) ||
            !byte_store_append_exact(address_of out, text, (positive)got))
                return bowl_refuse("mirrorlist is too long\n");

        return bowl_write_bytes(path, out.bytes, out.used);
}

static fn bowl_clear_lock(string_address root, string_address rel)
{
        p8 path[BOWL_PATH_LIMIT];

        if (bowl_root_path(path, sizeof(path), root, rel))
        {
                p8 leaf[BOWL_PATH_LIMIT];
                bipolar parent = system_open_parent_nofollow(AT_FDCWD, path,
                    false, 0, leaf, sizeof(leaf));
                if (parent >= 0)
                {
                        system_remove_at(parent, leaf, 0);
                        system_close(parent);
                }
        }
}

static bool bowl_archive_usable(string_address path, p64 floor)
{
        file_facts facts;

        return file_look_at(path, address_of facts) && facts.size >= floor &&
               bowl_archive_known(path);
}

static b32 bowl_write_pacman(string_address root);
static b32 bowl_write_apk(string_address root);
static b32 bowl_write_apt(string_address root);

static b32 bowl_configure(string_address root)
{
        b32 failed = bowl_write_resolv(root);

        if (!failed && bowl_has(root, "/etc/pacman.conf"))
        {
                if (bowl_has(root, "/etc/pacman.d/mirrorlist"))
                        failed = bowl_write_mirror(root);
                if (!failed)
                        failed = bowl_write_pacman(root);
                bowl_clear_lock(root, "/var/lib/pacman/db.lck");
        }

        if (!failed && bowl_has(root, "/etc/apk/repositories"))
                failed = bowl_write_apk(root);
        bowl_clear_lock(root, "/lib/apk/db/lock");

        if (!failed && bowl_has(root, "/etc/apt"))
                failed = bowl_write_apt(root);
        bowl_clear_lock(root, "/var/lib/dpkg/lock");
        bowl_clear_lock(root, "/var/lib/dpkg/lock-frontend");
        bowl_clear_lock(root, "/var/lib/apt/lists/lock");
        bowl_clear_lock(root, "/var/cache/apt/archives/lock");
        bowl_clear_lock(root, "/run/dnf/dnf.conf.lock");
        bowl_clear_lock(root, "/var/lib/rpm/.rpm.lock");

        return failed;
}

static b32 bowl_write_apk(string_address root)
{
        p8 path[BOWL_PATH_LIMIT];
        p8 existing[4096];
        bipolar got;
        positive at = 0;
        string_address text =
            "https://dl-cdn.alpinelinux.org/alpine/latest-stable/main\n"
            "https://dl-cdn.alpinelinux.org/alpine/latest-stable/community\n";

        if (!bowl_root_path(path, sizeof(path), root, "/etc/apk/repositories"))
                return bowl_refuse("bowl path is too long\n");

        got = file_slurp(path, existing, sizeof(existing));
        if (got > 0 && (positive)got >= sizeof(existing))
                got = (bipolar)(sizeof(existing) - 1);
        if (got > 0)
                existing[got] = end;
        while (got > 0 && at < (positive)got)
        {
                bool commented = false;
                string_address word =
                    bowl_line_word(existing + at, address_of commented);

                if (!commented && !string_compare_max(word, "https://", 8))
                        return 0;

                at += memory_span_without_byte(existing + at, '\n',
                                               (positive)got - at);
                at = at + (at < (positive)got);
        }

        return bowl_write_bytes(path, text, string_length(text));
}

static b32 bowl_write_apt(string_address root)
{
        p8 dir[BOWL_PATH_LIMIT];
        p8 path[BOWL_PATH_LIMIT];
        bipolar failed;
        string_address text =
            "APT::Sandbox::User \"root\";\n"
            "DPkg::Use-Pty \"false\";\n";

        if (!bowl_root_path(dir, sizeof(dir), root, "/etc/apt/apt.conf.d") ||
            !bowl_root_path(path, sizeof(path), root,
                            "/etc/apt/apt.conf.d/99bowl"))
                return bowl_refuse("bowl path is too long\n");

        failed = bowl_mkdir(dir);
        if (failed < 0)
                return bowl_fail(dir, failed);

        return bowl_write_bytes(path, text, string_length(text));
}

static b32 bowl_write_pacman(string_address root)
{
        p8 path[BOWL_PATH_LIMIT];
        p8 text[BOWL_TEXT];
        p8 buffer[BOWL_TEXT];
        byte_store out = {buffer, sizeof(buffer), 0};
        bipolar got;
        positive at = 0;
        bool seen_options = false;
        bool injected = false;

        if (!bowl_root_path(path, sizeof(path), root, "/etc/pacman.conf"))
                return bowl_refuse("bowl path is too long\n");

        got = file_slurp(path, text, sizeof(text));
        if (got <= 0)
                return bowl_fail(path, got < 0 ? got : -ERROR_NO_ENTRY);

        if ((positive)got >= sizeof(text) - 1)
                return bowl_refuse("pacman.conf is too long\n");
        text[got] = end;

        /*
                Pacman only reads these from [options]. Appending them after
                [extra] is a no-op and prints "directive not recognized".
        */
        if (bowl_options_keyword(text, "DisableSandboxFilesystem") &&
            bowl_options_keyword(text, "DisableSandboxSyscalls"))
                return 0;

        while (at < (positive)got)
        {
                bool commented = false;
                positive start = at;
                positive stop = start + memory_span_without_byte(
                                            text + start, '\n', (positive)got - start);
                string_address word;

                word = bowl_line_word(text + start, address_of commented);

                if (!commented && word[0] == '[')
                {
                        if (bowl_section_is(word, "options"))
                                seen_options = true;
                        else if (!injected)
                        {
                                if ((!seen_options &&
                                     !byte_store_append_exact(address_of out,
                                                              "[options]\n", 10)) ||
                                    !bowl_put_isolation(address_of out))
                                        return bowl_refuse(
                                            "pacman.conf is too long\n");
                                injected = true;
                        }
                }

                if (!commented && bowl_isolation_keyword(word))
                {
                        at = stop + (stop < (positive)got);
                        continue;
                }

                if (bowl_keyword(word, "DownloadUser") ||
                    bowl_keyword(word, "CheckSpace"))
                {
                        if (!commented &&
                            !byte_store_append_exact(address_of out, "#", 1))
                                return bowl_refuse("pacman.conf is too long\n");
                }

                if (!byte_store_append_exact(address_of out, text + start,
                                             stop - start + (stop < (positive)got)))
                        return bowl_refuse("pacman.conf is too long\n");

                at = stop + (stop < (positive)got);
        }

        if (!injected)
        {
                if ((!seen_options &&
                     !byte_store_append_exact(address_of out, "[options]\n", 10)) ||
                    !bowl_put_isolation(address_of out))
                        return bowl_refuse("pacman.conf is too long\n");
        }

        return bowl_write_bytes(path, out.bytes, out.used);
}

static b32 bowl_land(string_address archive, string_address root,
                     string_address marker)
{
        bipolar failed;

        if (!bowl_named_root(root) || !archive || archive[0] != '/' ||
            !marker || marker[0] != '/')
                return bowl_refuse("setup needs an archive path and /bowls/NAME\n");

        failed = system_access_at(AT_FDCWD, archive, 0);
        if (failed < 0)
                return bowl_fail(archive, failed);

        failed = bowl_mkdir(BOWL_ROOT_DIRECTORY);
        if (!failed)
                failed = bowl_mkdir(root);
        if (failed < 0)
                return bowl_fail(root, failed);

        failed = bowl_recover_from(root, marker);
        if (failed)
                return failed;

        if (!bowl_has(root, marker))
        {
                if (bowl_root_busy(root))
                {
                        p8 rel[258];

                        failed = bowl_prefix_find(root, marker, rel) > 0
                                     ? bowl_flatten(root, marker)
                                     : bowl_reset_root(root);
                        if (failed)
                                return failed;
                }

                if (!bowl_has(root, marker))
                {
                        failed = bowl_extract(archive, root);
                        if (!failed)
                                failed = bowl_flatten(root, marker);
                        if (failed)
                        {
                                // Asked before the half-landed tree is taken
                                // away, while a full filesystem is still full.
                                bowl_room_say_low(BOWL_ROOT_DIRECTORY);
                                bowl_reset_root(root);
                                return failed;
                        }
                }

                if (!bowl_has(root, marker))
                        return bowl_refuse("archive is not a bowl bootstrap\n");
        }

        return bowl_configure(root);
}

/* ---- The built-in first boots, one pipeline per named distribution. ---- */

/*
        Built-in first boots.

        Every named setup shares one pipeline: become root, install /bowl,
        download if the marker is missing, land (extract by magic, flatten
        by marker), configure whatever tree that archive actually contains,
        then expose the manager on PATH. Pacman, apt and apk stay guest
        binaries. Distros differ by URL, floor, marker and a
        small prime step.
*/

#define BOWL_ARCH_URL \
        "https://geo.mirror.pkgbuild.com/iso/latest/archlinux-bootstrap-x86_64.tar.zst"
#define BOWL_ALPINE_URL \
        "https://dl-cdn.alpinelinux.org/alpine/latest-stable/releases/x86_64/alpine-minirootfs-3.24.1-x86_64.tar.gz"
#define BOWL_DEBIAN_URL \
        "https://github.com/debuerreotype/docker-debian-artifacts/raw/dist-amd64/stable/oci/blobs/rootfs.tar.gz"
#define BOWL_INTERPRETER "/bowl"
#define BOWL_BIN_DIRECTORY "/bin"

#define BOWL_PRIME_NONE 0
#define BOWL_PRIME_ARCH 1

struct bowl_distro
{
        string_address name;
        string_address label;
        string_address root;
        string_address store;
        string_address url;
        string_address marker;
        string_address next;
        string_address refuse;
        p64 floor;
        // What the download and the tree it unpacks to took, measured.
        p64 archive_bytes;
        p64 tree_bytes;
        p8 prime;
        string_address address_to expose;
};

static string_address bowl_sudo_places[] = {
    "/usr/bin/sudo", "/bin/sudo", "/usr/local/bin/sudo", null};
static string_address bowl_ln_places[] = {
    "/usr/bin/ln", "/bin/ln", null};

static string_address bowl_find_executable(string_address address_to places)
{
        for (; *places; places++)
                if (system_access_at(AT_FDCWD, *places, BOWL_ACCESS_EXECUTE) >=
                    0)
                        return *places;

        return null;
}

static bool bowl_is_root(void)
{
        return system_call_1(syscall(geteuid), 0) == 0;
}

static b32 bowl_setup_self(p8 address_to into, positive room);

static b32 bowl_setup_run(string_address path, string_address address_to argv,
                          string_address what)
{
        bipolar child;

        child = system_fork();
        if (child == 0)
        {
                system_execute(path, argv, file_environment_all());
                exit(127);
        }

        return bowl_wait_applet(child, what);
}

static b32 bowl_setup_download(string_address dest, string_address url,
                               p64 floor)
{
        p8 self[BOWL_PATH_LIMIT];
        p8 part[BOWL_PATH_LIMIT];
        string_address argv[6];

        string_format(log, bowl_label "downloading %s\n", url);
        log_flush();

        if (!bowl_root_path(part, sizeof(part), dest, ".part"))
                return bowl_refuse("bowl path is too long\n");

        if (bowl_setup_self(self, sizeof(self)))
                return 1;

        system_remove_at(AT_FDCWD, part, 0);

        argv[0] = "wget";
        argv[1] = "-q";
        argv[2] = "-O";
        argv[3] = part;
        argv[4] = url;
        argv[5] = null;

        if (bowl_setup_run(self, argv, "download failed\n"))
        {
                // Asked while the part that filled it is still there.
                bowl_room_say_low(BOWL_ROOT_DIRECTORY);
                system_remove_at(AT_FDCWD, part, 0);
                return 1;
        }

        if (!bowl_archive_usable(part, floor))
        {
                system_remove_at(AT_FDCWD, part, 0);
                return bowl_refuse("download was not a bootstrap archive\n");
        }

        system_remove_at(AT_FDCWD, dest, 0);
        if (system_rename_at(AT_FDCWD, part, AT_FDCWD, dest, 0) < 0)
        {
                system_remove_at(AT_FDCWD, part, 0);
                return bowl_refuse("could not keep the bootstrap archive\n");
        }

        return 0;
}

static b32 bowl_setup_isolated(string_address root, string_address program,
                               string_address address_to arguments,
                               string_address what)
{
        bipolar child;

        log_flush();
        {
                b32 failed = bowl_configure(root);

                if (failed)
                        return failed;
        }
        child = system_fork();
        if (child == 0)
                exit(bowl_launch(root, program, arguments, true));

        return bowl_wait_applet(child, what);
}

static b32 bowl_setup_self(p8 address_to into, positive room)
{
        bipolar got = file_link_text("/proc/self/exe", into, room);

        if (got < 0)
                return bowl_fail("/proc/self/exe", got);

        return 0;
}

static b32 bowl_setup_bind_via_sudo(string_address self)
{
        string_address sudo = bowl_find_executable(bowl_sudo_places);
        string_address ln = bowl_find_executable(bowl_ln_places);
        string_address argv[8];

        if (!sudo || !ln)
                return bowl_refuse("setup needs to write /bowls\n");

        argv[0] = sudo;
        argv[1] = "-n";
        argv[2] = ln;
        argv[3] = "-sfn";
        argv[4] = self;
        argv[5] = BOWL_INTERPRETER;
        argv[6] = null;

        return bowl_setup_run(sudo, argv, "cannot install /bowl\n");
}

static b32 bowl_setup_bind_interpreter(void)
{
        p8 self[BOWL_PATH_LIMIT];
        p8 current[BOWL_PATH_LIMIT];
        bipolar got;
        bipolar failed;

        if (bowl_setup_self(self, sizeof(self)))
                return 1;

        got = file_link_text(BOWL_INTERPRETER, current, sizeof(current));
        if (got >= 0 && string_equals(current, self))
                return 0;

        /*
                A regular file at /bowl already satisfies #!/bowl. Leave it.
                A dangling or differently-aimed symlink is replaced so this
                binary is the one later shebangs invoke.
        */
        if (got < 0 &&
            system_access_at(AT_FDCWD, BOWL_INTERPRETER, BOWL_ACCESS_EXECUTE) >=
                0)
                return 0;

        system_remove_at(AT_FDCWD, BOWL_INTERPRETER, 0);
        failed = system_symbolic_link_at(self, AT_FDCWD, BOWL_INTERPRETER);
        if (failed >= 0 || failed == -EEXIST)
                return 0;

        if (failed != -ERROR_NOT_PERMITTED && failed != -ERROR_ACCESS)
                return bowl_fail(BOWL_INTERPRETER, failed);

        return bowl_setup_bind_via_sudo(self);
}

static DEAD_END fn bowl_setup_reexec_root(string_address name)
{
        string_address sudo = bowl_find_executable(bowl_sudo_places);
        string_address argv[6];

        if (!sudo)
        {
                bowl_refuse("setup needs to write /bowls\n");
                exit(1);
        }

        argv[0] = sudo;
        argv[1] = "-n";
        argv[2] = BOWL_INTERPRETER;
        argv[3] = "setup";
        argv[4] = name;
        argv[5] = null;

        system_execute(sudo, argv, file_environment_all());
        bowl_refuse("cannot obtain root\n");
        exit(1);
}

static b32 bowl_setup_become_root(string_address name)
{
        if (bowl_setup_bind_interpreter())
                return 1;

        if (bowl_is_root())
                return 0;

        bowl_setup_reexec_root(name);
        return 1;
}

static b32 bowl_publish_bin(string_address name)
{
        p8 from[BOWL_PATH_LIMIT];
        p8 to[BOWL_PATH_LIMIT];
        bipolar failed;

        if (sizeof(BOWL_EXPOSE_DIRECTORY) + string_length(name) >=
                sizeof(from) ||
            sizeof(BOWL_BIN_DIRECTORY) + string_length(name) >= sizeof(to))
                return bowl_refuse("exposed path is too long\n");

        path_join(from, sizeof(from), BOWL_EXPOSE_DIRECTORY, name);
        path_join(to, sizeof(to), BOWL_BIN_DIRECTORY, name);

        if (system_access_at(AT_FDCWD, to, 0) >= 0)
                return 0;

        failed = system_symbolic_link_at(from, AT_FDCWD, to);
        if (failed < 0 && failed != -EEXIST)
                return bowl_fail(to, failed);

        return 0;
}

static b32 bowl_prime_arch(string_address root)
{
        string_address init_argv[] = {"/usr/bin/pacman-key", "--init", null};
        string_address populate_argv[] = {"/usr/bin/pacman-key", "--populate",
                                          "archlinux", null};
        b32 failed;

        if (bowl_has(root, "/etc/pacman.d/gnupg/pubring.gpg") ||
            bowl_has(root, "/etc/pacman.d/gnupg/pubring.kbx"))
                return 0;

        string_format(log, bowl_label "initialising the keyring\n");
        log_flush();
        failed = bowl_setup_isolated(root, "/usr/bin/pacman-key", init_argv,
                                     "pacman-key --init failed\n");
        if (!failed)
                failed = bowl_setup_isolated(root, "/usr/bin/pacman-key",
                                             populate_argv,
                                             "pacman-key --populate failed\n");
        return failed;
}

static b32 bowl_setup_publish(string_address root, string_address program)
{
        p8 name[256];
        b32 failed;

        path_tail_copy(name, sizeof(name), program);
        failed = bowl_expose_program(root, program, null, false);
        if (!failed)
                failed = bowl_publish_bin(name);
        return failed;
}

static string_address bowl_arch_expose[] = {
    "/usr/bin/pacman", "/usr/bin/pacman-key", null};
static string_address bowl_alpine_expose[] = {"/sbin/apk", null};
static string_address bowl_debian_expose[] = {
    "/usr/bin/apt-get", "/usr/bin/apt", null};

/*
        The sizes are what each download and its unpacked tree took on a
        4 KiB-page tmpfs on 2026-09-15, with nothing added, so a setup that
        fits is never refused: Arch 126,491,574 bytes unpacking to 607,944,704,
        Alpine 3,698,422 to 8,675,328, Debian 49,337,828 to 130,928,640. A
        release that has grown since is what the check after the download is
        for, and a failure part way still says how much room is left.
*/
static const struct bowl_distro bowl_distros[] = {
    {"arch", "Arch", BOWL_ROOT_PREFIX "arch",
     BOWL_ROOT_PREFIX "archlinux-bootstrap-x86_64.tar.zst", BOWL_ARCH_URL,
     "/usr/bin/pacman", "pacman -Syu", null, (p64)32 * 1024 * 1024,
     126491574, 607944704, BOWL_PRIME_ARCH, bowl_arch_expose},
    {"alpine", "Alpine", BOWL_ROOT_PREFIX "alpine",
     BOWL_ROOT_PREFIX "alpine-minirootfs-x86_64.tar.gz", BOWL_ALPINE_URL, "/sbin/apk",
     "apk update", null, (p64)1024 * 1024, 3698422, 8675328, BOWL_PRIME_NONE,
     bowl_alpine_expose},
    {"debian", "Debian", BOWL_ROOT_PREFIX "debian", BOWL_ROOT_PREFIX "debian-rootfs-amd64.tar.gz",
     BOWL_DEBIAN_URL, "/usr/bin/apt-get", "apt-get update", null,
     (p64)8 * 1024 * 1024, 49337828, 130928640, BOWL_PRIME_NONE,
     bowl_debian_expose},
    {"fedora", "Fedora", BOWL_ROOT_PREFIX "fedora", null, null, "/usr/bin/dnf",
     null, "fedora is an OCI image, not a rootfs tarball\n", 0, 0, 0,
     BOWL_PRIME_NONE, null},
    {"nix", "Nix", BOWL_ROOT_PREFIX "nix", null, null, "/bin/nix", null,
     "nix is a /nix store, not a distro root\n", 0, 0, 0, BOWL_PRIME_NONE, null},
};

static const struct bowl_distro address_to bowl_find_distro(string_address name)
{
        positive at = string_table_find(name, bowl_distros,
                                        sizeof(bowl_distros[0]),
                                        array_count(bowl_distros));

        return at < array_count(bowl_distros) ? bowl_distros + at : null;
}

/*
        Refused before anything is written when the room is not there: the
        download and its tree are on the filesystem together until the download
        is removed. Which of the filesystem and memory is short is the one said.
*/
static b32 bowl_room_short(const struct bowl_distro address_to distro, p64 need)
{
        bowl_room room;

        if (!need || !bowl_room_at(BOWL_ROOT_DIRECTORY, address_of room))
                return 0;

        bool memory = room.in_memory && room.memory < room.free;
        p64 have = memory ? room.memory : room.free;

        if (have >= need)
                return 0;

        string_format(log, bowl_label "%s needs %p MiB in " BOWL_ROOT_DIRECTORY
                                      " and %s %p MiB %s\n",
                      distro->label,
                      (positive)((need + BOWL_MEBIBYTE - 1) / BOWL_MEBIBYTE),
                      memory ? "memory has" : "it has",
                      (positive)(have / BOWL_MEBIBYTE),
                      memory ? "available" : "free");
        bowl_room_hint(address_of room);
        log_flush();
        return 1;
}

static b32 bowl_setup_distro(const struct bowl_distro address_to distro)
{
        b32 failed = 0;
        string_address address_to program;

        failed = bowl_mkdir(BOWL_ROOT_DIRECTORY);
        if (failed < 0)
                return bowl_fail(BOWL_ROOT_DIRECTORY, failed);

        if (!bowl_has(distro->root, distro->marker))
        {
                bool kept = bowl_archive_usable(distro->store, distro->floor);

                if (bowl_room_short(distro, distro->tree_bytes +
                                                (kept ? 0 : distro->archive_bytes)))
                        return 1;

                if (!kept)
                {
                        system_remove_at(AT_FDCWD, distro->store, 0);
                        failed = bowl_setup_download(distro->store, distro->url,
                                                     distro->floor);
                        if (failed)
                                return failed;

                        // The download is in place now and its tree is not.
                        if (bowl_room_short(distro, distro->tree_bytes))
                        {
                                system_remove_at(AT_FDCWD, distro->store, 0);
                                return 1;
                        }
                }

                string_format(log, bowl_label "landing %s at %s\n",
                              distro->label, distro->root);
                log_flush();
                failed = bowl_land(distro->store, distro->root, distro->marker);
                if (failed)
                {
                        if (!bowl_has(distro->root, distro->marker))
                                system_remove_at(AT_FDCWD, distro->store, 0);
                        return failed;
                }

                system_remove_at(AT_FDCWD, distro->store, 0);
        }

        if (distro->prime == BOWL_PRIME_ARCH)
        {
                failed = bowl_prime_arch(distro->root);
                if (failed)
                        return failed;
        }

        program = distro->expose;
        while (program && *program)
        {
                failed = bowl_setup_publish(distro->root, *program);
                if (failed)
                        return failed;
                program++;
        }

        string_format(log, bowl_label "%s is ready. %s\n", distro->label,
                      distro->next);
        log_flush();
        return 0;
}

static b32 bowl_setup(positive count, string_address address_to arguments)
{
        const struct bowl_distro address_to distro;

        if (count != 3)
                return bowl_usage();

        distro = bowl_find_distro(arguments[2]);
        if (!distro)
                return bowl_refuse("known setups: arch alpine debian fedora nix\n");

        if (distro->refuse)
                return bowl_refuse(distro->refuse);

#ifndef X64
        string_format(log, bowl_label "%s setup is x86_64 for now\n",
                      distro->label);
        log_flush();
        return 1;
#endif

        if (bowl_setup_become_root(distro->name))
                return 1;

        return bowl_setup_distro(distro);
}


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
