/*
        The build tool.

        Bootstrap it with one command, from the repository root:

            cc -O2 -static -nostdlib -nostartfiles -fno-stack-protector \
               -fno-builtin -w -o build src/build/build.c

        That is the whole of what a bare machine needs: a C compiler and an
        assembler. Nothing is linked, no library is required, and the binary
        it produces is what builds everything else. build.sh runs exactly that
        line and then hands over, so `sh build.sh` still works and still means
        the same thing.

        What this replaces: build.sh's 885 lines and the six shell scripts
        under kit/ that were the build path -- build, spark, asm, common,
        config and verify_config. The external programs they drove are still
        driven: kbuild is GNU make, the kernel arrives through curl and tar,
        the image is packed by objcopy, and QEMU boots it. What has gone is
        the shell plumbing between them.

        Where a utility exists in this tree it is called rather than spawned.
        cp, ln, rm, mkdir, mknod, chmod, find and nproc here are the same
        functions the image ships, invoked in this address space through the
        registry in src/sh/builtin.c. That is deliberate: if our cp is wrong,
        the build breaks, which is the only way a userspace gets exercised by
        something that cares. They are called one at a time and never from a
        thread -- the tools keep static arenas and must not share an address
        space concurrently.

        Nothing in the code path below spells a path, a version, a flag set or
        an architecture. Every one of those is a setting, and the settings
        this project answers with are in one table at the top. Point them
        somewhere else and the same guarantees apply to another tree.
*/

#include "../compiler_memory.c"
#include "../spark.c"
#include "../sh/shell.c"

/*
        The settings.

        One table, read by name, with this project's answers as the defaults.
        A build.conf beside build.sh overrides any of them, and --set name=value
        overrides that, so nothing below this block ever names a path.
*/
#define BUILD_SETTING_ROOM 64
#define BUILD_VALUE_ROOM 4096

typedef struct build_setting
{
        string_address name;
        string_address value;
} build_setting;

static build_setting build_settings[BUILD_SETTING_ROOM] = {
        /*      Where a build puts things. */
        {"artifacts", "artifacts"},
        {"image_root", "fs"},
        {"output", "dist"},
        {"kernel_tree", "linux"},
        {"profile_root", "kernel/profile"},

        /*      The kernel this tree builds on, and where it comes from. */
        {"kernel_version", "7.2"},
        {"kernel_mirror", "https://cdn.kernel.org/pub/linux/kernel"},
        {"kernel_keys", "torvalds@kernel.org gregkh@kernel.org"},

        /*      Linking a freestanding binary of this tree's own shape. */
        {"link_script", "kit/spark.ld"},
        {"entry", "_start"},
        {"program_source", "programs/shell.c"},

        /*      The ISA floor the library promises, and what must not be in it.
                Read by `build floor`; the lane in test/run calls that. */
        {"floor_arch", "riscv64"},
        {"floor_march", "rv64imafd_zicsr_zicntr"},
        {"floor_mabi", "lp64d"},
        {"floor_source", "src/library.c"},
        {"floor_require", "i m a f d zicsr zicntr"},
        {"floor_forbid", "c zca zcb zcd zcf zcmp zcmt"},

        /*      What the build needs before it starts. */
        {"required", "bison flex bc gpg make gcc clang rustc"},

        /*      The profiles composed ahead of whatever was asked for, in this
                order, so the last two win the choices the earlier ones touch. */
        {"profiles_always", "any general gpu guests latency prod"},
        {"profiles_default", "arch/x64 debug_none limbo desktop serial"},

        {null, null},
};

static string_address build_setting_get(string_address name)
{
        for (positive at = 0; build_settings[at].name; at++)
                if (word_is(build_settings[at].name, name))
                        return build_settings[at].value;

        return null;
}

static bool build_setting_set(string_address name, string_address value)
{
        positive at = 0;

        while (build_settings[at].name)
        {
                if (word_is(build_settings[at].name, name))
                {
                        build_settings[at].value = value;
                        return true;
                }
                at++;
        }

        if (at + 1 >= BUILD_SETTING_ROOM)
                return false;

        build_settings[at].name = name;
        build_settings[at].value = value;
        build_settings[at + 1].name = null;
        build_settings[at + 1].value = null;

        return true;
}

