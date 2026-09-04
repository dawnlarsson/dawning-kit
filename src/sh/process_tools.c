/*
        Small coreutils process wrappers.

        These are policy around facilities the combined shell already owns:
        file_take reads the leading options, file_exec_path_try performs the
        PATH/environment handoff, wait_status_code decodes the kernel's wait
        word, and the signal/clock/file fronts reach the kernel.  In
        particular, none of the three carries a private command launcher.
*/

// Common command handoff ------------------------------------------

static b32 process_tool_exec_environment(
    string_address program, string_address address_to words,
    string_address address_to environment, string_address path)
{
        log_flush();

        bipolar answer = file_exec_path_try_in(words[0], words, environment,
                                               path);

        string_format(file_fail, "%s: failed to run command '%s': %s\n",
                      program, words[0], file_reason(answer));
        return answer == -ERROR_NO_ENTRY ? 127 : 126;
}

static b32 process_tool_exec(string_address program,
                             string_address address_to words)
{
        return process_tool_exec_environment(
            program, words, file_environment_all(),
            file_environment((string_address) "PATH"));
}

// stdbuf ----------------------------------------------------------
/* The PATH query itself is shared with command/type.  Supplying the current
   process PATH explicitly is important here: a farm-linked utility dispatches
   before the resident shell has initialized its variable table. */
static b32 shell_find_in_path_mode(string_address name, p8 address_to into,
                                   positive room, positive access,
                                   bool use_hash, string_address value);

#define STDBUF_ELF_DYNAMIC 1
#define STDBUF_ELF_STATIC 2

static const file_long process_stdbuf_longs[] = {
    {(string_address) "input", 'i'},
    {(string_address) "output", 'o'},
    {(string_address) "error", 'e'},
    {null, 0},
};

static p8 stdbuf_input_assignment[48];
static p8 stdbuf_output_assignment[48];
static p8 stdbuf_error_assignment[48];
static p8 address_to stdbuf_preload_assignment;
static positive stdbuf_preload_room;
static p8 stdbuf_library[FILE_PATH_MAX];

static PURE positive stdbuf_u16(p8 address_to bytes)
{
        return (positive)bytes[0] | (positive)bytes[1] << 8;
}

static PURE positive stdbuf_u32(p8 address_to bytes)
{
        return (positive)bytes[0] | (positive)bytes[1] << 8 |
               (positive)bytes[2] << 16 | (positive)bytes[3] << 24;
}

static PURE p64 stdbuf_u64(p8 address_to bytes)
{
        return (p64)stdbuf_u32(bytes) | (p64)stdbuf_u32(bytes + 4) << 32;
}

static PURE positive stdbuf_elf_machine()
{
#if X64
        return 62;
#elif ARM64
        return 183;
#else
        return 243;
#endif
}

static PURE bool stdbuf_elf_native(p8 address_to bytes, positive length,
                                   bool shared)
{
        return length >= 64 && bytes[0] == 0x7f && bytes[1] == 'E' &&
               bytes[2] == 'L' && bytes[3] == 'F' && bytes[4] == 2 &&
               bytes[5] == 1 && stdbuf_u16(bytes + 18) == stdbuf_elf_machine() &&
               (!shared || stdbuf_u16(bytes + 16) == 3);
}

/* A candidate must be a native ELF shared object and must carry the three
   variables libstdbuf consumes.  This rejects a same-architecture executable
   renamed to libstdbuf.so instead of letting the loader complain and continue
   without applying the requested policy. */
static bool stdbuf_library_usable(string_address path)
{
        if (string_first_of(path, ':') || string_first_of(path, ' ') ||
            string_first_of(path, '\t'))
                return false;

        bipolar handle = system_open_at(AT_FDCWD, path,
                                        FILE_READ | O_CLOEXEC);

        if (handle < 0)
                return false;

        positive used = 0;

        while (used < sizeof(file_transfer))
        {
                bipolar got = system_read_retry(
                    (positive)handle, file_transfer + used,
                    sizeof(file_transfer) - used);

                if (got < 0)
                {
                        used = 0;
                        break;
                }
                if (!got)
                        break;

                used += (positive)got;
        }

        system_close(handle);

        if (!stdbuf_elf_native(file_transfer, used, true) ||
            !memory_search(file_transfer, used,
                           (address_any) "_STDBUF_I", 9) ||
            !memory_search(file_transfer, used,
                           (address_any) "_STDBUF_O", 9) ||
            !memory_search(file_transfer, used,
                           (address_any) "_STDBUF_E", 9))
                return false;

        positive length = string_length(path);

        if (length >= sizeof(stdbuf_library))
                return false;

        memory_copy_end(stdbuf_library, path, length);
        return true;
}

