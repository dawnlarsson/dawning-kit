/*
        Running something with a working directory, an environment or a
        muzzle.

        The shell build reached for a subshell whenever it needed one of the
        three -- `( cd linux && make ... )`, `env $make_flags sh ...`,
        `> /dev/null`. Each was a process whose only job was to change one
        thing about the next one. Here they are fields.
*/
typedef struct build_command
{
        string_address address_to words;
        string_address directory;
        string_address address_to environment;
        bool quiet;
        bool privileged;
} build_command;

static bool build_root()
{
        return geteuid() == 0;
}

static b32 build_execute(build_command address_to what)
{
        string_address raised[BUILD_ARGUMENT_ROOM];
        string_address address_to words = what->words;
        string_address path;
        b32 child;

        //      sudo only when we are not already it. Under the documented
        //      invocation this program is root and the prefix is a no-op; run
        //      as somebody else, it asks, which is what the shell did.
        if (what->privileged && !build_root())
        {
                positive count = 0;

                raised[count++] = "sudo";

                while (words[count - 1] && count + 1 < BUILD_ARGUMENT_ROOM)
                {
                        raised[count] = words[count - 1];
                        count++;
                }

                raised[count] = null;
                words = (string_address address_to)raised;
        }

        path = build_resolve(words[0]);

        if (!path)
        {
                string_format(log_error, "build: %s not found\n", words[0]);
                log_flush();
                return -1;
        }

        if (string_get_environment(environ, "BUILD_TRACE"))
        {
                string_format(log, "+ %s", path);

                for (positive at = 1; words[at]; at++)
                        string_format(log, " %s", words[at]);

                string_format(log, "\n");
        }

        log_flush();
        child = fork();

        if (child == 0)
        {
                if (what->directory && chdir(what->directory) < 0)
                        exit(126);

                if (what->quiet)
                {
                        b32 sink = open("/dev/null", O_WRONLY, 0);

                        if (sink >= 0)
                        {
                                dup2(sink, 1);
                                close(sink);
                        }
                }

                execve(path, words,
                       what->environment ? what->environment : environ);
                exit(127);
        }

        return build_wait(child);
}

//      environ with a few more entries on the end, for the two places the
//      shell wrote `env NAME=value command`.
static string_address address_to build_environment_with(string_address address_to extra,
                                                        positive count)
{
        positive have = 0;
        string_address address_to answer;

        while (environ[have])
                have++;

        answer = (string_address address_to)build_text_take(
                (have + count + 1) * sizeof(string_address));

        for (positive at = 0; at < have; at++)
                answer[at] = environ[at];

        for (positive at = 0; at < count; at++)
                answer[have + at] = extra[at];

        answer[have + count] = null;

        return answer;
}

/*
        The tool registry as a file rather than as this program's own table.

        src/sh/tools.inc is the compiled dispatch registry and the installed
        surface both, including the category each name belongs to. This
        program has the table linked in, but filtered by its own build's
        component macros and without the categories, so the image's surface is
        read from the source of truth instead.
*/
typedef struct build_tool_entry
{
        string_address category;
        string_address name;
} build_tool_entry;

#define BUILD_TOOL_ROOM 512

static build_tool_entry build_tool_table[BUILD_TOOL_ROOM];
static positive build_tool_count;

static bool build_tools_read()
{
        build_lines walk;
        p8 address_to store;

        if (build_tool_count)
                return true;

        if (build_slurp(build_setting_get("tool_registry"), build_file_two,
                        BUILD_FILE_ROOM) < 0)
                return false;

        store = build_text_take(BUILD_WORD_ROOM);
        build_lines_open(address_of walk, (string_address)build_file_two);

        while (build_lines_next(address_of walk) &&
               build_tool_count < BUILD_TOOL_ROOM)
        {
                string_address words[BUILD_ARGUMENT_ROOM];
                positive parts;
                positive at = 0;
                positive length = walk.length;
                p8 address_to flat = build_text_take(length + 1);

                //      The shell split on "[(),[:space:]]+", so the separators
                //      are turned into blanks and the ordinary word splitter
                //      does the rest.
                for (positive which = 0; which < length; which++)
                {
                        p8 byte = walk.line[which];

                        flat[which] = (byte == '(' || byte == ')' || byte == ',')
                                              ? ' '
                                              : byte;
                }

                flat[length] = end;
                parts = build_words_of((string_address)flat, length,
                                       (string_address address_to)words,
                                       BUILD_ARGUMENT_ROOM, store,
                                       BUILD_WORD_ROOM);

                if (parts < 3 || !word_is(words[0], "SHELL_TOOL"))
                        continue;

                build_tool_table[build_tool_count].category =
                        build_join(words[1], null);
                build_tool_table[build_tool_count].name = build_join(words[2], null);
                build_tool_count++;
                (void)at;
        }

        return true;
}

