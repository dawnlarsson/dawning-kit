/*
        Small coreutils process wrappers.

        These are policy around facilities the combined shell already owns:
        file_take reads the leading options, file_exec_path_try performs the
        PATH/environment handoff, wait_status_code decodes the kernel's wait
        word, and the signal/clock/file fronts reach the kernel.  In
        particular, none of the three carries a private command launcher.
*/

// Common command handoff ------------------------------------------

static b32 process_tool_exec(string_address program,
                             string_address address_to words)
{
        log_flush();

        bipolar answer = file_exec_path_try(words);

        string_format(file_fail, "%s: failed to run command '%s': %s\n",
                      program, words[0], file_reason(answer));
        return answer == -ERROR_NO_ENTRY ? 127 : 126;
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