/* Search one filesystem root.  Debian installs in libexec, Arch and newer
   coreutils builds commonly use lib/coreutils, and multiarch Debian roots use
   the architecture component. */
static bool stdbuf_library_under(string_address root)
{
        static const string_address common[] = {
            "/usr/libexec/coreutils/libstdbuf.so",
            "/usr/lib/coreutils/libstdbuf.so",
            "/usr/lib64/coreutils/libstdbuf.so",
#if X64
            "/usr/lib/x86_64-linux-gnu/coreutils/libstdbuf.so",
#elif ARM64
            "/usr/lib/aarch64-linux-gnu/coreutils/libstdbuf.so",
#else
            "/usr/lib/riscv64-linux-gnu/coreutils/libstdbuf.so",
#endif
            null,
        };

        for (positive i = 0; common[i]; i++)
        {
                p8 path[FILE_PATH_MAX];
                string_address candidate = common[i];

                if (root)
                {
                        if (!file_path_join(path, root, candidate + 1))
                                continue;
                        candidate = path;
                }

                if (stdbuf_library_usable(candidate))
                        return true;
        }

        return false;
}

static bool stdbuf_bowl_root_from_text(string_address text, positive length,
                                       p8 address_to root)
{
        string_address prefix = (string_address) "/bowls/";
        p8 address_to found = (p8 address_to)memory_search(
            (address_any)text, length, (address_any)prefix, 7);

        if (!found)
                return false;

        positive used = 7;
        positive remaining = length - (positive)(found - text);

        while (used < remaining && found[used] && found[used] != '/' &&
               found[used] != '\n' && !byte_is_space(found[used]))
                used++;

        if (used == 7 || used >= FILE_PATH_MAX)
                return false;

        memory_copy_end(root, found, used);
        return true;
}

/* An exposed Bowl command is a #!/bowl launcher containing its real root.
   A directly named /bowls/NAME program already carries the same answer in
   its path. */
static bool stdbuf_bowl_root(string_address target, p8 address_to root)
{
        if (stdbuf_bowl_root_from_text(target, string_length(target), root) &&
            string_compare(root, (string_address) "/bowls/bin"))
                return true;

        bipolar handle = system_open_at(AT_FDCWD, target,
                                        FILE_READ | O_CLOEXEC);

        if (handle < 0)
                return false;

        bipolar got = system_read_retry((positive)handle, file_transfer, 256);
        system_close(handle);

        return got > 0 && stdbuf_bowl_root_from_text(
                              file_transfer, (positive)got, root) &&
               string_compare(root, (string_address) "/bowls/bin");
}

static bool stdbuf_find_library(string_address preferred_root)
{
        string_address forced =
            file_environment((string_address) "MOONWATER_STDBUF_LIBRARY");

        if (forced)
                return stdbuf_library_usable(forced);

        if (preferred_root && stdbuf_library_under(preferred_root))
                return true;
        if (stdbuf_library_under(null))
                return true;

        file_walk walk;

        if (!file_walk_open(address_of walk, AT_FDCWD,
                            (string_address) "/bowls"))
                return false;

        struct linux_dirent64 address_to entry;
        bool found = false;

        while (!found && (entry = file_walk_next(address_of walk)))
        {
                if (file_is_dot(entry->d_name) ||
                    !string_compare(entry->d_name, (string_address) "bin"))
                        continue;

                p8 root[FILE_PATH_MAX];

                if (file_path_join(root, (string_address) "/bowls",
                                   entry->d_name))
                        found = stdbuf_library_under(root);
        }

        file_walk_close(address_of walk);
        return found;
}

/* A native ELF without PT_INTERP is statically linked, so LD_PRELOAD cannot
   possibly apply the requested buffering.  Scripts are left to the kernel;
   their interpreter receives the preload in the ordinary way. */