/*
        The build.

        Everything below is build.sh's local path, step for step and label for
        label. Where it ran a utility, this calls ours; where it ran the
        toolchain, make, tar or QEMU, this spawns those.
*/
static bool build_moon_core;
static bool build_moon_shell;
static bool build_moon_utilities;
static bool build_moon_util_linux;
static bool build_moon_shell_monitor;

//      An existing build tree has no lines for newly added symbols until
//      olddefconfig next runs; their Kconfig defaults are y, while the core
//      must be explicitly built in for the initial filesystem to use it.
static fn build_components(string_address config)
{
        build_lines walk;

        build_moon_core = false;
        build_moon_shell = true;
        build_moon_utilities = true;
        build_moon_util_linux = true;
        build_moon_shell_monitor = true;

        if (build_slurp(config, build_file_two, BUILD_FILE_ROOM) < 0)
                return;

        build_lines_open(address_of walk, (string_address)build_file_two);

        while (build_lines_next(address_of walk))
        {
                string_address name = null;
                positive length = 0;
                bool on = false;

                if (walk.length > 18 &&
                    !memory_compare(walk.line, "CONFIG_MOONWATER_", 17) &&
                    walk.line[walk.length - 2] == '=' &&
                    walk.line[walk.length - 1] == 'y')
                {
                        name = walk.line + 17;
                        length = walk.length - 17 - 2;
                        on = true;
                }
                else if (walk.length > 30 &&
                         !memory_compare(walk.line, "# CONFIG_MOONWATER_", 19) &&
                         !memory_compare(walk.line + walk.length - 12,
                                         " is not set", 11))
                {
                        name = walk.line + 19;
                        length = walk.length - 19 - 11;
                        on = false;
                }

                if (!name)
                        continue;

                if (length == 4 && !memory_compare(name, "CORE", 4))
                        build_moon_core = on;
                else if (length == 5 && !memory_compare(name, "SHELL", 5))
                        build_moon_shell = on;
                else if (length == 9 && !memory_compare(name, "UTILITIES", 9))
                        build_moon_utilities = on;
                else if (length == 10 && !memory_compare(name, "UTIL_LINUX", 10))
                        build_moon_util_linux = on;
                else if (length == 13 && !memory_compare(name, "SHELL_MONITOR", 13))
                        build_moon_shell_monitor = on;
        }
}

static bool build_category_installed(string_address category)
{
        if (word_is(category, "GENERAL"))
                return true;

        if (word_is(category, "MONITOR"))
                return build_moon_shell_monitor;

        if (word_is(category, "UTIL_BIN") || word_is(category, "UTIL_SBIN"))
                return build_moon_util_linux;

        return false;
}

static b32 build_link(string_address target, string_address path)
{
        return build_tool("ln", "-sf", target, path, null);
}

