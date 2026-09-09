//      SIGTERM. The shell's header names the three signals it traps and this
//      is not one of them, so it is spelled here rather than borrowed.
#define BUILD_SIGNAL_TERMINATE 15

//      getcwd has no wrapper in the standard layer, and the only caller is
//      the default output name, which is the working directory's own.
static string_address build_working_directory()
{
        p8 address_to into = build_text_take(4096);
        bipolar got = (bipolar)system_call_2(syscall(getcwd), (positive)into,
                                             4096);

        if (got <= 0)
                return ".";

        into[got ? got - 1 : 0] = end;

        return (string_address)into;
}

static string_address build_directory_of(string_address path)
{
        string_address copy = build_join(path, null);
        p8 address_to cut = (p8 address_to)string_last_of(copy, '/');

        if (!cut)
                return ".";

        address_to cut = end;

        return copy[0] ? copy : (string_address)"/";
}

static string_address build_name_of(string_address path)
{
        string_address cut = string_last_of(path, '/');

        return cut ? cut + 1 : path;
}

/*
        Building one freestanding binary, and optionally running it.

        This is the ordinary static link, not the spark one: it takes link
        time optimisation, which the spark path must not, because -flto
        discards the section layout that linker script depends on.
*/
static string_address build_whole_program_flags(string_address compiler)
{
        string_address words[4];

        words[0] = compiler;
        words[1] = "--version";
        words[2] = null;

        if (build_capture_words((string_address address_to)words, build_file_two,
                                BUILD_FILE_ROOM) < 0)
                return "";

        {
                positive length = string_length((string_address)build_file_two);

                //      clang first: it answers "clang version" and also names
                //      GCC nowhere, while gcc's banner says gcc and GCC both.
                if (memory_search(build_file_two, length, "clang", 5) ||
                    memory_search(build_file_two, length, "Clang", 5))
                        return "";

                if (memory_search(build_file_two, length, "gcc", 3) ||
                    memory_search(build_file_two, length, "GCC", 3))
                        return build_setting_get("whole_program_flags");
        }

        return "";
}

static b32 build_freestanding_link(string_address source, string_address output,
                                   bool loud)
{
        string_address compiler = build_compiler();
        string_address words[BUILD_ARGUMENT_ROOM];
        positive count = 0;

        build_tool("mkdir", "-p", build_directory_of(output), null);

        words[count++] = compiler;
        words[count++] = source;
        words[count++] = "-o";
        words[count++] = output;
        count = build_add_split((string_address address_to)words, count,
                                BUILD_ARGUMENT_ROOM,
                                build_setting_get("freestanding_flags"));
        count = build_add_split((string_address address_to)words, count,
                                BUILD_ARGUMENT_ROOM,
                                build_whole_program_flags(compiler));
        count = build_add_split((string_address address_to)words, count,
                                BUILD_ARGUMENT_ROOM,
                                build_setting_get("freestanding_flags_tail"));
        words[count++] = build_join("-Wl,-e,", build_setting_get("entry"), null);
        words[count] = null;

        if (build_run_words((string_address address_to)words, null))
        {
                string_format(log_error, "build: compilation failed\n");
                log_flush();
                return 1;
        }

        build_tool("chmod", "+x", output, null);

        if (loud)
                build_size(output);

        return 0;
}

/*
        Watching.

        The pid of what was started is tracked rather than matched by name.
        Matching on the output path used to catch anything whose command line
        merely contained it -- including the watcher and this program.
*/
static b32 build_watch_application;

static fn build_stop_application()
{
        if (build_watch_application <= 0)
                return;

        kill(build_watch_application, BUILD_SIGNAL_TERMINATE);
        build_wait(build_watch_application);
        build_watch_application = 0;
}