/*
        Text.

        A build assembles a great many short strings -- paths, command lines,
        flag lists -- and none of them outlives the step that made it. A ring
        of fixed buffers handed out in turn is the whole allocator this needs:
        no free, no growth, and a fixed ceiling that a build cannot quietly
        walk past. BUILD_TEXT_LIVE is how many are live at once; exceed it and
        an earlier one is overwritten, which is why nothing here keeps a
        borrowed string across a step.
*/
#define BUILD_TEXT_ROOM 8192
#define BUILD_TEXT_LIVE 32

static p8 build_text_ring[BUILD_TEXT_LIVE][BUILD_TEXT_ROOM];
static positive build_text_next;

static p8 address_to build_text_take()
{
        p8 address_to answer = build_text_ring[build_text_next];

        build_text_next = (build_text_next + 1) % BUILD_TEXT_LIVE;
        answer[0] = end;

        return answer;
}

/*
        Join, with the pieces named rather than counted.

        A null argument ends the list, so a caller can pass a value it knows
        may be absent and get the shorter string instead of a crash.
*/
static string_address build_join(string_address first, ...)
{
        p8 address_to into = build_text_take();
        p8 address_to at = into;
        positive left = BUILD_TEXT_ROOM - 1;
        string_address piece = first;
        var_args rest;

        var_list(rest, first);

        while (piece)
        {
                positive length = string_length(piece);

                if (length > left)
                        length = left;

                memory_copy(at, piece, length);
                at += length;
                left -= length;
                piece = var_list_get(rest, string_address);
        }

        var_list_end(rest);
        *at = end;

        return (string_address)into;
}

static string_address build_number(positive value)
{
        p8 address_to into = build_text_take();
        positive length = positive_into(into, value);

        into[length] = end;

        return (string_address)into;
}

//      A setting used as a path, which is every path this tool writes.
static string_address build_in(string_address setting, string_address name)
{
        string_address root = build_setting_get(setting);

        return name ? build_join(root, "/", name, null) : root;
}

/*
        Diagnostics.

        The colours and the shape of a label are what the shell build printed,
        because a build log people have read for years is an interface too.
        $'\033[' was a bash extension the old kit/common had to work around;
        here the byte is just a byte.
*/
#define BUILD_RESET "\033[0m"
#define BUILD_BOLD "\033[1m"
#define BUILD_RED "\033[91m"
#define BUILD_GREEN "\033[92m"
#define BUILD_YELLOW "\033[93m"
#define BUILD_CYAN "\033[96m"

static fn build_say(string_address text)
{
        string_format(log, BUILD_CYAN BUILD_BOLD "%s" BUILD_RESET "\n", text);
        log_flush();
}

static fn build_label(string_address colour, string_address text)
{
        string_format(log, "%s %s \n", BUILD_CYAN, BUILD_BOLD);
        string_format(log, "    %s%s\n", colour, text);
        string_format(log,
                      "_____________________________________________________________________________\n");
        string_format(log, "%s \n", BUILD_RESET);
        log_flush();
}

static b32 build_die(string_address text)
{
        log_flush();
        string_format(log_error, BUILD_RED "build failed: %s" BUILD_RESET "\n",
                      text);
        log_flush();
        exit(1);

        return 1;
}

/*
        A size, in the three units the old size helper printed.

        Integer arithmetic on purpose: bc is not installed everywhere, and the
        tenths are a remainder scaled by ten rather than a division that would
        need a floating point unit this may not have.
*/
static fn build_size(string_address path)
{
        error_stat status;

        if (stat(path, address_of status) < 0)
        {
                string_format(log_error, "size: cannot stat '%s'\n", path);
                log_flush();
                return;
        }

        positive bytes = (positive)status.st_size;

        string_format(log, "%p bytes (%p.%p KB, %p.%p MB)\n", bytes,
                      bytes / 1024, ((bytes % 1024) * 10) / 1024,
                      bytes / 1048576, ((bytes % 1048576) * 10) / 1048576);
        log_flush();
}