static b32 stdbuf_target_kind(string_address path)
{
        bipolar handle = system_open_at(AT_FDCWD, path,
                                        FILE_READ | O_CLOEXEC);

        if (handle < 0)
                return 0;

        bipolar got = system_read_retry((positive)handle, file_transfer,
                                        FILE_BLOCK);

        if (got < 64 || !stdbuf_elf_native(file_transfer, (positive)got,
                                           false))
        {
                system_close(handle);
                return 0;
        }

        p64 phoff = stdbuf_u64(file_transfer + 32);
        positive entry_size = stdbuf_u16(file_transfer + 54);
        positive entries = stdbuf_u16(file_transfer + 56);

        if (entry_size < 4 || !entries ||
            entries > FILE_BLOCK / entry_size ||
            phoff > positive_max - entry_size * entries)
        {
                system_close(handle);
                return STDBUF_ELF_STATIC;
        }

        positive bytes = entry_size * entries;
        p8 address_to table = file_transfer;

        if (phoff + bytes > (positive)got)
        {
                bipolar read = system_call_4(
                    syscall(pread64), (positive)handle,
                    (positive)file_transfer, bytes, (positive)phoff);

                if (read != (bipolar)bytes)
                {
                        system_close(handle);
                        return STDBUF_ELF_STATIC;
                }
        }
        else
                table += (positive)phoff;

        b32 answer = STDBUF_ELF_STATIC;

        for (positive i = 0; i < entries; i++)
                if (stdbuf_u32(table + i * entry_size) == 3)
                {
                        answer = STDBUF_ELF_DYNAMIC;
                        break;
                }

        system_close(handle);
        return answer;
}

static bool stdbuf_mode(string_address text, bool input,
                        p8 letter, p8 address_to assignment)
{
        memory_copy_apart(assignment, (address_any) "_STDBUF_I=", 10);
        assignment[8] = letter;

        if (string_is(text, 'L') && !string_get(text + 1))
        {
                if (input)
                {
                        file_fail("stdbuf: line buffering stdin is meaningless\n",
                                  0);
                        return false;
                }

                assignment[10] = 'L';
                assignment[11] = end;
                return true;
        }

        string_address at = text;

        if (string_is(at, '+'))
                at++;

        positive value;

        if (!string_digits_checked(address_of at, 10, address_of value))
                goto invalid;

        positive power = 0;
        positive base = 1024;
        p8 suffix = string_get(at);

        if (suffix)
        {
                power = suffix == 'k' ? 1
                                      : file_size_power(suffix, false);

                if (!power || (suffix >= 'a' && suffix != 'k'))
                        goto invalid;

                at++;

                if (string_is(at, 'B') && !string_get(at + 1))
                {
                        base = 1000;
                        at++;
                }
                else if (string_is(at, 'i') && string_is(at + 1, 'B') &&
                         !string_get(at + 2))
                        at += 2;
                else if (string_get(at))
                        goto invalid;
        }

        if (string_get(at))
                goto invalid;

        if (value)
                while (power--)
                {
                        if (value > positive_max / base)
                                goto invalid;
                        value *= base;
                }

        positive_into_string(assignment + 10, value);
        return true;

invalid:
        string_format(file_fail, "stdbuf: invalid mode '%s'\n", text);
        return false;
}

static bool stdbuf_preload(string_address library)
{
        string_address before =
            file_environment((string_address) "LD_PRELOAD");
        positive old_length = before ? string_length(before) : 0;
        positive library_length = string_length(library);
        positive wanted = 11 + old_length + (before ? 1 : 0) +
                          library_length + 1;

        if (!shell_room((address_any address_to)address_of stdbuf_preload_assignment,
                        address_of stdbuf_preload_room, wanted, 1))
                return false;

        memory_copy_apart(stdbuf_preload_assignment,
                          (address_any) "LD_PRELOAD=", 11);
        positive used = 11;

        if (before)
        {
                memory_copy_apart(stdbuf_preload_assignment + used, before,
                                  old_length);
                used += old_length;
                stdbuf_preload_assignment[used++] = ':';
        }

        memory_copy_end(stdbuf_preload_assignment + used, library,
                        library_length);
        return true;
}