static b32 build_freestanding(string_address address_to arguments, positive count)
{
        string_address source = null;
        string_address output = null;
        bool loud = false;
        bool run = false;
        bool watch = false;
        bool options = true;
        positive positional = 0;

        for (positive at = 0; at < count; at++)
        {
                string_address word = arguments[at];

                if (options)
                {
                        if (word_is(word, "-v"))
                        {
                                loud = true;
                                continue;
                        }

                        if (word_is(word, "--run"))
                        {
                                run = true;
                                continue;
                        }

                        if (word_is(word, "--watch"))
                        {
                                run = true;
                                watch = true;
                                continue;
                        }

                        if (word_is(word, "--"))
                        {
                                options = false;
                                continue;
                        }

                        if (word[0] == '-' && word[1] == '-')
                        {
                                string_format(log_error,
                                              "build: unknown option %s\n", word);
                                log_flush();
                                return 1;
                        }
                }

                if (positional == 0)
                        source = word;
                else if (positional == 1)
                        output = word;
                else
                {
                        string_format(log_error, "build: too many paths\n");
                        log_flush();
                        return 1;
                }

                positional++;
        }

        if (!source)
                source = build_setting_get("freestanding_source");

        if (!output)
                output = build_join(build_setting_get("freestanding_output"), "/",
                                    build_name_of(build_working_directory()),
                                    null);

        //      The compiler accepts a bare output filename; executing it must
        //      still refer to this directory rather than searching PATH for a
        //      different program.
        if (output[0] != '/' &&
            !(output[0] == '.' && (output[1] == '/' ||
                                   (output[1] == '.' && output[2] == '/'))))
                output = build_join("./", output, null);

        if (!build_is_file(source))
        {
                string_format(log_error, "build: no such source file: %s\n",
                              source);
                log_flush();
                return 1;
        }

        if (!watch)
        {
                b32 answer = build_freestanding_link(source, output, loud);

                if (answer || !run)
                        return answer;

                answer = build_run(output, null);

                if (answer)
                {
                        string_format(log, "Exited with %p\n", (positive)answer);
                        log_flush();
                }

                return answer;
        }

        {
                string_address watcher;
                string_address directory = build_directory_of(source);
                string_address words[10];
                b32 pair[2];
                b32 child;
                positive at = 0;

                if (build_have("inotifywait"))
                        watcher = "inotifywait";
                else if (build_have("fswatch"))
                        watcher = "fswatch";
                else
                {
                        string_format(log_error,
                                      "build: --watch needs inotifywait (inotify-tools) or fswatch\n");
                        log_flush();
                        return 1;
                }

                if (pipe(pair) < 0)
                        return 1;

                words[at++] = watcher;

                if (word_is(watcher, "inotifywait"))
                {
                        words[at++] = "-q";
                        words[at++] = "-m";
                        words[at++] = "-r";
                        words[at++] = "-e";
                        words[at++] = "modify,create,delete,move";
                }
                else
                        words[at++] = "-r";

                words[at++] = directory;
                words[at] = null;

                log_flush();
                child = fork();

                if (child == 0)
                {
                        string_address found = build_resolve(words[0]);

                        close(pair[0]);
                        dup2(pair[1], 1);
                        close(pair[1]);

                        if (found)
                                execve(found, (string_address address_to)words,
                                       environ);

                        exit(127);
                }

                close(pair[1]);

                while (true)
                {
                        p8 byte = 0;
                        bipolar got;

                        build_stop_application();
                        string_format(log, "\033[H\033[2J");
                        log_flush();

                        if (!build_freestanding_link(source, output, loud))
                        {
                                string_address only[2];

                                only[0] = output;
                                only[1] = null;
                                build_watch_application =
                                        build_spawn((string_address address_to)only,
                                                    null);
                        }
                        else
                        {
                                string_format(log_error,
                                              "build: waiting for the next change\n");
                                log_flush();
                        }

                        //      One line of the watcher's output is one change.
                        //      A byte at a time, because a buffered read can
                        //      hold two events and rebuild once for both.
                        do
                                got = read(pair[0], address_of byte, 1);
                        while (got == 1 && byte != '\n');

                        if (got <= 0)
                                break;
                }

                build_stop_application();

                if (child > 0)
                {
                        kill(child, BUILD_SIGNAL_TERMINATE);
                        build_wait(child);
                }

                close(pair[0]);
        }

        return 0;
}