/*
        Where a build runs.

        Every path this tool writes is relative to the repository root, so
        running it from anywhere else quietly writes into the wrong place.
        Checked by looking for what only a root has rather than by its name,
        which is the test kit/common settled on after the tree was rearranged
        twice underneath the old one.
*/
static bool build_is_directory(string_address path)
{
        error_stat status;

        return stat(path, address_of status) >= 0 &&
               (status.st_mode & S_IFMT) == S_IFDIR;
}

static bool build_is_file(string_address path)
{
        error_stat status;

        return stat(path, address_of status) >= 0 &&
               (status.st_mode & S_IFMT) == S_IFREG;
}

static positive build_modified(string_address path)
{
        error_stat status;

        if (stat(path, address_of status) < 0)
                return 0;

        return (positive)status.st_mtime;
}

//      True when the first is newer, and when either is missing -- the caller
//      is gating "does this need redoing", and a missing file always does.
//      kit/common's version of this compared file SIZES while being named and
//      used as an age comparison, so a config edit that did not grow the file
//      was silently ignored and the previous configuration got built.
static bool build_is_newer(string_address first, string_address second)
{
        if (!build_is_file(first) || !build_is_file(second))
                return true;

        return build_modified(first) > build_modified(second);
}

/*
        Calling a utility of this image.

        The registry in src/sh/builtin.c dispatches on argv[0], so a call is
        the argument vector swapped for one of ours, the tool run, and the
        caller's vector put back. That is what shell_kill already does for the
        one builtin that borrows a utility's parser; this is the same move
        with the utility named rather than implied.

        One at a time and never from a thread: the tools keep static arenas
        and must not share an address space concurrently.
*/
static b32 build_tool_words(string_address address_to words)
{
        string_address address_to saved = program_argument_list();
        b32 saved_count = program_argument_count();
        b32 count = 0;
        b32 answer;

        while (words[count])
                count++;

        //      Ours buffers its output through the same log this does. Flush
        //      before and after or the tool's bytes land inside a line of the
        //      build log, or after the step that produced them.
        log_flush();
        program_arguments_use(words, count);
        answer = shell_tool_as_called();
        program_arguments_use(saved, saved_count);
        log_flush();

        //      -1 is the registry saying the name is not a tool's, which for
        //      a caller that named one is a programming error, not a status.
        return answer < 0 ? 127 : answer;
}

#define BUILD_ARGUMENT_ROOM 512

static b32 build_tool(string_address name, ...)
{
        string_address words[BUILD_ARGUMENT_ROOM];
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

        return build_tool_words((string_address address_to)words);
}

/*
        Spawning something that is not ours.

        The toolchain, make, tar, curl, gpg, ssh, rsync and QEMU stay separate
        programs: they are not this tree's to reimplement and driving them is
        what a build tool is for. Everything below goes through these three so
        that a failed command is a status rather than a shell's opinion of one.
*/
static b32 build_wait(b32 child)
{
        b32 status = 0;

        if (child < 0)
                return -1;

        if (system_wait4_retry(child, address_of status, 0, null) < 0)
                return -1;

        return (b32)wait_status_code((positive)status);
}

static b32 build_spawn(string_address address_to words,
                       string_address address_to environment)
{
        b32 child;

        log_flush();
        child = fork();

        if (child == 0)
        {
                execve(words[0], words, environment ? environment : environ);
                //      exec only returns having failed, and this is the child:
                //      leaving would run the rest of the build twice.
                exit(127);
        }

        return child;
}

static b32 build_run_words(string_address address_to words,
                           string_address address_to environment)
{
        return build_wait(build_spawn(words, environment));
}

static b32 build_run(string_address name, ...)
{
        string_address words[BUILD_ARGUMENT_ROOM];
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

        return build_run_words((string_address address_to)words, null);
}

/*
        Reading what another program said.

        A pipe, a child with its output on the write end, and the parent
        reading until the end. Only stdout is collected: the callers here are
        asking a question -- which accelerators does this QEMU have, what is
        the entry point of that ELF -- and a tool's complaints belong on the
        terminal where somebody can see them.
*/
static bipolar build_capture_words(string_address address_to words,
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
                execve(words[0], words, environ);
                exit(127);
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

        //      The status is discarded on purpose: every caller wants the
        //      bytes, and a program that says nothing and fails says the same
        //      thing to them as one that says nothing and succeeds.
        build_wait(child);

        return (bipolar)used;
}