static b32 build_kernel_source()
{
        string_address artifacts = build_setting_get("artifacts");
        string_address tree = build_setting_get("kernel_tree");
        string_address version = build_setting_get("kernel_version");
        string_address series;
        string_address archive;
        string_address tarball;
        string_address download;
        string_address required = build_setting_get("required");
        string_address names[BUILD_ARGUMENT_ROOM];
        p8 address_to store = build_text_take(BUILD_WORD_ROOM);
        positive count = build_words_of(required, string_length(required),
                                        (string_address address_to)names,
                                        BUILD_ARGUMENT_ROOM, store,
                                        BUILD_WORD_ROOM);
        bool present;

        //      Derived rather than written out, so moving to another release
        //      means editing the version and the signature and nothing else.
        //      kernel.org lays every series out under vMAJOR.x.
        {
                positive major = 0;

                while (version[major] && version[major] != '.')
                        major++;

                series = build_join("v", build_join(null, null), null);
                {
                        p8 address_to into = build_text_take(major + 4);

                        into[0] = 'v';
                        memory_copy(into + 1, version, major);
                        into[major + 1] = '.';
                        into[major + 2] = 'x';
                        into[major + 3] = end;
                        series = (string_address)into;
                }
        }

        tarball = build_join(artifacts, "/linux-", version, ".tar", null);
        archive = build_join(tarball, ".xz", null);
        download = build_join(build_setting_get("kernel_mirror"), "/", series,
                              "/linux-", version, ".tar.xz", null);

        //      Checked here rather than after the early exit below, which is
        //      where it used to sit -- so it never ran on any build after the
        //      first.
        for (positive at = 0; at < count; at++)
                if (!build_have(names[at]))
                {
                        string_format(log_error,
                                      "%s is required to build the kernel. Please install it and try again.\n",
                                      names[at]);
                        log_flush();
                        exit(1);
                }

        //      A Makefile is the marker that the tree is really there. Testing
        //      only for the directory treated an empty or half extracted tree
        //      as a finished extraction.
        present = build_is_file(build_join(tree, "/Makefile", null));

        if (present)
        {
                string_format(log, "%s Kernel already extracted... %s\n",
                              BUILD_BOLD, BUILD_RESET);
                log_flush();
        }

        build_tool("mkdir", "-p", artifacts, null);
        build_tool("mkdir", "-p", tree, null);

        if (present)
                return 0;

        if (!build_is_file(archive))
        {
                //      curl, and not our own fetch: fetch speaks HTTP without
                //      TLS and does not follow redirects, and this URL is
                //      https and redirects. The signature below is what makes
                //      the download trustworthy either way, but a fetch that
                //      cannot reach the mirror at all is not a substitute.
                if (build_run("curl", "-fL", download, "-o", archive, null))
                {
                        string_format(log_error, "ERROR: failed to download %s\n",
                                      download);
                        log_flush();
                        build_tool("rm", "-f", archive, null);
                        exit(1);
                }
        }

        string_format(log, "%s Checking kernel signature %s\n", BUILD_BOLD,
                      BUILD_RESET);
        log_flush();

        /*
                Every step below gates the next one. None of these exit
                statuses were checked before, so a failed download, a failed
                key fetch or a failed signature verification all still ended in
                a compiled kernel.
        */
        {
                string_address keys = build_setting_get("kernel_keys");
                string_address named[BUILD_ARGUMENT_ROOM];
                p8 address_to holder = build_text_take(BUILD_WORD_ROOM);
                positive many = build_words_of(keys, string_length(keys),
                                               (string_address address_to)named,
                                               BUILD_ARGUMENT_ROOM, holder,
                                               BUILD_WORD_ROOM);
                string_address words[BUILD_ARGUMENT_ROOM];
                positive at = 0;

                words[at++] = "gpg";
                words[at++] = "--locate-keys";

                for (positive which = 0; which < many; which++)
                        words[at++] = named[which];

                words[at] = null;

                if (build_run_words((string_address address_to)words, null))
                {
                        string_format(log_error,
                                      BUILD_RED
                                      "ERROR: could not fetch the kernel signing keys (%s).\n",
                                      keys);
                        string_format(log_error,
                                      "Refusing to build an unverified kernel." BUILD_RESET "\n");
                        log_flush();
                        exit(1);
                }
        }

        if (build_run("unxz", "-k", archive, null))
        {
                string_format(log_error,
                              BUILD_RED "ERROR: could not decompress %s" BUILD_RESET "\n",
                              archive);
                log_flush();
                exit(1);
        }

        {
                string_address signature = build_setting_get("kernel_signature");

                build_write_file(build_join(tarball, ".sign", null), signature,
                                 string_length(signature));
        }

        if (build_run("gpg", "--verify", build_join(tarball, ".sign", null),
                      tarball, null))
        {
                string_format(log_error,
                              BUILD_RED "ERROR: SIGNATURE VERIFICATION FAILED for %s\n",
                              tarball);
                string_format(log_error,
                              "The archive does not match the signature pinned in this tool.\n");
                string_format(log_error,
                              "Refusing to extract or build it. Delete %s and retry."
                              BUILD_RESET "\n", archive);
                log_flush();
                build_tool("rm", "-f", tarball, null);
                exit(1);
        }

        if (build_run("tar", "-xf", tarball, "--strip-components=1", "-C", tree,
                      null))
        {
                string_format(log_error,
                              BUILD_RED "ERROR: could not extract %s" BUILD_RESET "\n",
                              tarball);
                log_flush();
                build_tool("rm", "-f", tarball, null);
                exit(1);
        }

        build_tool("rm", tarball, null);
        string_format(log, "%s Kernel extracted to %s %s\n", BUILD_BOLD, tree,
                      BUILD_RESET);
        log_flush();

        return 0;
}