/*
        The ISA floor, proved rather than asserted.

        Compile the library at the floor it advertises and read the ISA
        attribute back out of the object. A normal toolchain defaults to
        something richer -- rv64gc, say -- which would let both a compressed
        instruction and an extension above the floor enter unnoticed. The
        explicit march makes the assembler reject those; the attribute check
        proves a driver default did not put them back.

        This is the piece nothing else does, and it is why it belongs in the
        tool rather than in a test lane: what must be present and what must be
        absent are settings, so another tree points them at its own floor and
        gets the same proof. It answers 2 when nothing here has that back end,
        which is a skip and not a pass -- a check that manufactures a pass when
        it cannot run is worse than no check.

        An extension is present when the ISA string carries it as its own
        component: gcc writes rv64i2p1_m2p0_a2p1_f2p2_d2p2_zicsr2p0_zicntr2p0,
        so the components are separated by underscores and each is a name
        followed by its version. The first carries the rvNN base in front of
        it. Reading it this way rather than by substring is what keeps "a"
        from matching the "a" inside "zicsr" -- and keeps "c" from matching
        the one in "zicntr", which is the check that matters most here.
*/
static bool build_isa_holds(string_address isa, string_address name)
{
        positive at = 0;
        positive length = string_length(isa);
        positive want = string_length(name);

        //      Past the rvNN that opens the string; everything after is
        //      components separated by underscores.
        if (length > 2 && isa[0] == 'r' && isa[1] == 'v')
        {
                at = 2;

                while (at < length && isa[at] >= '0' && isa[at] <= '9')
                        at++;
        }

        while (at < length)
        {
                positive letters = 0;

                while (at + letters < length &&
                       ((isa[at + letters] >= 'a' && isa[at + letters] <= 'z') ||
                        (isa[at + letters] >= 'A' && isa[at + letters] <= 'Z')))
                        letters++;

                if (letters == want && !memory_compare(isa + at, name, want))
                        return true;

                //      On to the next underscore, or the end.
                while (at < length && isa[at] != '_')
                        at++;

                while (at < length && isa[at] == '_')
                        at++;
        }

        return false;
}