/*
        Is that program installed.

        `command -v` in one function. A name with a separator in it is a path
        and is asked about directly; anything else is looked for along PATH,
        which is what the shell would have done and what the build asks about
        before it decides to install a compiler.
*/
static bool build_have(string_address name)
{
        string_address path = string_get_environment(environ, "PATH");
        string_address at;

        if (!name || !*name)
                return false;

        if (string_first_of(name, '/'))
                return access(name, X_OK) >= 0;

        if (!path)
                return false;

        at = path;

        while (true)
        {
                p8 address_to into = build_text_take();
                positive span = 0;
                positive kept;

                while (at[span] && at[span] != ':')
                        span++;

                kept = span;

                //      An empty entry in PATH means the working directory,
                //      which is what every shell does with it.
                if (!kept)
                {
                        into[0] = '.';
                        kept = 1;
                }
                else
                {
                        if (kept > BUILD_TEXT_ROOM - 2)
                                kept = BUILD_TEXT_ROOM - 2;
                        memory_copy(into, at, kept);
                }

                into[kept] = end;

                if (access(build_join((string_address)into, "/", name, null),
                           X_OK) >= 0)
                        return true;

                if (!at[span])
                        break;

                at += span + 1;
        }

        return false;
}

/*
        Whole files.

        A configuration, a profile and a generated assembly source are all
        read entire and walked in memory: they are tens of kilobytes, the
        walks want to look backwards as well as forwards, and a build that
        cannot hold linux/.config in memory cannot build a kernel either.
*/
#define BUILD_FILE_ROOM (4 << 20)

static p8 build_file_one[BUILD_FILE_ROOM];
static p8 build_file_two[BUILD_FILE_ROOM];

static bipolar build_slurp(string_address path, p8 address_to into,
                           positive capacity)
{
        b32 handle = open(path, O_RDONLY, 0);
        positive used = 0;

        if (handle < 0)
                return -1;

        while (used + 1 < capacity)
        {
                bipolar got = read(handle, into + used, capacity - used - 1);

                if (got <= 0)
                        break;

                used += (positive)got;
        }

        into[used] = end;
        close(handle);

        return (bipolar)used;
}

static bool build_write_file(string_address path, string_address data,
                             positive length)
{
        b32 handle = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        positive written = 0;

        if (handle < 0)
                return false;

        while (written < length)
        {
                bipolar put = write(handle, data + written, length - written);

                if (put <= 0)
                {
                        close(handle);
                        return false;
                }

                written += (positive)put;
        }

        close(handle);

        return true;
}

static bool build_append_file(string_address path, string_address data,
                              positive length)
{
        b32 handle = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        positive written = 0;

        if (handle < 0)
                return false;

        while (written < length)
        {
                bipolar put = write(handle, data + written, length - written);

                if (put <= 0)
                {
                        close(handle);
                        return false;
                }

                written += (positive)put;
        }

        close(handle);

        return true;
}

/*
        Lines.

        Every file this reads is a line-oriented one, and every walk over it
        wants the line without its newline and the place the next one starts.
        The buffer is written into rather than copied out of: a slurped file
        is ours, and terminating each line in place is one store against a
        copy per line.
*/
typedef struct build_lines
{
        string_address at;
        string_address line;
        positive length;
} build_lines;

static fn build_lines_open(build_lines address_to walk, string_address buffer)
{
        walk->at = buffer;
        walk->line = null;
        walk->length = 0;
}

/*
        The next line, without a newline and without writing anything.

        Terminating each line in place would be cheaper and is what the first
        version did, which meant a second walk over the same buffer saw a file
        of one line. Nothing here mutates what it was handed.
*/
static bool build_lines_next(build_lines address_to walk)
{
        string_address stop;

        if (!walk->at || !*walk->at)
                return false;

        stop = string_first_of(walk->at, '\n');
        walk->line = walk->at;

        if (stop)
        {
                walk->length = (positive)(stop - walk->at);
                walk->at = stop + 1;
        }
        else
        {
                walk->length = string_length(walk->at);
                walk->at += walk->length;
        }

        return true;
}

