/*
        Asking one of ours for an answer rather than for an effect.

        A tool writes to its standard output, which is fine when the point is
        that somebody reads it and no use at all when the build needs the
        number. The fork is what makes that safe: the tool runs in a child
        with its output on a pipe, so this address space never has two tools
        in it and the static arenas they keep stay theirs alone.

        Only for short answers. A tool that writes more than a pipe will hold
        before the parent reads would deadlock, and every caller here wants a
        word.
*/
static bipolar build_tool_capture(string_address address_to words,
                                  p8 address_to into, positive capacity)
{
        b32 pair[2];
        b32 child;
        positive used = 0;

        if (!capacity)
                return -1;

        into[0] = end;

        if (pipe(pair) < 0)
                return -1;

        log_flush();
        child = fork();

        if (child == 0)
        {
                close(pair[0]);
                dup2(pair[1], 1);
                close(pair[1]);
                exit(build_tool_words(words));
        }

        close(pair[1]);

        if (child < 0)
        {
                close(pair[0]);
                return -1;
        }

        while (used + 1 < capacity)
        {
                bipolar got = read(pair[0], into + used, capacity - used - 1);

                if (got <= 0)
                        break;

                used += (positive)got;
        }

        into[used] = end;
        close(pair[0]);
        build_wait(child);

        //      A tool's answer is a line; the caller wants the word on it.
        while (used && (into[used - 1] == '\n' || into[used - 1] == '\r'))
                into[--used] = end;

        return (bipolar)used;
}

static string_address build_tool_answer(string_address name, ...)
{
        string_address words[BUILD_ARGUMENT_ROOM];
        p8 address_to into = build_text_take(4096);
        positive count = 0;
        string_address piece = name;
        var_args rest;

        var_list(rest, name);

        while (piece && count + 1 < BUILD_ARGUMENT_ROOM)
        {
                words[count++] = piece;
                piece = var_list_get(rest, string_address);
        }

        var_list_end(rest);
        words[count] = null;

        if (build_tool_capture((string_address address_to)words, into, 4096) < 0)
                return "";

        return (string_address)into;
}

static positive build_processors()
{
        string_address answer = build_tool_answer("nproc", null);
        positive count = string_to_positive(answer);

        return count ? count : 1;
}

/*
        A key whose value is a command.

        pre and post carry shell source, not an argument vector -- a profile
        writes `#> post sh kernel/profile/post/rpi.sh` -- so this is the one
        place a shell is still the right thing to run. Nothing is passed to
        it but the text.
*/
static fn build_shell_key(string_address name)
{
        string_address value = build_key(name);

        if (!value || !*value)
                return;

        build_run("sh", "-c", value, null);
}

/*
        Installing a compiler that is not there.

        Checks which system this is and picks the command that installs
        things on it. Kept because the shell build had it and a first build on
        a fresh machine is where it earns its place.
*/
static bool build_install(string_address what)
{
        string_address words[BUILD_ARGUMENT_ROOM];
        string_address command = null;
        positive count = 0;
        positive at;

        if (build_is_file("/etc/debian_version"))
                command = "sudo apt-get install";
        else if (build_is_file("/etc/redhat-release"))
                command = "sudo yum install";
        else if (build_is_file("/etc/arch-release"))
                command = "sudo pacman -S";
        else if (build_is_file("/etc/alpine-release"))
                command = "sudo apk add";
        else if (build_is_file("/etc/SuSE-release"))
                command = "sudo zypper install";
        else if (build_is_file("/etc/gentoo-release"))
                command = "sudo emerge";
        else if (build_have("brew"))
                command = "brew install";
        else
        {
                string_format(log,
                              "Unknown distribution, unable to set up build environment.\n");
                log_flush();
                exit(1);
        }

        count = build_add_split((string_address address_to)words, 0,
                                BUILD_ARGUMENT_ROOM, command);
        words[count++] = what;
        words[count] = null;
        at = (positive)build_run_words((string_address address_to)words, null);

        return at == 0;
}