static b32 process_stdbuf()
{
        file_taking taking = {
            .program = (string_address) "stdbuf",
            .allowed = (string_address) "ioe",
            .valued = (string_address) "ioe",
            .longs = process_stdbuf_longs,
        };

        if (!file_take(address_of taking))
                return 125;

        positive modes = taking.flags &
                         (FILE_FLAG('i') | FILE_FLAG('o') | FILE_FLAG('e'));
        positive count = (positive)program_argument_count();

        if (!modes)
        {
                file_fail("stdbuf: you must specify a buffering mode option\n",
                          0);
                return 125;
        }
        if (taking.first >= count)
        {
                file_fail("stdbuf: missing operand\n", 0);
                return 125;
        }

        bool input = (modes & FILE_FLAG('i')) == 0 ||
                     stdbuf_mode(file_option_value(address_of taking, 'i'),
                                 true, 'I', stdbuf_input_assignment);
        bool output = (modes & FILE_FLAG('o')) == 0 ||
                      stdbuf_mode(file_option_value(address_of taking, 'o'),
                                  false, 'O', stdbuf_output_assignment);
        bool error = (modes & FILE_FLAG('e')) == 0 ||
                     stdbuf_mode(file_option_value(address_of taking, 'e'),
                                 false, 'E', stdbuf_error_assignment);

        if (!input || !output || !error)
                return 125;

        string_address address_to words =
            program_argument_list() + taking.first;
        p8 target[FILE_PATH_MAX];
        p8 bowl_root[FILE_PATH_MAX];
        string_address path = file_environment((string_address) "PATH");

        if (!path)
                path = (string_address) "/bin:/usr/bin:/bowls/bin:/";

        bool target_found = shell_find_in_path_mode(
            words[0], target, sizeof(target), 1, false, path);
        bool has_bowl_root = target_found &&
                             stdbuf_bowl_root(target, bowl_root);
        string_address forced = file_environment(
            (string_address) "MOONWATER_STDBUF_LIBRARY");

        if (!stdbuf_find_library(has_bowl_root ? bowl_root : null))
        {
                if (forced)
                        string_format(file_fail,
                                      "stdbuf: '%s' is not a compatible libstdbuf.so\n",
                                      forced);
                else
                        file_fail("stdbuf: no compatible libstdbuf.so found in the native or Bowl roots\n",
                                  0);
                return 125;
        }

        if (target_found && stdbuf_target_kind(target) == STDBUF_ELF_STATIC)
        {
                string_format(file_fail,
                              "stdbuf: '%s' is statically linked; preload buffering cannot apply\n",
                              words[0]);
                return 125;
        }

        if (!stdbuf_preload(stdbuf_library))
        {
                file_fail("stdbuf: environment is too large\n", 0);
                return 125;
        }

        env_have = 0;
        string_address address_to inherited = file_environment_all();

        for (positive i = 0; inherited && inherited[i]; i++)
                if (!env_put(inherited[i]))
                        return 125;

        env_drop((string_address) "MOONWATER_STDBUF_LIBRARY");

        if (((modes & FILE_FLAG('i')) &&
             !env_put(stdbuf_input_assignment)) ||
            ((modes & FILE_FLAG('o')) &&
             !env_put(stdbuf_output_assignment)) ||
            ((modes & FILE_FLAG('e')) &&
             !env_put(stdbuf_error_assignment)) ||
            !env_put(stdbuf_preload_assignment))
                return 125;

        env_list[env_have] = null;
        path = string_get_environment(env_list, (string_address) "PATH");

        return process_tool_exec_environment((string_address) "stdbuf", words,
                                             env_list, path);
}

// chroot ----------------------------------------------------------

static const file_long process_chroot_longs[] = {
    {(string_address) "skip-chdir", 'k'},
    {null, 0},
};