static b32 build_userspace()
{
        string_address image = build_setting_get("image_root");
        string_address applet = null;
        string_address flags = build_join(null, null);

        /*
                What was here last time, gone.

                Nothing ever removed a build product from the image tree, so
                anything that stopped being built stayed in it forever: an 857
                kilobyte binary from a fortnight ago was still shipping, along
                with every program that had since become a name for the shell.
                Only the top level and only files and links -- the directories
                below hold the device nodes and are made once.
        */
        {
                string_address where[4];

                where[0] = image;
                where[1] = build_join(image, "/bin", null);
                where[2] = build_join(image, "/sbin", null);
                where[3] = build_join(image, "/usr", null);

                for (positive at = 0; at < 4; at++)
                        if (build_tool("find", where[at], "-maxdepth", "1", "(",
                                       "-type", "f", "-o", "-type", "l", ")",
                                       "-delete", null))
                                return build_die(build_join("clearing ",
                                                            where[at], null));
        }

        build_components(build_join(build_setting_get("kernel_tree"), "/.config",
                                    null));

        if (!build_tools_read())
                return build_die("cannot read the tool registry");

        if (build_moon_core && build_moon_shell)
        {
                //      Every program in the default image is spark, including
                //      the one the kernel execs as /init, so no ELF is loaded
                //      on its boot path.
                if (!build_moon_utilities)
                        flags = build_join(flags, " -DSHELL_NO_UTILITIES", null);

                if (!build_moon_util_linux)
                        flags = build_join(flags, " -DSHELL_NO_UTIL_LINUX", null);

                if (!build_moon_shell_monitor)
                        flags = build_join(flags, " -DSHELL_NO_MONITOR", null);

                build_setting_set("spark_cppflags", flags);

                if (build_spark(build_setting_get("shell_source"),
                                build_join(image, "/shell", null), null))
                        return build_die("building the shell");

                applet = "shell";

                //      Scripts need a real interpreter path: /shell is the
                //      image's binary, but a #!/bin/sh shebang is resolved by
                //      the kernel before the shell gets any say.
                {
                        string_address names[3];

                        names[0] = "sh";
                        names[1] = "dash";
                        names[2] = "bash";

                        for (positive at = 0; at < 3; at++)
                                if (build_link("../shell",
                                               build_join(image, "/bin/", names[at],
                                                          null)))
                                        return build_die(build_join("linking /bin/",
                                                                    names[at], null));
                }

                if (build_link("../bin", build_join(image, "/usr/bin", null)))
                        return build_die("linking /usr/bin");

                if (build_link("../sbin", build_join(image, "/usr/sbin", null)))
                        return build_die("linking /usr/sbin");

                for (positive at = 0; at < build_tool_count; at++)
                        if (word_is(build_tool_table[at].category, "SYSTEM") &&
                            build_link("shell",
                                       build_join(image, "/",
                                                  build_tool_table[at].name, null)))
                                return build_die(build_join("linking ",
                                                            build_tool_table[at].name,
                                                            null));

                if (build_moon_shell_monitor && build_moon_utilities)
                {
                        string_address monitor = build_setting_get("monitor_source");

                        if (build_tool("cp", monitor,
                                       build_join(image, "/monitor.sh", null),
                                       null))
                                return build_die("installing /monitor.sh");

                        if (build_tool("chmod", "0755",
                                       build_join(image, "/monitor.sh", null),
                                       null))
                                return build_die("making /monitor.sh executable");

                        if (build_link("monitor.sh",
                                       build_join(image, "/mointor.sh", null)) ||
                            build_link("../monitor.sh",
                                       build_join(image, "/bin/monitor.sh", null)) ||
                            build_link("../monitor.sh",
                                       build_join(image, "/bin/mointor.sh", null)))
                                return build_die("linking the monitor");
                }
        }
        else if (build_moon_core && build_moon_utilities)
        {
                flags = build_join(flags, " -DSHELL_NO_MONITOR", null);

                if (!build_moon_util_linux)
                        flags = build_join(flags, " -DSHELL_NO_UTIL_LINUX", null);

                build_setting_set("spark_cppflags", flags);

                if (build_spark(build_setting_get("utilities_source"),
                                build_join(image, "/shell", null), null))
                        return build_die("building the utilities");

                //      The kernel's SPAWN_TOOL ABI accelerates through this
                //      fixed path. This binary has no shell fallback.
                applet = "shell";

                if (build_link("../bin", build_join(image, "/usr/bin", null)))
                        return build_die("linking /usr/bin");

                if (build_link("../sbin", build_join(image, "/usr/sbin", null)))
                        return build_die("linking /usr/sbin");
        }

        /*
                Utilities are one multicall Spark program under other names.

                With the shell present they share its binary. A utility-only
                image has the same dispatch table but no shell fallback.
        */
        if (build_moon_core && build_moon_utilities)
        {
                for (positive at = 0; at < build_tool_count; at++)
                {
                        build_tool_entry address_to one = address_of build_tool_table[at];

                        if (!build_category_installed(one->category))
                                continue;

                        if (build_link(applet,
                                       build_join(image, "/", one->name, null)))
                                return build_die(build_join("linking ", one->name,
                                                            null));
                }

                //      Conventional paths for absolute commands and
                //      /usr/bin/env shebangs. The registry selects enabled
                //      categories; every alias shares the same binary.
                for (positive at = 0; at < build_tool_count; at++)
                {
                        build_tool_entry address_to one = address_of build_tool_table[at];
                        string_address directory = null;

                        if (word_is(one->category, "GENERAL"))
                                directory = "bin";
                        else if (build_moon_util_linux &&
                                 word_is(one->category, "UTIL_BIN"))
                                directory = "bin";
                        else if (build_moon_util_linux &&
                                 word_is(one->category, "UTIL_SBIN"))
                                directory = "sbin";

                        if (!directory)
                                continue;

                        if (build_link(build_join("../", applet, null),
                                       build_join(image, "/", directory, "/",
                                                  one->name, null)))
                                return build_die(build_join("linking /", directory,
                                                            "/", one->name, null));
                }
        }

        return 0;
}