//      The words of one line, collapsed the way an unquoted shell expansion
//      collapses them: runs of blanks are one separator and the ends are
//      trimmed. kit/common's `key` was exactly `echo $(...)`, so anything
//      reading a key got this and nothing else. The newline is in the
//      separator set because the line is a span of a larger buffer and is not
//      terminated.
static positive build_words_of(string_address line, positive bound,
                               string_address address_to into, positive room,
                               p8 address_to store, positive store_room)
{
        positive count = 0;
        positive used = 0;
        positive at = 0;

        while (at < bound && count < room)
        {
                positive length = 0;

                while (at < bound && (line[at] == ' ' || line[at] == '\t'))
                        at++;

                while (at + length < bound && line[at + length] != ' ' &&
                       line[at + length] != '\t')
                        length++;

                if (!length)
                        break;

                if (used + length + 1 > store_room)
                        break;

                memory_copy(store + used, line + at, length);
                store[used + length] = end;
                into[count++] = (string_address)(store + used);
                used += length + 1;
                at += length;
        }

        return count;
}

/*
        The keys.

        A profile may carry lines the kernel's own configuration language has
        no room for -- which compiler to use, what to name the built image,
        what to run before and after -- and they ride in comments the kernel
        ignores:

            #> compiler gcc

        Anchored, with the trailing space, so `key pre` cannot also match
        `#> prefix`. Multiple matching lines join into one line on purpose:
        that is how a flag list accumulates across composed profiles.
*/
static string_address build_key_from(string_address buffer, string_address name,
                                     positive address_to matched)
{
        p8 address_to into = build_text_take();
        p8 address_to write_at = into;
        positive left = BUILD_TEXT_ROOM - 1;
        string_address marker = build_join("#> ", name, " ", null);
        positive marker_length = string_length(marker);
        build_lines walk;
        positive seen = 0;

        build_lines_open(address_of walk, buffer);

        while (build_lines_next(address_of walk))
        {
                string_address words[BUILD_ARGUMENT_ROOM];
                p8 address_to store = build_text_take();
                positive count;

                if (walk.length < marker_length ||
                    memory_compare(walk.line, marker, marker_length))
                        continue;

                seen++;
                count = build_words_of(walk.line + marker_length,
                                       walk.length - marker_length,
                                       (string_address address_to)words,
                                       BUILD_ARGUMENT_ROOM, store,
                                       BUILD_TEXT_ROOM);

                for (positive at = 0; at < count; at++)
                {
                        positive length = string_length(words[at]);

                        if (write_at != into && left)
                        {
                                *write_at++ = ' ';
                                left--;
                        }

                        if (length > left)
                                length = left;

                        memory_copy(write_at, words[at], length);
                        write_at += length;
                        left -= length;
                }
        }

        *write_at = end;

        if (matched)
                address_to matched = seen;

        return (string_address)into;
}

static bool build_config_loaded;

//      artifacts/.config is read once and every key comes out of that copy.
//      The shell asked grep afresh for each of them, which was a process and
//      a re-read per key and made the answers able to disagree with each
//      other if a profile step rewrote the file in between.
static bool build_config_load()
{
        string_address path = build_in("artifacts", ".config");

        build_config_loaded = build_slurp(path, build_file_one,
                                          BUILD_FILE_ROOM) >= 0;

        if (!build_config_loaded)
                build_file_one[0] = end;

        return build_config_loaded;
}

static string_address build_key(string_address name)
{
        if (!build_config_loaded)
                return "";

        return build_key_from((string_address)build_file_one, name, null);
}

/*
        For scalars -- compiler, arch, kernel_image -- where two profiles both
        setting the value would silently concatenate into an unusable command.
*/
static string_address build_key_one(string_address name, bool address_to good)
{
        positive matched = 0;
        string_address answer;

        if (good)
                address_to good = true;

        if (!build_config_loaded)
                return "";

        answer = build_key_from((string_address)build_file_one, name,
                                address_of matched);

        if (matched > 1)
        {
                string_format(log_error,
                              "config: '%s' is set %p times; expected one value\n",
                              name, matched);
                log_flush();

                if (good)
                        address_to good = false;

                return "";
        }

        return answer;
}

b32 main()
{
        build_say("build");

        return 0;
}