static b32 process_chroot()
{
        file_taking taking = {
            .program = (string_address) "chroot",
            .allowed = (string_address) "",
            .longs = process_chroot_longs,
        };
        positive count = (positive)program_argument_count();

        if (!file_take(address_of taking))
                return 125;
        if (taking.first >= count)
        {
                file_fail("chroot: missing operand\n", 0);
                return 125;
        }

        string_address root = program_argument((b32)taking.first++);

        /* --skip-chdir is safe only when the requested root is the root we
           already have.  Compare inode/device identity instead of accepting
           one spelling of "/" while rejecting another. */
        if (taking.flags & FILE_FLAG('k'))
        {
                file_facts requested;
                file_facts current;

                if (!file_look_at(root, address_of requested) ||
                    !file_look_at((string_address) "/", address_of current) ||
                    !file_same_identity(address_of requested,
                                        address_of current))
                {
                        file_fail("chroot: option --skip-chdir only permitted if NEWROOT is old '/'\n",
                                  0);
                        return 125;
                }
        }

        bipolar changed = system_call_1(syscall(chroot), (positive)root);

        if (changed < 0)
        {
                string_format(file_fail, "chroot: cannot change root directory to '%s': %s\n",
                              root, file_reason(changed));
                return 125;
        }

        if (!(taking.flags & FILE_FLAG('k')) &&
            (changed = system_change_directory((string_address) "/")) < 0)
        {
                string_format(file_fail, "chroot: cannot chdir to root directory: %s\n",
                              file_reason(changed));
                return 125;
        }

        if (taking.first < count)
                return process_tool_exec((string_address) "chroot",
                    program_argument_list() + taking.first);

        string_address shell = file_environment((string_address) "SHELL");
        string_address words[3];

        if (!shell || !string_get(shell))
                shell = (string_address) "/bin/sh";

        words[0] = shell;
        words[1] = (string_address) "-i";
        words[2] = null;
        return process_tool_exec((string_address) "chroot", words);
}

// nohup -----------------------------------------------------------

static bool process_nohup_duplicate(bipolar from, b32 to,
                                    string_address what)
{
        if (from == to)
                return true;

        bipolar answer = system_duplicate((b32)from, to, 0);

        if (answer < 0)
        {
                string_format(file_fail, "nohup: cannot redirect %s: %s\n",
                              what, file_reason(answer));
                return false;
        }

        return true;
}

static bipolar process_nohup_output(p8 address_to path)
{
        bipolar answer = system_open_at_mode(
            AT_FDCWD, (string_address) "nohup.out", FILE_APPEND | O_CLOEXEC,
            0600);

        if (answer >= 0)
        {
                string_copy(path, (string_address) "nohup.out");
                return answer;
        }

        string_address home = file_environment((string_address) "HOME");
        positive home_length = home ? string_length(home) : 0;

        if (!home_length || home_length >= FILE_PATH_MAX - 11)
                return answer;

        path_join(path, FILE_PATH_MAX, home, (string_address) "nohup.out");
        return system_open_at_mode(AT_FDCWD, path, FILE_APPEND | O_CLOEXEC,
                                   0600);
}

static b32 process_nohup()
{
        file_taking taking = {
            .program = (string_address) "nohup",
            .allowed = (string_address) "",
        };
        positive count = (positive)program_argument_count();

        if (!file_take(address_of taking))
                return 125;
        if (taking.first >= count)
        {
                file_fail("nohup: missing operand\n", 0);
                return 125;
        }

        bool input_terminal = stream_is_terminal(0);
        bool output_terminal = stream_is_terminal(1);
        bool error_terminal = stream_is_terminal(2);
        bipolar null_input = -1;
        bipolar output = -1;
        p8 output_path[FILE_PATH_MAX];

        if (input_terminal)
        {
                null_input = system_open_at(AT_FDCWD,
                    (string_address) "/dev/null", FILE_READ | O_CLOEXEC);

                if (null_input < 0)
                {
                        string_format(file_fail, "nohup: failed to open '/dev/null': %s\n",
                                      file_reason(null_input));
                        return 125;
                }
        }

        if (output_terminal)
        {
                output = process_nohup_output(output_path);

                if (output < 0)
                {
                        if (null_input >= 0)
                                system_close(null_input);
                        string_format(file_fail, "nohup: failed to open 'nohup.out': %s\n",
                                      file_reason(output));
                        return 125;
                }
        }

        if (input_terminal && output_terminal)
                string_format(file_fail,
                    "nohup: ignoring input and appending output to '%s'\n",
                    output_path);
        else if (input_terminal)
                file_fail("nohup: ignoring input\n", 0);
        else if (output_terminal)
                string_format(file_fail, "nohup: appending output to '%s'\n",
                              output_path);

        /* Flush the explanation to the original error stream before that
           descriptor is made to follow the command's output. */
        log_flush();

        if (null_input >= 0)
        {
                if (!process_nohup_duplicate(null_input, 0,
                                              (string_address) "standard input"))
                {
                        system_close(null_input);
                        if (output >= 0)
                                system_close(output);
                        return 125;
                }
                if (null_input != 0)
                        system_close(null_input);
        }

        if (output >= 0)
        {
                if (!process_nohup_duplicate(output, 1,
                                              (string_address) "standard output"))
                {
                        system_close(output);
                        return 125;
                }
                if (output != 1)
                        system_close(output);
        }

        if (error_terminal &&
            !process_nohup_duplicate(1, 2, (string_address) "standard error"))
                return 125;

        /* Ignored dispositions survive exec; that is precisely nohup's one
           signal operation. */
        if (!system_signal_install(SIGHUP, SIGNAL_IGNORE, 0, 0, null))
        {
                file_fail("nohup: cannot ignore hangup signal\n", 0);
                return 125;
        }

        return process_tool_exec((string_address) "nohup",
            program_argument_list() + taking.first);
}