static b32 build_floor(string_address arch)
{
        string_address source = build_setting_get("floor_source");
        string_address march = build_setting_get("floor_march");
        string_address mabi = build_setting_get("floor_mabi");
        string_address require = build_setting_get("floor_require");
        string_address forbid = build_setting_get("floor_forbid");
        string_address prefix = build_setting_get("floor_prefix");
        string_address compiler = null;
        string_address reader = null;
        string_address target = null;
        string_address work;
        string_address object;
        string_address words[BUILD_ARGUMENT_ROOM];
        string_address attributes = null;
        positive count = 0;
        b32 answer = 0;

        if (!arch || !*arch)
                arch = build_setting_get("floor_arch");

        {
                string_address named = build_join(arch, "-linux-gnu-gcc", null);

                if (build_have(named))
                {
                        compiler = named;
                        reader = build_join(arch, "-linux-gnu-readelf", null);
                }
        }

        work = build_temporary_directory("floor");

        if (!work)
                return build_die("floor: cannot make a working directory");

        object = build_join(work, "/floor.o", null);

        if (!compiler)
        {
                //      Apple clang does not carry every back end. Try the
                //      clang on PATH, then the two usual package manager
                //      locations; an empty translation unit separates an
                //      unavailable target from a real failure to compile.
                string_address tries[4];
                string_address named = string_get_environment(environ, "CLANG");
                positive which = 0;

                tries[which++] = named && *named ? named : (string_address)"clang";
                tries[which++] = "/opt/homebrew/opt/llvm/bin/clang";
                tries[which++] = "/usr/local/opt/llvm/bin/clang";
                tries[which] = null;

                target = build_join("--target=", arch, "-unknown-linux-gnu", null);

                for (which = 0; tries[which]; which++)
                {
                        if (!build_have(tries[which]))
                                continue;

                        if (build_run(tries[which], target,
                                      build_join("-march=", march, null),
                                      build_join("-mabi=", mabi, null),
                                      "-x", "c", "-c", "-o",
                                      build_join(work, "/probe.o", null),
                                      "/dev/null", null))
                                continue;

                        compiler = tries[which];
                        reader = build_join(build_directory_of(
                                                    build_resolve(tries[which])),
                                            "/llvm-readelf", null);
                        break;
                }

                if (!compiler)
                {
                        build_remove_tree(work);
                        string_format(log,
                                      "%s floor: NOT RUN -- no compiler with a %s back end\n",
                                      arch, arch);
                        log_flush();
                        return 2;
                }
        }

        if (!build_have(reader))
        {
                if (build_have("llvm-readelf"))
                        reader = "llvm-readelf";
                else if (build_have("readelf"))
                        reader = "readelf";
                else
                {
                        build_remove_tree(work);
                        string_format(log,
                                      "%s floor: NOT RUN -- no ELF attribute reader\n",
                                      arch);
                        log_flush();
                        return 2;
                }
        }

        words[count++] = compiler;

        if (target)
                words[count++] = target;

        words[count++] = build_join("-march=", march, null);
        words[count++] = build_join("-mabi=", mabi, null);
        words[count++] = "-c";
        words[count++] = "-O2";
        words[count++] = "-DSTANDARD_NO_PLATFORM";
        words[count++] = "-ffreestanding";
        words[count++] = "-fno-builtin";
        words[count++] = "-fno-stack-protector";
        words[count++] = "-w";
        words[count++] = "-o";
        words[count++] = object;
        words[count++] = source;
        words[count] = null;

        if (build_run_words((string_address address_to)words, null))
        {
                build_remove_tree(work);
                string_format(log, "%s does not compile at the %s floor\n",
                              source, arch);
                log_flush();
                return 1;
        }

        count = 0;
        words[count++] = reader;
        words[count++] = "-A";
        words[count++] = object;
        words[count] = null;

        if (build_capture_words((string_address address_to)words, build_file_one,
                                BUILD_FILE_ROOM) < 0)
        {
                build_remove_tree(work);
                string_format(log, "the %s ELF attributes could not be read\n",
                              arch);
                log_flush();
                return 1;
        }

        build_remove_tree(work);

        //      GNU readelf calls this Tag_RISCV_arch and quotes the value;
        //      llvm-readelf prints a TagName line and then a Value. Both put
        //      the ISA string in a word of its own, so the word is what this
        //      looks for rather than either layout.
        {
                build_lines walk;
                p8 address_to store = build_text_take(BUILD_WORD_ROOM);
                positive wanted = string_length(prefix);

                build_lines_open(address_of walk, (string_address)build_file_one);

                while (build_lines_next(address_of walk) && !attributes)
                {
                        string_address found[BUILD_ARGUMENT_ROOM];
                        positive parts = build_words_of(walk.line, walk.length,
                                                        (string_address address_to)found,
                                                        BUILD_ARGUMENT_ROOM,
                                                        store, BUILD_WORD_ROOM);

                        for (positive at = 0; at < parts; at++)
                        {
                                string_address word = found[at];
                                positive length = string_length(word);

                                if (length > 1 && word[0] == '"')
                                {
                                        p8 address_to trimmed =
                                                build_text_take(length + 1);

                                        memory_copy(trimmed, word + 1, length - 1);
                                        trimmed[length - 1] = end;

                                        if (length >= 2 &&
                                            trimmed[length - 2] == '"')
                                                trimmed[length - 2] = end;

                                        word = (string_address)trimmed;
                                        length = string_length(word);
                                }

                                if (length > wanted &&
                                    !memory_compare(word, prefix, wanted))
                                {
                                        attributes = word;
                                        break;
                                }
                        }
                }
        }

        if (!attributes)
        {
                string_format(log, "%s ELF floor: missing attribute\n", arch);
                log_flush();
                return 1;
        }

        {
                string_address wanted[BUILD_ARGUMENT_ROOM];
                p8 address_to store = build_text_take(BUILD_WORD_ROOM);
                positive parts = build_words_of(require, string_length(require),
                                                (string_address address_to)wanted,
                                                BUILD_ARGUMENT_ROOM, store,
                                                BUILD_WORD_ROOM);

                for (positive at = 0; at < parts; at++)
                        if (!build_isa_holds(attributes, wanted[at]))
                        {
                                string_format(log, "%s ELF floor lacks %s: %s\n",
                                              arch, wanted[at], attributes);
                                log_flush();
                                answer = 1;
                        }
        }

        {
                string_address banned[BUILD_ARGUMENT_ROOM];
                p8 address_to store = build_text_take(BUILD_WORD_ROOM);
                positive parts = build_words_of(forbid, string_length(forbid),
                                                (string_address address_to)banned,
                                                BUILD_ARGUMENT_ROOM, store,
                                                BUILD_WORD_ROOM);

                for (positive at = 0; at < parts; at++)
                        if (build_isa_holds(attributes, banned[at]))
                        {
                                string_format(log,
                                              "%s ELF floor unexpectedly requires %s: %s\n",
                                              arch, banned[at], attributes);
                                log_flush();
                                answer = 1;
                        }
        }

        if (!answer)
        {
                string_format(log, "%s floor: %s\n", arch, attributes);
                log_flush();
        }

        return answer;
}