static b32 build_local(string_address address_to profiles, positive count)
{
        string_address artifacts = build_setting_get("artifacts");
        string_address image = build_setting_get("image_root");
        string_address tree = build_setting_get("kernel_tree");
        string_address output = build_setting_get("output");
        string_address make_flags;
        string_address compiler;
        string_address kernel_image;
        string_address kernel_export;
        string_address chosen[BUILD_ARGUMENT_ROOM];
        positive chosen_count = 0;

        //      Only this path needs it. Booting an image, writing a stick and
        //      driving a build on another machine all run as you.
        if (!build_root())
        {
                build_label("", BUILD_YELLOW " WARNING !!!");
                string_format(log_error,
                              "Building here wants root: sudo sh build.sh\n");
                log_flush();
                string_format(log, "\n");
                log_flush();
        }

        {
                string_address system = build_join(null, null);
                string_address words[3];

                words[0] = "uname";
                words[1] = null;
                build_capture_words((string_address address_to)words,
                                    build_file_two, BUILD_FILE_ROOM);
                system = (string_address)build_file_two;

                if (memory_compare(system, "Linux", 5))
                        return build_die(
                                "building a kernel wants a Linux toolchain and a case\n"
                                "sensitive filesystem. Name a machine that has them with\n"
                                "--host, or set MOONWATER_BUILD_HOST.");
        }

        build_label("", "REPOSITORY SETUP");
        string_format(log, "Building %s\n", build_setting_get("full_name"));
        log_flush();

        if (build_tool("mkdir", "-p", artifacts, image, output, null))
                return build_die("repository setup");

        build_label("", "DISTRO INFO");

        {
                string_address text = build_join(
                        "CONFIG_LOCALVERSION=\"", build_setting_get("full_name"),
                        "\"\nCONFIG_DEFAULT_HOSTNAME=\"",
                        build_setting_get("name"), "-box\"\n", null);

                build_write_file(build_join(artifacts, "/info", null), text,
                                 string_length(text));
        }

        /*
                The device nodes the image boots with. mknod fails when the
                node already exists, which made every rebuild after the first
                noisy and, under set -e, fatal -- so each is created only when
                missing.

                tmp, etc and root because everything expects them to be there:
                a redirection into /tmp is the first thing anybody tries. The
                empty runtime directories are mount targets for Bowl's fast
                merged view; /bowls is where distribution roots live.
        */
        {
                string_address directories = build_setting_get("image_directories");
                string_address names[BUILD_ARGUMENT_ROOM];
                p8 address_to store = build_text_take(BUILD_WORD_ROOM);
                positive many = build_words_of(directories,
                                               string_length(directories),
                                               (string_address address_to)names,
                                               BUILD_ARGUMENT_ROOM, store,
                                               BUILD_WORD_ROOM);
                string_address words[BUILD_ARGUMENT_ROOM];
                positive at = 0;

                words[at++] = "mkdir";
                words[at++] = "-p";

                for (positive which = 0; which < many; which++)
                        words[at++] = build_join(image, "/", names[which], null);

                words[at] = null;

                if (build_tool_words((string_address address_to)words))
                        return build_die("filesystem setup");
        }

        {
                string_address nodes = build_setting_get("image_nodes");
                string_address names[BUILD_ARGUMENT_ROOM];
                p8 address_to store = build_text_take(BUILD_WORD_ROOM);
                positive many = build_words_of(nodes, string_length(nodes),
                                               (string_address address_to)names,
                                               BUILD_ARGUMENT_ROOM, store,
                                               BUILD_WORD_ROOM);

                for (positive at = 0; at + 3 < many + 1; at += 4)
                {
                        string_address path = build_join(image, "/", names[at],
                                                         null);

                        if (build_is_file(path) || build_is_directory(path) ||
                            access(path, 0) >= 0)
                                continue;

                        if (build_tool("mknod", path, names[at + 1], names[at + 2],
                                       names[at + 3], null))
                                return build_die(build_join("making ", path, null));
                }
        }

        build_label("", "KERNEL SOURCE");

        if (build_kernel_source())
                return 1;

        build_label("", "KERNEL CONFIGURATION");

        {
                string_address always = build_setting_get("profiles_always");
                string_address names[BUILD_ARGUMENT_ROOM];
                p8 address_to store = build_text_take(BUILD_WORD_ROOM);
                positive many = build_words_of(always, string_length(always),
                                               (string_address address_to)names,
                                               BUILD_ARGUMENT_ROOM, store,
                                               BUILD_WORD_ROOM);

                for (positive at = 0; at < many; at++)
                        chosen[chosen_count++] = names[at];

                if (!count)
                {
                        /*
                                serial is last on purpose, and it is not
                                optional. The boot lane of test/run drives the
                                image over a serial line and reads its answers
                                back, so a default image without the 8250 has
                                no way to be tested at all: the kernel comes
                                up, runs /init, and says nothing. Last, because
                                it also asks for the loglevel and the
                                timestamps that make the transcript readable,
                                and debug_none quietens both.
                        */
                        string_address preset = build_setting_get("profiles_default");
                        p8 address_to holder = build_text_take(BUILD_WORD_ROOM);
                        positive some = build_words_of(preset,
                                                       string_length(preset),
                                                       (string_address address_to)names,
                                                       BUILD_ARGUMENT_ROOM,
                                                       holder, BUILD_WORD_ROOM);

                        for (positive at = 0; at < some; at++)
                                chosen[chosen_count++] = names[at];
                }
                else
                        for (positive at = 0; at < count; at++)
                                chosen[chosen_count++] = profiles[at];

                chosen[chosen_count] = null;
        }

        /*
                Always compose the selected profiles. Reusing the last
                configuration made a plain default build inherit whichever
                special profile had run before it -- notably leaving Canvas
                disabled after a server build even though a bare build promises
                the defaults. Composition preserves incremental builds when the
                result is unchanged, so deterministic selection costs no
                rebuild by itself.
        */
        if (build_config((string_address address_to)chosen, chosen_count))
                return build_die("configuration");

        make_flags = build_key("make_flags");

        build_label("", "BUILD ENVIRONMENT CHECK");
        compiler = build_key("compiler");

        if (!build_have(compiler))
        {
                build_label("", BUILD_YELLOW " WARNING !!!");
                string_format(log, "%s not found. Attempting to install it.\n\n",
                              compiler);
                log_flush();

                if (!build_install(compiler))
                        string_format(log,
                                      "ERROR: Unable to install %s. Please install it manually.\n",
                                      compiler);
        }
        else
        {
                string_format(log, "Using compiler: %s%s\n", BUILD_BOLD, compiler);
                log_flush();
        }

        build_label("", "KERNEL CONFIG");

        /*
                Every edit made to Linux's own source lives in the patch
                script, because everything that touches the kernel belongs
                under kernel/ and nothing else does. What was once here was a
                hundred and eighty lines of claims, displacements and grafts,
                which is the answer to "what did you change about the kernel"
                and was findable only by reading a build script.
        */
        if (build_run("sh", build_setting_get("patch_script"), null))
                return build_die("patching the kernel source");

        if (build_is_newer(build_join(artifacts, "/.config", null),
                           build_join(tree, "/.config", null)))
        {
                string_address extra[BUILD_ARGUMENT_ROOM];
                p8 address_to store = build_text_take(BUILD_WORD_ROOM);
                positive many = build_words_of(make_flags,
                                               string_length(make_flags),
                                               (string_address address_to)extra,
                                               BUILD_ARGUMENT_ROOM, store,
                                               BUILD_WORD_ROOM);
                string_address words[BUILD_ARGUMENT_ROOM];
                build_command what = {null, tree, null, true, true};
                positive at;

                at = 0;
                words[at++] = "make";
                words[at++] = "allnoconfig";

                for (positive which = 0; which < many; which++)
                        words[at++] = extra[which];

                words[at] = null;
                what.words = (string_address address_to)words;

                if (build_execute(address_of what))
                        return build_die("kernel configuration");

                /*
                        Quiet: merge_config compares the combined fragment
                        against an allnoconfig baseline and calls most of what
                        the profiles ask for a "redefinition" -- 125 lines
                        meaning nothing. The composition step reports the
                        disagreements that matter, between profiles.

                        make_flags go in the environment, not on the command
                        line: merge_config.sh reads trailing arguments as
                        fragment paths, so "ARCH=arm64" became a file that did
                        not exist and stopped every cross build.
                */
                at = 0;
                words[at++] = "sh";
                words[at++] = "scripts/kconfig/merge_config.sh";
                words[at++] = "-m";
                words[at++] = ".config";
                words[at++] = build_join("../", artifacts, "/.config", null);
                words[at] = null;

                what.words = (string_address address_to)words;
                what.privileged = false;
                what.environment = build_environment_with(
                        (string_address address_to)extra, many);

                if (build_execute(address_of what))
                        return build_die("kernel configuration");

                at = 0;
                words[at++] = "make";
                words[at++] = "olddefconfig";

                for (positive which = 0; which < many; which++)
                        words[at++] = extra[which];

                words[at] = null;
                what.words = (string_address address_to)words;
                what.privileged = true;
                what.environment = null;

                if (build_execute(address_of what))
                        return build_die("kernel configuration");
        }
        else
        {
                string_format(log, "No changes\n");
                log_flush();
        }

        build_label("", "CONFIGURATION CHECK");

        /*
                merge_config and olddefconfig drop unmet options without a
                word, so anything a profile asked for and did not get is
                reported here rather than discovered later as hardware that
                does not work.

                Called with no profiles, which is what the shell did: the
                variable it expanded here was never assigned, so this has
                always reported on an empty list and said "All 0 requested
                options took effect." That is preserved rather than fixed,
                because fixing it changes what every build prints and belongs
                in a change that is about this check rather than about who
                runs it. `build verify-config <config> <profile ...>` is the
                same code with the list supplied.
        */
        build_verify_config(build_join(tree, "/.config", null), null, 0);

        build_label("", "ASSEMBLY");

        //      Where a profile asked for a .asm from src/ to stand in for a
        //      file the kernel already builds. The .asm files that belong to
        //      the module rather than to the kernel need nothing here -- the
        //      module's Makefile builds those as part of it.
        {
                string_address words[3];
                build_command what = {null, null, null, false, true};

                words[0] = "sh";
                words[1] = build_setting_get("replace_script");
                words[2] = null;
                what.words = (string_address address_to)words;

                if (build_execute(address_of what))
                        return build_die("assembly");
        }

        build_label("", "PRE BUILD");
        build_shell_key("pre");

        build_label("", "USER SPACE BUILD");

        if (build_userspace())
                return 1;

        build_label("", "KERNEL BUILD");

        make_flags = build_key("make_flags");

        //      The kernel used to be built with whatever its own Makefile
        //      chose, because nothing reached the C compiler. KCFLAGS is that
        //      gap closed.
        {
                string_address cores;
                string_address kernel_cflags = build_key("kernel_cflags");
                string_address extra[BUILD_ARGUMENT_ROOM];
                p8 address_to store = build_text_take(BUILD_WORD_ROOM);
                positive many = build_words_of(make_flags,
                                               string_length(make_flags),
                                               (string_address address_to)extra,
                                               BUILD_ARGUMENT_ROOM, store,
                                               BUILD_WORD_ROOM);
                string_address words[BUILD_ARGUMENT_ROOM];
                build_command what = {null, tree, null, false, false};
                positive at = 0;
                bool good = true;

                cores = build_number(build_processors());
                kernel_image = build_key_one("kernel_image", address_of good);
                kernel_export = build_key_one("kernel_export", address_of good);

                if (!good || !*kernel_image || !*kernel_export)
                        return build_die(
                                "kernel_image / kernel_export not set in the configuration");

                words[at++] = "make";
                words[at++] = build_join("-j", cores, null);

                for (positive which = 0; which < many; which++)
                        words[at++] = extra[which];

                words[at++] = build_join("KCFLAGS=", kernel_cflags, null);
                words[at] = null;
                what.words = (string_address address_to)words;

                /*
                        make's exit status was discarded once, so a failed
                        build fell through to the copy below and shipped
                        whatever image was left over from the run before.

                        KCPPFLAGS, KAFLAGS, LDFLAGS and RUSTFLAGS were passed
                        here too, from keys no profile has ever set -- four
                        empty variables handed to make on every build.
                */
                if (build_execute(address_of what))
                        return build_die("kernel build");
        }

        if (!build_is_file(kernel_image))
                return build_die(build_join("expected image '", kernel_image,
                                            "' was not produced", null));

        build_tool("mkdir", "-p", build_directory_of(kernel_export), null);

        {
                string_address words[4];
                build_command what = {null, null, null, false, true};

                words[0] = "cp";
                words[1] = kernel_image;
                words[2] = kernel_export;
                words[3] = null;
                what.words = (string_address address_to)words;

                //      Our cp, unless we are not root, in which case the copy
                //      into a root-owned directory needs sudo and sudo needs a
                //      program to run.
                if (build_root())
                {
                        if (build_tool("cp", kernel_image, kernel_export, null))
                                return build_die("exporting the kernel image");
                }
                else if (build_execute(address_of what))
                        return build_die("exporting the kernel image");
        }

        build_label("", "POST BUILD");
        build_shell_key("post");
        string_format(log, "%sDone Building Kernel%s\n", BUILD_BOLD, BUILD_GREEN);
        log_flush();
        build_size(build_key("kernel_export"));
        string_format(log, "%s\n", BUILD_RESET);
        log_flush();

        return 0;
}