// timeout ---------------------------------------------------------

typedef struct
{
        b32 descriptor;
        b16 events;
        b16 returned;
} process_timeout_poll;

static const file_long process_timeout_longs[] = {
    {(string_address) "foreground", 'f'},
    {(string_address) "kill-after", 'k'},
    {(string_address) "preserve-status", 'p'},
    {(string_address) "signal", 's'},
    {(string_address) "verbose", 'v'},
    {null, 0},
};

/* HUP, INT, QUIT and TERM are relayed to the command. SIGCHLD shares the
   signalfd only so an old kernel without pidfd support still has an event to
   wake an unlimited wait. One mask operation and one descriptor replace a
   handler installation per signal. */
#define PROCESS_TIMEOUT_SIGNALS                                             \
        (((positive)1 << (SIGHUP - 1)) |                                    \
         ((positive)1 << (SIGINT - 1)) |                                    \
         ((positive)1 << (SIGQUIT - 1)) |                                   \
         ((positive)1 << (SIGTERM - 1)) |                                   \
         ((positive)1 << (SIGCHLD - 1)))

/* util-linux's duration reader is already the exact, overflow-checked
   decimal/scientific seconds grammar used by flock and waitpid.  Coreutils
   adds only four unit suffixes around that same grammar. */
static bool process_timeout_duration(string_address text,
                                     positive address_to nanoseconds)
{
        positive length = string_length(text);
        positive scale = 1;
        p8 number[128];

        if (!length || length >= sizeof(number))
                return false;

        p8 suffix = string_get(text + length - 1);

        if (suffix == 's' || suffix == 'm' || suffix == 'h' || suffix == 'd')
        {
                scale = suffix == 'm' ? 60
                      : suffix == 'h' ? 60 * 60
                      : suffix == 'd' ? 24 * 60 * 60
                                      : 1;
                length--;
        }

        if (!length)
                return false;

        memory_copy(number, text, length);
        number[length] = end;

        positive made;

        if (!ul_duration((string_address)number, address_of made) ||
            made > positive_max / scale)
                return false;

        address_to nanoseconds = made * scale;
        return true;
}

/* Wait for one child until a monotonic deadline. pidfd+ppoll is the native
   steady-state path: no handler, alarm signal, tick loop, or PID reuse race.
   The WNOHANG loop exists only for kernels predating pidfd_open. */
