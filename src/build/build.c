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

/*
        A string that must outlive the ring.

        The ring hands out BUILD_TEXT_LIVE buffers in turn, so anything kept
        across a loop that joins something has been overwritten by the time it
        is read. Whatever is held for longer than one step is copied into the
        caller's own storage first.

        This was found the honest way. `build config` printed "Configuration
        generated at kernel/profile/debug_none" -- the path it had written to
        was correct, but the name it had been holding was the last profile
        read. The assembly splitter had the same shape and was invisible: it
        wrote its temporary file under a recycled name and then renamed that
        to the right place, so the output was right and the debris was not.
*/
static string_address build_own(p8 address_to into, positive room,
                                string_address text)
{
        positive length = string_length(text);

        if (length + 1 > room)
                length = room - 1;

        memory_copy(into, text, length);
        into[length] = end;

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

/*
        Composing a configuration out of profiles.

        The profiles are concatenated into one fragment, so the kernel's own
        merge_config sees a single file and can only compare it against the
        kernel's defaults -- it has no way to say that "gpu" and "console"
        asked for opposite things. The last value written wins, silently. This
        is the only place that comparison can be made, so it is made here.
*/
#define BUILD_PAIR_ROOM 16384

typedef struct build_pair
{
        string_address name;
        positive name_length;
        string_address value;
        positive value_length;
} build_pair;

static build_pair build_pairs[BUILD_PAIR_ROOM];
static positive build_pair_order[BUILD_PAIR_ROOM];
static positive build_pair_scratch[BUILD_PAIR_ROOM];

static bipolar build_pair_compare(positive left, positive right)
{
        build_pair address_to one = address_of build_pairs[left];
        build_pair address_to two = address_of build_pairs[right];
        positive shortest = one->name_length < two->name_length
                                    ? one->name_length
                                    : two->name_length;
        bipolar answer = memory_compare(one->name, two->name, shortest);

        if (answer)
                return answer;

        if (one->name_length != two->name_length)
                return one->name_length < two->name_length ? -1 : 1;

        shortest = one->value_length < two->value_length ? one->value_length
                                                         : two->value_length;
        answer = memory_compare(one->value, two->value, shortest);

        if (answer)
                return answer;

        if (one->value_length == two->value_length)
                return 0;

        return one->value_length < two->value_length ? -1 : 1;
}

//      Bottom-up merge, because the order this produces is the order the
//      report is read in and a comparison sort that is not stable would make
//      two runs over the same profiles disagree about which line came first.
static fn build_pair_sort(positive count)
{
        for (positive at = 0; at < count; at++)
                build_pair_order[at] = at;

        for (positive width = 1; width < count; width *= 2)
        {
                positive at = 0;

                while (at < count)
                {
                        positive left = at;
                        positive middle = at + width;
                        positive right = at + width * 2;
                        positive one = left;
                        positive two = middle;
                        positive into = left;

                        if (middle > count)
                                middle = count;
                        if (right > count)
                                right = count;

                        two = middle;

                        while (one < middle && two < right)
                                build_pair_scratch[into++] =
                                        build_pair_compare(build_pair_order[two],
                                                           build_pair_order[one]) < 0
                                                ? build_pair_order[two++]
                                                : build_pair_order[one++];

                        while (one < middle)
                                build_pair_scratch[into++] = build_pair_order[one++];

                        while (two < right)
                                build_pair_scratch[into++] = build_pair_order[two++];

                        at = right;
                }

                for (positive copy = 0; copy < count; copy++)
                        build_pair_order[copy] = build_pair_scratch[copy];
        }
}

//      CONFIG_NAME=value, and only that. The name is CONFIG_ followed by a
//      run of capitals, digits and underscores that reaches an equals sign;
//      anything else on the line -- a comment, an "is not set", a lower case
//      letter in the middle of the name -- is not a setting this can compare.
static bool build_config_pair(string_address line, positive length,
                              build_pair address_to into)
{
        positive at = 7;

        if (length < 8 || memory_compare(line, "CONFIG_", 7))
                return false;

        while (at < length && ((line[at] >= 'A' && line[at] <= 'Z') ||
                               (line[at] >= '0' && line[at] <= '9') ||
                               line[at] == '_'))
                at++;

        if (at >= length || line[at] != '=' || at == 7)
                return false;

        into->name = line;
        into->name_length = at;
        into->value = line + at + 1;
        into->value_length = length - at - 1;

        return true;
}

static fn build_write_field(string_address text, positive width)
{
        positive length = string_length(text);

        string_format(log, "%s", text);

        while (length < width)
        {
                string_format(log, " ");
                length++;
        }
}

/*
        Where two profiles disagree.

        Every distinct name/value pair, sorted, deduplicated, and any name left
        with more than one is a disagreement. Each is then asked of every
        profile in turn -- the last value in a profile is that profile's
        answer, which is how merge_config resolves them too.
*/
static fn build_config_conflicts(string_address text,
                                 string_address address_to profiles,
                                 positive profile_count)
{
        build_lines walk;
        positive count = 0;
        bool announced = false;

        build_lines_open(address_of walk, text);

        while (build_lines_next(address_of walk) && count < BUILD_PAIR_ROOM)
                if (build_config_pair(walk.line, walk.length,
                                      address_of build_pairs[count]))
                        count++;

        build_pair_sort(count);

        for (positive at = 0; at < count;)
        {
                positive first = build_pair_order[at];
                positive distinct = 1;
                positive step = at + 1;

                //      Distinct pairs only: the same option set to the same
                //      value by two profiles is agreement, not conflict.
                while (step < count)
                {
                        positive here = build_pair_order[step];
                        positive before = build_pair_order[step - 1];

                        if (build_pairs[here].name_length !=
                                    build_pairs[first].name_length ||
                            memory_compare(build_pairs[here].name,
                                           build_pairs[first].name,
                                           build_pairs[first].name_length))
                                break;

                        if (build_pair_compare(here, before))
                                distinct++;

                        step++;
                }

                if (distinct > 1)
                {
                        p8 address_to name = build_text_take();
                        positive length = build_pairs[first].name_length;

                        if (length > BUILD_TEXT_ROOM - 1)
                                length = BUILD_TEXT_ROOM - 1;

                        memory_copy(name, build_pairs[first].name, length);
                        name[length] = end;

                        if (!announced)
                        {
                                string_format(log, "\n");
                                string_format(log,
                                              "Profiles disagree -- the last value wins:\n");
                                announced = true;
                        }

                        string_format(log, "  %s\n", (string_address)name);

                        for (positive which = 0; which < profile_count; which++)
                        {
                                string_address path =
                                        build_join(build_setting_get("profile_root"),
                                                   "/", profiles[which], null);
                                build_lines profile_walk;
                                p8 address_to value = build_text_take();
                                bool found = false;

                                if (build_slurp(path, build_file_two,
                                                BUILD_FILE_ROOM) < 0)
                                        continue;

                                build_lines_open(address_of profile_walk,
                                                 (string_address)build_file_two);

                                while (build_lines_next(address_of profile_walk))
                                {
                                        positive want = string_length((string_address)name);
                                        positive have;

                                        if (profile_walk.length <= want ||
                                            memory_compare(profile_walk.line,
                                                           name, want) ||
                                            profile_walk.line[want] != '=')
                                                continue;

                                        have = profile_walk.length - want - 1;

                                        if (have > BUILD_TEXT_ROOM - 1)
                                                have = BUILD_TEXT_ROOM - 1;

                                        memory_copy(value,
                                                    profile_walk.line + want + 1,
                                                    have);
                                        value[have] = end;
                                        found = have > 0;
                                }

                                if (!found)
                                        continue;

                                string_format(log, "      ");
                                build_write_field(profiles[which], 14);
                                string_format(log, " %s\n", (string_address)value);
                        }
                }

                at = step;
        }

        if (announced)
                string_format(log, "\n");

        log_flush();
}

static b32 build_config(string_address address_to profiles, positive count)
{
        p8 target_store[512];
        p8 information_store[512];
        string_address artifacts = build_setting_get("artifacts");
        string_address target = build_own(target_store, 512,
                                          build_join(artifacts, "/.config", null));
        string_address information = build_own(information_store, 512,
                                               build_join(artifacts, "/info", null));
        bool missing = false;
        positive used = 0;

        if (!count)
        {
                string_format(log_error,
                              "Usage: build config <profile1> [profile2] [profile3] ...\n");
                string_format(log_error,
                              "Example: build config any arch/x64 debug_none\n");
                log_flush();
                return 1;
        }

        //      A missing profile used to print a warning and carry on,
        //      producing a config silently missing whole feature sets. Every
        //      one is checked before anything is written.
        for (positive at = 0; at < count; at++)
        {
                string_address path = build_join(build_setting_get("profile_root"),
                                                 "/", profiles[at], null);

                if (build_is_file(path))
                        continue;

                string_format(log_error, "config: no such profile: profile/%s\n",
                              profiles[at]);
                missing = true;
        }

        log_flush();

        if (missing)
                return 1;

        {
                string_address banner = "# Auto generated, do not edit.\n";

                used = string_length(banner);
                memory_copy(build_file_one, banner, used);
        }

        {
                bipolar got = build_slurp(information, build_file_two,
                                          BUILD_FILE_ROOM);

                if (got > 0)
                {
                        memory_copy(build_file_one + used, build_file_two,
                                    (positive)got);
                        used += (positive)got;
                }
        }

        for (positive at = 0; at < count; at++)
        {
                string_address path = build_join(build_setting_get("profile_root"),
                                                 "/", profiles[at], null);
                bipolar got;

                build_file_one[used++] = '\n';
                string_format(log, "Adding profile: %s\n", profiles[at]);
                got = build_slurp(path, build_file_two, BUILD_FILE_ROOM);

                if (got < 0)
                        return build_die(build_join("cannot read profile ",
                                                    profiles[at], null));

                if (used + (positive)got + 1 >= BUILD_FILE_ROOM)
                        return build_die("the composed configuration is too large");

                memory_copy(build_file_one + used, build_file_two,
                            (positive)got);
                used += (positive)got;
        }

        build_file_one[used] = end;
        log_flush();

        if (!build_write_file(target, (string_address)build_file_one, used))
                return build_die(build_join("cannot write ", target, null));

        build_config_conflicts((string_address)build_file_one, profiles, count);

        string_format(log, "Configuration generated at %s\n", target);
        log_flush();

        //      Every key below now comes out of what was just written.
        build_config_load();

        return 0;
}

/*
        Where the built kernel disagrees with what the profiles asked for.

        merge_config and olddefconfig drop unmet options silently, so a profile
        can ask for a driver, get no warning, and produce a kernel without it
        -- which shows up later as hardware that does not work.

        The other direction is quieter and was missed for longer. A great many
        kernel options read

                bool "Something" if EXPERT
                default y

        which means that without CONFIG_EXPERT the symbol is invisible, forced
        on, and a profile line saying =n is discarded without a word. That is
        how a kernel built from a profile named debug_none shipped SLUB_DEBUG
        for as long as it did.

        Profiles are read in the order they are composed and the last request
        for an option wins, which is how merge_config resolves them too, so a
        profile deliberately overriding an earlier one is not reported.
*/
#define BUILD_REQUEST_ROOM 8192

typedef struct build_request
{
        string_address name;
        positive name_length;
        p8 setting;
        string_address profile;
} build_request;

static build_request build_requests[BUILD_REQUEST_ROOM];
static build_request build_effective[BUILD_REQUEST_ROOM];

//      Names outlive the buffer the profile was read into and there are
//      thousands of them, so they get an arena of their own rather than the
//      text ring, which recycles after BUILD_TEXT_LIVE and would hand the
//      report a name belonging to a later option.
#define BUILD_NAME_ROOM (1 << 20)

static p8 build_name_arena[BUILD_NAME_ROOM];
static positive build_name_used;

static string_address build_name_keep(string_address text, positive length)
{
        p8 address_to into;

        if (build_name_used + length + 1 > BUILD_NAME_ROOM)
                return null;

        into = build_name_arena + build_name_used;
        memory_copy(into, text, length);
        into[length] = end;
        build_name_used += length + 1;

        return (string_address)into;
}
static string_address build_built[BUILD_PAIR_ROOM];
static positive build_built_length[BUILD_PAIR_ROOM];

//      Only the tri-state settings can be checked this way. A string or an
//      integer option is left alone: it has no "present or absent" reading.
static bool build_tristate(string_address line, positive length,
                           build_request address_to into)
{
        build_pair pair;

        if (!build_config_pair(line, length, address_of pair))
                return false;

        if (pair.value_length != 1)
                return false;

        if (pair.value[0] != 'y' && pair.value[0] != 'm' && pair.value[0] != 'n')
                return false;

        into->name = pair.name;
        into->name_length = pair.name_length;
        into->setting = pair.value[0];

        return true;
}

static b32 build_verify_config(string_address config,
                               string_address address_to profiles,
                               positive profile_count)
{
        positive built = 0;
        positive requested = 0;
        positive effective = 0;
        positive missing = 0;
        positive lingering = 0;
        build_lines walk;

        build_name_used = 0;

        if (!build_is_file(config))
        {
                string_format(log_error, "verify_config: no such config: %s\n",
                              config);
                log_flush();
                return 1;
        }

        if (build_slurp(config, build_file_one, BUILD_FILE_ROOM) < 0)
        {
                string_format(log_error, "verify_config: cannot read %s\n",
                              config);
                log_flush();
                return 1;
        }

        build_lines_open(address_of walk, (string_address)build_file_one);

        while (build_lines_next(address_of walk) && built < BUILD_PAIR_ROOM)
        {
                build_pair pair;

                if (!build_config_pair(walk.line, walk.length, address_of pair))
                        continue;

                if (pair.value_length != 1 ||
                    (pair.value[0] != 'y' && pair.value[0] != 'm'))
                        continue;

                build_built[built] = pair.name;
                build_built_length[built] = pair.name_length;
                built++;
        }

        //      The profiles are read into the second buffer one at a time, so
        //      a request keeps the name of the profile it came from but not a
        //      pointer into a buffer about to be reused. The names go into the
        //      text ring, which is why the room here is bounded.
        for (positive at = 0; at < profile_count; at++)
        {
                string_address path = build_join(build_setting_get("profile_root"),
                                                 "/", profiles[at], null);
                build_lines profile_walk;

                if (!build_is_file(path))
                        continue;

                if (build_slurp(path, build_file_two, BUILD_FILE_ROOM) < 0)
                        continue;

                build_lines_open(address_of profile_walk,
                                 (string_address)build_file_two);

                while (build_lines_next(address_of profile_walk) &&
                       requested < BUILD_REQUEST_ROOM)
                {
                        build_request one;
                        string_address keep;

                        if (!build_tristate(profile_walk.line,
                                            profile_walk.length,
                                            address_of one))
                                continue;

                        keep = build_name_keep(one.name, one.name_length);

                        if (!keep)
                                continue;

                        one.name = keep;
                        one.profile = profiles[at];
                        build_requests[requested++] = one;
                }
        }

        //      Last request wins, so read the list backwards and keep the
        //      first sighting. The order that leaves is the reverse of the
        //      request order, and it is the order the report is printed in --
        //      the shell this replaces did exactly the same walk, so a report
        //      that used to be read top to bottom still reads that way.
        for (positive back = requested; back > 0; back--)
        {
                build_request address_to one = address_of build_requests[back - 1];
                bool seen = false;

                for (positive kept = 0; kept < effective; kept++)
                        if (build_effective[kept].name_length == one->name_length &&
                            !memory_compare(build_effective[kept].name, one->name,
                                            one->name_length))
                        {
                                seen = true;
                                break;
                        }

                if (seen || effective >= BUILD_REQUEST_ROOM)
                        continue;

                build_effective[effective++] = address_to one;
        }

        {
                p8 address_to dropped = build_file_two;
                p8 address_to forced = build_file_two + BUILD_FILE_ROOM / 2;
                positive dropped_used = 0;
                positive forced_used = 0;

                dropped[0] = end;
                forced[0] = end;

                for (positive at = 0; at < effective; at++)
                {
                        build_request address_to one = address_of build_effective[at];
                        bool present = false;

                        for (positive which = 0; which < built; which++)
                                if (build_built_length[which] == one->name_length &&
                                    !memory_compare(build_built[which], one->name,
                                                    one->name_length))
                                {
                                        present = true;
                                        break;
                                }

                        if ((one->setting == 'y' || one->setting == 'm') && !present)
                        {
                                string_address line = build_join("  ", one->name,
                                                                 "  (", one->profile,
                                                                 ")\n", null);
                                positive length = string_length(line);

                                if (dropped_used + length < BUILD_FILE_ROOM / 2)
                                {
                                        memory_copy(dropped + dropped_used, line,
                                                    length);
                                        dropped_used += length;
                                        dropped[dropped_used] = end;
                                }

                                missing++;
                        }
                        else if (one->setting == 'n' && present)
                        {
                                string_address line = build_join("  ", one->name,
                                                                 "  (", one->profile,
                                                                 ")\n", null);
                                positive length = string_length(line);

                                if (forced_used + length < BUILD_FILE_ROOM / 2)
                                {
                                        memory_copy(forced + forced_used, line,
                                                    length);
                                        forced_used += length;
                                        forced[forced_used] = end;
                                }

                                lingering++;
                        }
                }

                if (missing)
                {
                        string_format(log, BUILD_YELLOW
                                      "Requested but not in the built kernel:"
                                      BUILD_RESET "\n");
                        string_format(log, "%s", (string_address)dropped);
                }

                if (lingering)
                {
                        string_format(log, BUILD_YELLOW
                                      "Asked to be off but built in anyway:"
                                      BUILD_RESET "\n");
                        string_format(log, "%s", (string_address)forced);
                }
        }

        if (!missing && !lingering)
        {
                string_format(log, "All %p requested options took effect.\n",
                              effective);
                log_flush();
                return 0;
        }

        string_format(log, "\n");

        if (missing)
                string_format(log,
                              "%p of %p requested options were dropped -- usually an\n"
                              "unmet dependency, or a symbol renamed or removed in this kernel version.\n",
                              missing, effective);

        if (lingering)
                string_format(log,
                              "%p of %p options could not be turned off -- usually a\n"
                              "symbol that is invisible and forced on without CONFIG_EXPERT.\n",
                              lingering, effective);

        string_format(log, "Check either with:\n");
        string_format(log, "  grep -rn '^config OPTION$' -A5 linux/*/Kconfig*\n");
        log_flush();

        //      Not fatal: a profile composed for one architecture will
        //      legitimately carry options another cannot satisfy.
        return 0;
}

/*
        Glue -- one assembly source, every architecture.

        A .asm file holds the assembly for every architecture at once, split
        into blocks. This translates one down to a .S for a single
        architecture, which the kernel's own build then hands to whatever
        assembler that toolchain provides. Nothing here assembles anything and
        nothing here rewrites an instruction: a block holds the native syntax
        of its architecture verbatim, so real kernel assembly can be pasted in
        unchanged and an error from the assembler names an instruction that
        was actually written.

        The directives -- each of which is only a comment to the assembler,
        and none of which reach it:

            #> arch <name> [name ...]   begin a block for these architectures
            #> arch other               begin the block for every architecture
                                        no other block claimed
            #> shared                   go back to emitting for all of them

        A #> shared also closes the run of blocks before it, so a file can hold
        more than one function: each run gets its own architectures and its own
        "other". Everything before the first #> arch is shared.

        The file is read twice: once to find out whether any block claims this
        architecture, which is what decides whether the "other" block is
        emitted, and once to write. Deciding it at the end instead would move
        the "other" block to the end of the output.
*/
#define BUILD_ASM_GROUPS 512

typedef struct build_asm_group
{
        bool matched;
        bool other;
} build_asm_group;

static build_asm_group build_asm_groups[BUILD_ASM_GROUPS];
static positive build_asm_group_count;

static string_address build_asm_normalize(string_address name)
{
        if (word_is(name, "x86_64") || word_is(name, "amd64") ||
            word_is(name, "x64"))
                return "x86_64";

        if (word_is(name, "aarch64") || word_is(name, "arm64"))
                return "aarch64";

        if (word_is(name, "riscv64") || word_is(name, "riscv"))
                return "riscv64";

        return null;
}

static bool build_asm_fail(string_address source, positive line,
                           string_address message)
{
        log_flush();
        string_format(log_error, "asm: %s:%p: %s\n", source, line, message);
        log_flush();

        return false;
}

static bool build_blank(p8 byte)
{
        return byte == ' ' || byte == '\t';
}

static bool build_asm_pass(string_address text, string_address target,
                           string_address source, positive pass,
                           p8 address_to into, positive address_to used,
                           positive room)
{
        build_lines walk;
        positive line_number = 0;
        positive group = 0;
        bool open_group = false;
        bool emitting = false;
        bool marker = false;

        if (pass == 2)
        {
                for (positive at = 0; at < build_asm_group_count; at++)
                {
                        if (build_asm_groups[at].matched ||
                            build_asm_groups[at].other)
                                continue;

                        log_flush();
                        string_format(log_error, "asm: %s has no block for %s\n",
                                      source, target);
                        string_format(log_error,
                                      "asm: add \"#> arch %s\", or \"#> arch other\" to say there is nothing to do here\n",
                                      target);
                        log_flush();

                        return false;
                }

                //      What comes before the first #> arch belongs to everyone.
                emitting = true;
                marker = true;
        }

        build_lines_open(address_of walk, text);

        while (build_lines_next(address_of walk))
        {
                positive lead = 0;

                line_number++;

                while (lead < walk.length && build_blank(walk.line[lead]))
                        lead++;

                //      A directive is any line whose first non blank is "#>".
                //      Keying on that rather than on column one lets a block be
                //      indented with the code it introduces.
                if (lead + 1 < walk.length && walk.line[lead] == '#' &&
                    walk.line[lead + 1] == '>')
                {
                        string_address words[BUILD_ARGUMENT_ROOM];
                        p8 address_to store = build_text_take();
                        positive at = lead + 2;
                        positive stop = walk.length;
                        positive count;
                        bool claimed = false;

                        while (at < stop && build_blank(walk.line[at]))
                                at++;

                        while (stop > at && build_blank(walk.line[stop - 1]))
                                stop--;

                        count = build_words_of(walk.line + at, stop - at,
                                               (string_address address_to)words,
                                               BUILD_ARGUMENT_ROOM, store,
                                               BUILD_TEXT_ROOM);

                        if (!count)
                                return build_asm_fail(source, line_number,
                                                      "#> with no directive");

                        if (word_is(words[0], "shared"))
                        {
                                if (count != 1)
                                        return build_asm_fail(source, line_number,
                                                              "#> shared takes no arguments");

                                emitting = true;
                                marker = true;

                                //      A #> shared closes the run of blocks
                                //      before it, so the next #> arch opens a
                                //      new one. That is what lets one file hold
                                //      more than one function.
                                open_group = false;
                                continue;
                        }

                        if (!word_is(words[0], "arch"))
                                return build_asm_fail(source, line_number,
                                                      build_join("unknown directive: #> ",
                                                                 words[0], null));

                        if (count < 2)
                                return build_asm_fail(source, line_number,
                                                      "#> arch names no architecture");

                        if (!open_group)
                        {
                                open_group = true;
                                group++;

                                if (group > BUILD_ASM_GROUPS)
                                        return build_asm_fail(source, line_number,
                                                              "too many blocks in one file");

                                if (pass == 1 && group > build_asm_group_count)
                                        build_asm_group_count = group;
                        }

                        if (word_is(words[1], "other"))
                        {
                                if (count != 2)
                                        return build_asm_fail(source, line_number,
                                                              "#> arch other cannot be combined with an architecture");

                                if (pass == 1)
                                {
                                        if (build_asm_groups[group - 1].other)
                                                return build_asm_fail(source, line_number,
                                                                      "a second #> arch other in one run of blocks");

                                        build_asm_groups[group - 1].other = true;
                                }

                                emitting = !build_asm_groups[group - 1].matched;
                                marker = true;
                                continue;
                        }

                        for (positive which = 1; which < count; which++)
                        {
                                string_address name;

                                if (word_is(words[which], "other"))
                                        return build_asm_fail(source, line_number,
                                                              "#> arch other cannot be combined with an architecture");

                                name = build_asm_normalize(words[which]);

                                if (!name)
                                        return build_asm_fail(source, line_number,
                                                              build_join("unknown architecture: ",
                                                                         words[which], null));

                                if (word_is(name, target))
                                        claimed = true;
                        }

                        if (pass == 1 && claimed)
                                build_asm_groups[group - 1].matched = true;

                        emitting = claimed;
                        marker = true;
                        continue;
                }

                //      The first reading is only after the directives above;
                //      its bodies are nothing.
                if (pass == 1)
                        continue;

                if (!emitting)
                {
                        marker = true;
                        continue;
                }

                /*
                        The assembler is told which line of the .asm this came
                        from, so a diagnostic names the file that was written
                        rather than the one that was generated. One marker per
                        run of kept lines is enough.

                        .linefile, and not the "# 12 \"file\"" form a .S would
                        normally carry, because that form does not survive the
                        trip: the output is preprocessed before it is
                        assembled, and cpp rewrites a # line that follows a
                        macro expansion to start with a space so the next stage
                        cannot read it as a directive. Every function here
                        opens with SYM_FUNC_START, which is a macro, so the
                        marker that matters most was precisely the one being
                        discarded. It has to stay one line: a marker says what
                        line the NEXT line is.
                */
                if (marker)
                {
                        string_address text_line = build_join("\tasm_line(",
                                                              build_number(line_number),
                                                              ", \"", source,
                                                              "\")\n", null);
                        positive length = string_length(text_line);

                        if (address_to used + length >= room)
                                return false;

                        memory_copy(into + address_to used, text_line, length);
                        address_to used += length;
                        marker = false;
                }

                /*
                        A prose comment goes out as //, not #.

                        These files reach the C preprocessor, where a line
                        beginning with # is a directive. A comment starting
                        "#\tif that byte is the one asked for" is read as an
                        #if, and the error names a token in the middle of an
                        English sentence. # followed by whitespace is a comment
                        and # followed by a word is a directive, which is what
                        tells them apart.
                */
                {
                        bool prose = lead < walk.length &&
                                     walk.line[lead] == '#' &&
                                     (lead + 1 == walk.length ||
                                      build_blank(walk.line[lead + 1]));
                        positive fill = address_to used;

                        if (fill + walk.length + 3 >= room)
                                return false;

                        if (prose)
                        {
                                memory_copy(into + fill, walk.line, lead);
                                fill += lead;
                                into[fill++] = '/';
                                into[fill++] = '/';
                                memory_copy(into + fill, walk.line + lead + 1,
                                            walk.length - lead - 1);
                                fill += walk.length - lead - 1;
                        }
                        else
                        {
                                memory_copy(into + fill, walk.line, walk.length);
                                fill += walk.length;
                        }

                        into[fill++] = '\n';
                        address_to used = fill;
                }
        }

        return true;
}

static b32 build_asm(string_address arch, string_address input,
                     string_address output)
{
        p8 temporary_store[512];
        string_address target;
        string_address temporary = build_own(temporary_store, 512,
                                             build_join(output, ".asm_tmp", null));
        positive used = 0;

        if (!arch || !*arch)
        {
                string_format(log_error,
                              "asm: no target architecture given for %s\n", input);
                string_format(log_error,
                              "asm: nothing in the kernel config names an architecture this knows\n");
                log_flush();
                return 1;
        }

        if (!build_is_file(input))
        {
                string_format(log_error, "asm: no such file: %s\n", input);
                log_flush();
                return 1;
        }

        target = build_asm_normalize(arch);

        if (!target)
        {
                string_format(log_error, "asm: unknown architecture: %s\n", arch);
                log_flush();
                return 1;
        }

        if (build_slurp(input, build_file_one, BUILD_FILE_ROOM) < 0)
        {
                string_format(log_error, "asm: cannot read %s\n", input);
                log_flush();
                return 1;
        }

        build_asm_group_count = 0;
        memory_zero(build_asm_groups, sizeof(build_asm_groups));

        if (!build_asm_pass((string_address)build_file_one, target, input, 1,
                            build_file_two, address_of used, BUILD_FILE_ROOM))
                return 1;

        /*
                The banner is a C++ comment on purpose: the output is always a
                .S, so the preprocessor removes it before any assembler has an
                opinion about which character starts a comment.

                asm_line is how the line markers in the body reach an assembler
                that will take one. The clang assembler does not know
                .linefile and would stop on it, so the macro is the directive
                under one and nothing under the other.
        */
        {
                string_address banner = build_join(
                        "// Generated by kit/asm from ", input, " for ", arch,
                        ". Do not edit.\n"
                        "// Edit the .asm and build again; this file is overwritten.\n"
                        "#ifdef __clang__\n"
                        "#define asm_line(number, file)\n"
                        "#else\n"
                        "#define asm_line(number, file) .linefile number file\n"
                        "#endif\n"
                        "#ifdef MOONWATER_FREESTANDING_ASM\n"
                        "#define SYM_FUNC_START(name) .globl name ; .balign 16 ; name:\n"
                        "#define SYM_FUNC_END(name) .size name, . - name\n"
                        "#define EXPORT_SYMBOL(name)\n"
                        "#define RET ret\n"
                        "#endif\n",
                        null);

                used = string_length(banner);
                memory_copy(build_file_two, banner, used);
        }

        if (!build_asm_pass((string_address)build_file_one, target, input, 2,
                            build_file_two, address_of used, BUILD_FILE_ROOM))
                return 1;

        //      Written beside the output so the rename is atomic and a failed
        //      run leaves the previous .S alone rather than a half written one
        //      the build would trust.
        if (!build_write_file(temporary, (string_address)build_file_two, used))
        {
                string_format(log_error, "asm: cannot write %s\n", temporary);
                log_flush();
                return 1;
        }

        if (rename(temporary, output) < 0)
        {
                unlink(temporary);
                string_format(log_error, "asm: cannot rename %s to %s\n",
                              temporary, output);
                log_flush();
                return 1;
        }

        return 0;
}

/*
        Every path in this tool is relative to the repository root, so running
        it from anywhere else quietly writes into the wrong place. Checked by
        looking for what only a root has rather than by its name, which is the
        test kit/common settled on after the tree was rearranged twice
        underneath the old one: src is where kbuild wants the module's
        Makefile, kit is what the published tools live in, and build.sh is
        this program's own bootstrap.
*/
static fn build_is_safe()
{
        if (build_is_directory("src") && build_is_directory("kit") &&
            build_is_file("build.sh"))
                return;

        string_format(log_error, "ERROR: not in the repository root.\n");
        string_format(log_error,
                      "This tool expects to run from the directory holding\n");
        string_format(log_error, "build.sh, kit/ and src/.\n");
        log_flush();
        exit(1);
}

static fn build_usage()
{
        string_format(log,
                      "Builds a Moonwater image, and optionally boots or writes it.\n"
                      "\n"
                      "    build                       build with the default profiles\n"
                      "    build arch/x64 debug_none   build with the profiles named\n"
                      "    build --run                 build, then boot it in a window\n"
                      "    build --run --shell         boot with the console on this terminal\n"
                      "    build --boot                boot the last image, do not rebuild\n"
                      "    build --usb                 build, then write a USB stick\n"
                      "    build --clean               remove what a build produced\n"
                      "    build --host box            build on another machine over ssh\n"
                      "\n"
                      "The pieces, each of which was its own script under kit/:\n"
                      "\n"
                      "    build config <profile ...>              compose artifacts/.config\n"
                      "    build verify-config <config> [profile ...]  what the profiles did not get\n"
                      "    build asm <arch> <in.asm> <out.S>       one architecture out of a .asm\n"
                      "    build spark <source> <output> [debug]   link a spark program\n"
                      "    build freestanding [-v] [--run] [--watch] [source] [output]\n"
                      "    build floor [arch]                      verify the ISA floor\n"
                      "    build key <name>                        a value from artifacts/.config\n"
                      "    build key-one <name>                    the same, refusing two\n"
                      "    build size <path>                       bytes, KB and MB\n");
        log_flush();
}

b32 main()
{
        string_address address_to arguments = program_argument_list();
        positive count = (positive)program_argument_count();
        string_address command = count > 1 ? arguments[1] : null;

        //      A sub-command is only a sub-command when it cannot also be a
        //      profile: the build takes profile names as bare words, and a
        //      profile added later must not silently become a mode.
        if (command)
        {
                string_address profile = build_join(build_setting_get("profile_root"),
                                                    "/", command, null);

                if (build_is_file(profile))
                        command = null;
        }

        if (command && word_is(command, "config"))
        {
                build_is_safe();
                build_config_load();
                return build_config((string_address address_to)(arguments + 2),
                                    count - 2);
        }

        if (command && word_is(command, "verify-config"))
        {
                build_is_safe();

                if (count < 3)
                {
                        string_format(log_error,
                                      "verify_config: usage: build verify-config <built .config> [profile ...]\n");
                        log_flush();
                        return 1;
                }

                return build_verify_config(arguments[2],
                                           (string_address address_to)(arguments + 3),
                                           count - 3);
        }

        if (command && word_is(command, "asm"))
        {
                if (count != 5)
                {
                        string_format(log_error,
                                      "asm: usage: asm <arch> <input.asm> <output.S>\n");
                        log_flush();
                        return 1;
                }

                return build_asm(arguments[2], arguments[3], arguments[4]);
        }

        if (command && word_is(command, "key"))
        {
                build_is_safe();
                build_config_load();

                if (count < 3)
                        return 1;

                string_format(log, "%s\n", build_key(arguments[2]));
                log_flush();
                return 0;
        }

        if (command && word_is(command, "key-one"))
        {
                bool good = true;
                string_address answer;

                build_is_safe();
                build_config_load();

                if (count < 3)
                        return 1;

                answer = build_key_one(arguments[2], address_of good);

                if (!good)
                        return 1;

                string_format(log, "%s\n", answer);
                log_flush();
                return 0;
        }

        if (command && word_is(command, "size"))
        {
                if (count < 3)
                        return 1;

                build_size(arguments[2]);
                return 0;
        }

        if (command && (word_is(command, "--help") || word_is(command, "-h")))
        {
                build_usage();
                return 0;
        }

        build_usage();

        return 0;
}