static bipolar process_timeout_wait(b32 child, bipolar pidfd, bipolar signal_fd,
                                    positive deadline,
                                    positive address_to status,
                                    b32 address_to forwarded)
{
        if (pidfd >= 0 || signal_fd >= 0)
        {
                process_timeout_poll waited[2];
                positive descriptors = 0;
                positive pid_index = positive_max;
                positive signal_index = positive_max;

                if (pidfd >= 0)
                {
                        pid_index = descriptors;
                        waited[descriptors++] =
                            (process_timeout_poll){(b32)pidfd, 1, 0};
                }
                if (signal_fd >= 0)
                {
                        signal_index = descriptors;
                        waited[descriptors++] =
                            (process_timeout_poll){(b32)signal_fd, 1, 0};
                }

                for (;;)
                {
                        timespec span;
                        timespec address_to limit = null;

                        if (deadline)
                        {
                                positive now = clock_monotonic_nanoseconds();

                                if (now >= deadline)
                                        return 0;

                                positive left = deadline - now;
                                span = (timespec){left / 1000000000,
                                                  left % 1000000000};
                                limit = address_of span;
                        }

                        for (positive i = 0; i < descriptors; i++)
                                waited[i].returned = 0;
                        bipolar ready = system_call_5(
                            syscall(ppoll), (positive)waited, descriptors,
                            (positive)limit, 0, 8);

                        if (!ready)
                                return 0;
                        if (ready < 0)
                        {
                                if (ready == UL_ERROR_INTERRUPTED)
                                        continue;
                                return -1;
                        }

                        if (pid_index != positive_max &&
                            waited[pid_index].returned)
                                return system_wait4_retry(child, status, 0,
                                                          null) < 0 ? -1 : 1;

                        if (signal_index != positive_max &&
                            waited[signal_index].returned)
                        {
                                positive information[16];
                                bipolar got = system_read_retry(
                                    (positive)signal_fd, information,
                                    sizeof(information));

                                if (got < (bipolar)sizeof(p32))
                                        return got < 0 ? -1 : 0;

                                b32 number = (b32)(p32)information[0];

                                if (number != SIGCHLD)
                                {
                                        address_to forwarded = number;
                                        return 2;
                                }

                                /* pidfd and SIGCHLD can become ready in
                                   either order. After consuming SIGCHLD, the
                                   next poll observes the pidfd; without one,
                                   the WNOHANG check below reaps it. */
                                if (pidfd >= 0)
                                        continue;

                                bipolar reaped = system_wait4_retry(
                                    child, status, 1, null);

                                if (reaped == child)
                                        return 1;
                                if (reaped < 0)
                                        return -1;
                        }
                }
        }

        if (!deadline)
                return system_wait4_retry(child, status, 0, null) < 0 ? -1 : 1;

        for (;;)
        {
                bipolar waited = system_wait4_retry(child, status, 1, null);

                if (waited == child)
                        return 1;
                if (waited < 0)
                        return -1;

                positive now = clock_monotonic_nanoseconds();

                if (now >= deadline)
                        return 0;

                positive left = deadline - now;
                positive nap = left < 10000000 ? left : 10000000;
                timespec span = {nap / 1000000000, nap % 1000000000};

                system_call_2(syscall(nanosleep), (positive)address_of span,
                              0);
        }
}

static fn process_timeout_cleanup(bipolar pidfd, bipolar signal_fd,
                                  positive previous_mask)
{
        if (pidfd >= 0)
                system_close(pidfd);
        if (signal_fd >= 0)
                system_close(signal_fd);

        system_signal_mask(UL_SIGNAL_SET_MASK, address_of previous_mask, null,
                           8);
}

static fn process_timeout_signal(b32 child, b32 signal, bool foreground,
                                 bool verbose, string_address command)
{
        if (verbose)
        {
                p8 name[16];

                kill_name((positive)signal, name);
                string_format(file_fail,
                              "timeout: sending signal %s to command '%s'\n",
                              name, command);
                log_flush();
        }

        system_call_2(syscall(kill),
                      (positive)(foreground ? child : -child),
                      (positive)signal);
}

static b32 process_timeout()
{
        file_taking taking = {
            .program = (string_address) "timeout",
            .allowed = (string_address) "ksv",
            .valued = (string_address) "ks",
            .longs = process_timeout_longs,
        };
        positive count = (positive)program_argument_count();

        if (!file_take(address_of taking))
                return 125;
        if (taking.first >= count)
        {
                file_fail("timeout: missing operand\n", 0);
                return 125;
        }

        positive duration;

        if (!process_timeout_duration(program_argument((b32)taking.first++),
                                      address_of duration))
        {
                file_fail("timeout: invalid time interval\n", 0);
                return 125;
        }
        if (taking.first >= count)
        {
                file_fail("timeout: missing command\n", 0);
                return 125;
        }

        b32 signal = SIGTERM;
        string_address signal_text = file_option_value(address_of taking, 's');

        if (signal_text)
        {
                bipolar named = ul_signal_number(signal_text);

                if (named <= 0 || named > SIGNAL_HIGHEST)
                {
                        string_format(file_fail, "timeout: invalid signal '%s'\n",
                                      signal_text);
                        return 125;
                }
                signal = (b32)named;
        }

        bool escalate = (taking.flags & FILE_FLAG('k')) != 0;
        positive kill_after = 0;

        if (escalate &&
            !process_timeout_duration(file_option_value(address_of taking, 'k'),
                                      address_of kill_after))
        {
                file_fail("timeout: invalid time interval for --kill-after\n", 0);
                return 125;
        }

        bool foreground = (taking.flags & FILE_FLAG('f')) != 0;
        bool preserve = (taking.flags & FILE_FLAG('p')) != 0;
        bool verbose = (taking.flags & FILE_FLAG('v')) != 0;
        positive status = 0;
        positive blocked = PROCESS_TIMEOUT_SIGNALS;
        positive previous_mask = 0;

        if (system_signal_mask(UL_SIGNAL_BLOCK, address_of blocked,
                               address_of previous_mask, 8) < 0)
        {
                file_fail("timeout: cannot block relay signals\n", 0);
                return 125;
        }

        log_flush();
        bipolar child = system_fork();

        if (child < 0)
        {
                system_signal_mask(UL_SIGNAL_SET_MASK,
                                   address_of previous_mask, null, 8);
                string_format(file_fail, "timeout: cannot fork: %s\n",
                              file_reason(child));
                return 125;
        }

        if (child == 0)
        {
                system_signal_mask(UL_SIGNAL_SET_MASK,
                                   address_of previous_mask, null, 8);

                if (!foreground)
                        system_call_2(syscall(setpgid), 0, 0);

                b32 answer = process_tool_exec((string_address) "timeout",
                    program_argument_list() + taking.first);
                exit(answer);
        }

        if (!foreground)
                system_call_2(syscall(setpgid), (positive)child,
                              (positive)child);

        bipolar pidfd = system_call_2(syscall(pidfd_open), (positive)child, 0);
        bipolar signal_fd = system_call_4(
            syscall(signalfd4), (positive)(bipolar)-1,
            (positive)address_of blocked, 8, O_CLOEXEC);

        /* A kernel old enough to lack signalfd must not leave the signals
           blocked. pidfd still gives it the fast child wait; externally
           delivered signals then retain their inherited disposition. */
        if (signal_fd < 0)
                system_signal_mask(UL_SIGNAL_SET_MASK,
                                   address_of previous_mask, null, 8);

        positive deadline = 0;

        if (duration)
        {
                positive now = clock_monotonic_nanoseconds();
                deadline = duration > positive_max - now
                               ? positive_max : now + duration;
        }

        b32 forwarded = 0;
        bipolar waited;

        do
        {
                waited = process_timeout_wait((b32)child, pidfd, signal_fd,
                                               deadline, address_of status,
                                               address_of forwarded);

                if (waited == 2)
                {
                        process_timeout_signal((b32)child, forwarded,
                                               foreground, verbose,
                                               program_argument(
                                                   (b32)taking.first));
                        forwarded = 0;
                }
        } while (waited == 2);

        if (waited < 0)
        {
                process_timeout_cleanup(pidfd, signal_fd, previous_mask);
                file_fail("timeout: failure while waiting for command\n", 0);
                return 125;
        }

        if (waited > 0)
        {
                process_timeout_cleanup(pidfd, signal_fd, previous_mask);
                return wait_status_code(status);
        }

        string_address command = program_argument((b32)taking.first);

        process_timeout_signal((b32)child, signal, foreground, verbose,
                               command);

        bool killed = signal == SIGKILL;

        if (escalate && !killed)
        {
                positive now = clock_monotonic_nanoseconds();
                positive kill_deadline = kill_after > positive_max - now
                                             ? positive_max
                                             : now + kill_after;

                do
                {
                        waited = process_timeout_wait(
                            (b32)child, pidfd, signal_fd, kill_deadline,
                            address_of status, address_of forwarded);

                        if (waited == 2)
                        {
                                process_timeout_signal((b32)child, forwarded,
                                                       foreground, verbose,
                                                       command);
                                forwarded = 0;
                        }
                } while (waited == 2);

                if (!waited)
                {
                        process_timeout_signal((b32)child, SIGKILL,
                                               foreground, verbose, command);
                        killed = true;
                }
                else if (waited < 0)
                {
                        process_timeout_cleanup(pidfd, signal_fd,
                                                previous_mask);
                        file_fail("timeout: failure while waiting for command\n",
                                  0);
                        return 125;
                }
        }

        if (waited <= 0)
        {
                if (system_wait4_retry((b32)child, address_of status, 0,
                                       null) < 0)
                {
                        process_timeout_cleanup(pidfd, signal_fd,
                                                previous_mask);
                        return 125;
                }
        }

        process_timeout_cleanup(pidfd, signal_fd, previous_mask);

        if (killed)
                return 128 + SIGKILL;
        if (preserve)
                return wait_status_code(status);
        return 124;
}
