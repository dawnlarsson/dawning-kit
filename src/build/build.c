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
        An optional build.conf beside build.sh overrides any of them, and
        --set name=value on the command line overrides that, so nothing below
        this block ever names a path, a version, a flag set or an
        architecture.
*/
#define BUILD_SETTING_ROOM 128

//      Beside build.sh, and optional: this repository ships none.
#define BUILD_SETTINGS_FILE "build.conf"

typedef struct build_setting
{
        string_address name;
        string_address value;
} build_setting;

static build_setting build_settings[BUILD_SETTING_ROOM] = {
        /*      What the built system calls itself. */
        {"name", "moonwater"},
        {"version", "25"},
        {"full_name", "moonwater-25"},

        /*      Where a build puts things. */
        {"artifacts", "artifacts"},
        {"image_root", "fs"},
        {"output", "dist"},
        {"kernel_tree", "linux"},
        {"profile_root", "kernel/profile"},

        /*      The kernel this tree builds on, and where it comes from.

                The signature is pinned here rather than downloaded next to
                the tarball. Fetching both would still verify, but only that
                the archive is signed by a trusted key -- pinning ties the
                build to this exact release, so a validly signed but different
                kernel cannot be substituted.

                To move to a new release: take the .sign file from the
                mirror's linux-VERSION.tar.sign and paste it here along with
                the version. */
        {"kernel_version", "7.2"},
        {"kernel_mirror", "https://cdn.kernel.org/pub/linux/kernel"},
        {"kernel_keys", "torvalds@kernel.org gregkh@kernel.org"},
        {"kernel_signature",
         "-----BEGIN PGP SIGNATURE-----\n"
         "Comment: This signature is for the .tar version of the archive\n"
         "Comment: git archive --format tar --prefix=linux-7.2/ v7.2\n"
         "Comment: git version 2.55.0\n"
         "\n"
         "iQIzBAABCgAdFiEEZH8oZUiU471FcZm+ONu9yGCSaT4FAmqCjM4ACgkQONu9yGCS\n"
         "aT6jEBAAi+dDv3sQNuZPoSOjnv3be79xilhgbYRjXjYGyYr/axHwyCfRxYkV/sL0\n"
         "SHOXT9ZGKp/GPjc8i21Pgca4c4UhckX48RTH7xNO3dR9X8n3g+8OLqP8FF2iFqdv\n"
         "TWnagMo6CFyMmWj75WRwcZGKw2fOjCr9tSTSklAkLc8gytgUyHJKxcDHrYDpcdRF\n"
         "GbhXn9GauSYu0ablmf6pSInjicXDMzPj9QVSt9NkO6FcrSoAfUfmU4c9EEsKW9T6\n"
         "K5LsiyhRgcQfE0zrw1hYQBr2gFSXt8pa2u2XPVVukIBB9XSPdSG2x228b+yHmp/Y\n"
         "zPRUzPDVkkK1BkU1D7XJdVmt2C3kfeBUJEcAlVKcDWf9rY80SU6FVyc45TwRfw8h\n"
         "kq86+ERAmWOCwYsZjMK4i3PK4Zs60Q0rQZgmMY/mfqSxzMoCV2O9FGea8ZZQIlGH\n"
         "m3qZw79igreY852bLihddRDgXAz47VFAwRnqzKaSJVMtUdigEPb34idC2ZE0yp07\n"
         "PnHgCqFYktDu3+Enpm7RItsK0b0oQHdmeB8eOPgGSJ3gcJVmGKVaS4zd46gGDgJC\n"
         "yt0LTonkwQO8q3jTN/2ffkVjzdrvk4IeYX5k3SQ6rinfebi0OMCQ9xDyR7MYvdwu\n"
         "wgcVXSeiHcXa9SSFDvKn0L1q5nSLQGHp38qUi1ZPf/1uQSuB3ME=\n"
         "=D53G\n"
         "-----END PGP SIGNATURE-----\n"},

        /*      Linking a freestanding binary of this tree's own shape.
                The head and tail are separate so the whole-program flags land
                where they always have. Flag order does not change the output,
                but a diff of two build logs should not claim it did. */
        {"link_script", "kit/spark.ld"},
        {"entry", "_start"},
        {"freestanding_source", "src/main.c"},
        {"freestanding_output", "bin"},
        {"freestanding_flags",
         "-static -s -flto -nostdlib -nostartfiles -ffreestanding -fno-builtin"
         " -Qn -Wl,--build-id=none -Wl,--gc-sections -Wl,--strip-all"
         " -Wl,--strip-debug -Wl,-x -Wl,-s -Wl,--no-warn-rwx-segments"
         " -Wl,-nmagic -O2"},
        {"whole_program_flags", "-fwhole-program -fipa-pta"},
        {"freestanding_flags_tail",
         "-fno-asynchronous-unwind-tables -fomit-frame-pointer"
         " -fno-stack-protector -fno-semantic-interposition"
         " -D_FORTIFY_SOURCE=0 -fno-unwind-tables -fno-plt -fno-PIE -fno-pie"
         " -fno-stack-clash-protection"},

        /*      The ISA floor the library promises, and what must not be in
                it. `build floor` proves both against the ELF attributes of an
                object it compiles at that floor, and the standard lane in
                test/run reads its answer.

                Why these extensions, for this tree: A is part of the public
                surface because the atomic macros lower to it. F and D are part
                of the assembly itself -- string_format and fast_sin use both
                precisions. get_cpu_time reads the time CSR, so Zicntr and its
                Zicsr dependency are explicit too. Software fallbacks for those
                would be a different implementation, not something a compile
                check should pretend exists. C is forbidden rather than merely
                unrequested: the shipped routines must assemble without
                compressed instructions, and a toolchain defaulting to rv64gc
                would put them back without a word. */
        {"floor_arch", "riscv64"},
        {"floor_prefix", "rv64"},
        {"floor_march", "rv64imafd_zicsr_zicntr"},
        {"floor_mabi", "lp64d"},
        {"floor_source", "src/library.c"},
        {"floor_require", "i m a f d zicsr zicntr"},
        {"floor_forbid", "c zca zcb zcd zcf zcmp zcmt"},

        /*      What the build needs before it starts. */
        {"required", "bison flex bc gpg make gcc clang rustc"},

        /*      The sources and scripts a build reads. */
        {"tool_registry", "src/sh/tools.inc"},
        {"shell_source", "programs/shell"},
        {"utilities_source", "programs/utilities"},
        {"monitor_source", "programs/monitor.sh"},
        {"patch_script", "kernel/patch/apply"},
        {"replace_script", "kernel/replace/apply"},

        /*      Booting the built image, and where the module's own build
                products land beside its source. */
        {"emulator", "qemu-system-x86_64"},
        {"emulator_flags", "-m 2G -smp 2 -cpu Nehalem"},
        {"emulator_devices",
         "-vga none -device virtio-gpu-pci -device qemu-xhci"
         " -device usb-tablet -device usb-kbd -no-reboot"},
        {"kernel_cmdline", "console=ttyS0 drm_client_lib.active="},
        {"default_image", "dist/bootx64.efi"},
        {"module_root", "src"},
        {"clean_patterns",
         "[!.]*.a [!.]*.o [!.]*.o.d [!.]*.cmd [!.]*.order"
         " [!.]*.S [!.]*.asm_tmp"},

        /*      What a remote build does not need a copy of: the upstream
                kernel tree, its artifacts and the built filesystem are large
                and none of them belong to this checkout. */
        {"remote_excludes", ".git .claude linux artifacts fs dist"},

        /*      The image's own layout: the directories every build makes and
                the device nodes it boots with, as name, type, major, minor.
                The spark minor has to match SPARK_DEVICE_MINOR in src/spark.c. */
        {"image_directories",
         "sys proc dev tmp etc root bin sbin usr lib lib64 var opt bowls/bin"},
        {"image_nodes",
         "dev/tty c 5 0"
         " dev/console c 5 1"
         " dev/null c 1 3"
         " dev/zero c 1 5"
         " dev/random c 1 8"
         " dev/urandom c 1 9"
         " dev/kmsg c 1 11"
         " dev/spark c 10 250"},

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
        flag lists -- and it is one command from start to finish, so an arena
        that only ever grows is the whole allocator this needs. Nothing is
        freed and nothing is reused, which is the point: every string this
        hands out stays valid until the command ends.

        The first version was a ring of thirty two buffers handed out in turn,
        on the reasoning that no string outlives the step that made it. Three
        separate bugs said otherwise and only one of them was visible. `build
        config` reported writing to the last profile it had read. The assembly
        splitter wrote its temporary file under a recycled name and renamed
        that to the right place, so the output was correct and the debris was
        not. And the key reader took a buffer per line of a two thousand line
        configuration, which recycled the very marker it was matching against,
        so every key came back empty and the compiler was invoked as its own
        directory. A ring is a bet that the author remembers its rule at every
        call site. This does not need the bet.

        The ceiling is a fixed array rather than a growing one, so a build that
        asks for too much stops and says so instead of failing later in a way
        that looks like something else.
*/
#define BUILD_TEXT_ROOM (16 << 20)

//      One line's worth of words, which is what every splitter asks for.
#define BUILD_WORD_ROOM 8192

static p8 build_text_arena[BUILD_TEXT_ROOM];
static positive build_text_used;

static b32 build_die(string_address text);

static p8 address_to build_text_take(positive want)
{
        p8 address_to answer;

        if (build_text_used + want > BUILD_TEXT_ROOM)
                build_die("build: ran out of room for text");

        answer = build_text_arena + build_text_used;
        build_text_used += want;
        answer[0] = end;

        return answer;
}

//      Only --watch runs more than one build in one process, and it is the
//      one caller that has to give the arena back.
static fn build_text_reset()
{
        build_text_used = 0;
}

/*
        Join, with the pieces named rather than counted.

        A null argument ends the list, so a caller can pass a value it knows
        may be absent and get the shorter string instead of a crash. The list
        is walked twice -- once to measure, once to copy -- so the arena is
        asked for exactly what the answer needs.
*/
static string_address build_join(string_address first, ...)
{
        positive total = 0;
        string_address piece = first;
        p8 address_to into;
        p8 address_to at;
        var_args rest;
        var_args measure;

        var_list(rest, first);
        var_list_copy(rest, measure);

        while (piece)
        {
                total += string_length(piece);
                piece = var_list_get(measure, string_address);
        }

        var_list_end(measure);

        into = build_text_take(total + 1);
        at = into;
        piece = first;

        while (piece)
        {
                positive length = string_length(piece);

                memory_copy(at, piece, length);
                at += length;
                piece = var_list_get(rest, string_address);
        }

        var_list_end(rest);
        *at = end;

        return (string_address)into;
}

static string_address build_number(positive value)
{
        p8 address_to into = build_text_take(32);
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
        string_format(log, "%s %s\n", BUILD_CYAN, BUILD_BOLD);
        string_format(log, "    %s%s\n", colour, text);
        string_format(log,
                      "_____________________________________________________________________________\n");
        string_format(log, "%s\n", BUILD_RESET);
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

//      Named below, defined below that: the spawn helpers need it and it
//      needs the text ring, so one of the two orders has to be broken.
static string_address build_resolve(string_address name);

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

/*
        execve takes a path, not a name.

        Everything the shell build invoked -- gcc, make, tar, ssh -- it named
        and the shell found along PATH. A cross compiler is named
        x86_64-linux-gnu-gcc in the configuration and lives in /usr/bin, so
        handing that name straight to execve got 127 and "compilation failed"
        with nothing above it to say why.
*/
static b32 build_spawn(string_address address_to words,
                       string_address address_to environment)
{
        string_address path = build_resolve(words[0]);
        b32 child;

        if (!path)
        {
                string_format(log_error, "build: %s not found\n", words[0]);
                log_flush();
                return -1;
        }

        //      BUILD_TRACE prints every command before it runs. A build tool
        //      that drives six other programs has to be able to say exactly
        //      what it asked them, or a failure is a guess.
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
                execve(path, words, environment ? environment : environ);
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
        string_address found;
        b32 pair[2];
        b32 child;
        positive used = 0;

        if (!capacity)
                return -1;

        into[0] = end;

        {
                string_address path = build_resolve(words[0]);

                if (!path)
                        return -1;

                found = path;
        }

        if (pipe(pair) < 0)
                return -1;

        log_flush();
        child = fork();

        if (child == 0)
        {
                close(pair[0]);
                dup2(pair[1], 1);
                close(pair[1]);
                execve(found, words, environ);
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
static string_address build_resolve(string_address name)
{
        string_address path = string_get_environment(environ, "PATH");
        string_address at;

        if (!name || !*name)
                return null;

        if (string_first_of(name, '/'))
                return access(name, X_OK) >= 0 ? name : null;

        if (!path)
                return null;

        at = path;

        while (true)
        {
                p8 address_to into = build_text_take(BUILD_WORD_ROOM);
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
                        if (kept > BUILD_WORD_ROOM - 2)
                                kept = BUILD_WORD_ROOM - 2;
                        memory_copy(into, at, kept);
                }

                into[kept] = end;

                {
                        string_address candidate =
                                build_join((string_address)into, "/", name, null);

                        if (access(candidate, X_OK) >= 0)
                                return candidate;
                }

                if (!at[span])
                        break;

                at += span + 1;
        }

        return null;
}

static bool build_have(string_address name)
{
        return build_resolve(name) != null;
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

//      The composed configuration gets a buffer nothing else touches. It was
//      read into the first of the two above, and the verifier and the spark
//      packer both read files into that one -- so by the time the userspace
//      build asked for the architecture, the answer had been overwritten by a
//      kernel configuration and every key came back empty.
static p8 build_config_buffer[BUILD_FILE_ROOM];

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

//      Named here and defined with the assembly splitter, which is the other
//      thing that cares where a line's blanks are.
static bool build_blank(p8 byte);

/*
        A tree that is not this one.

        Every setting above is this project's answer, and there are two ways to
        give another tree's. A build.conf beside build.sh holds one
        `name value` per line -- the same shape as the `#>` keys a profile
        carries, because a reader who knows one knows the other -- and
        --set name=value on the command line wins over it. Neither is required
        and this repository ships neither, so the table is what runs here.

        A name the table does not have is added rather than refused: a tree
        with its own steps has its own settings, and this is where they live.
*/
static fn build_settings_read()
{
        build_lines walk;
        p8 address_to store;

        if (build_slurp(BUILD_SETTINGS_FILE, build_file_two,
                        BUILD_FILE_ROOM) < 0)
                return;

        store = build_text_take(BUILD_WORD_ROOM);
        build_lines_open(address_of walk, (string_address)build_file_two);

        while (build_lines_next(address_of walk))
        {
                positive at = 0;
                positive name_length = 0;
                string_address name;
                string_address value;

                while (at < walk.length && build_blank(walk.line[at]))
                        at++;

                if (at >= walk.length || walk.line[at] == '#')
                        continue;

                while (at + name_length < walk.length &&
                       !build_blank(walk.line[at + name_length]))
                        name_length++;

                //      Copied out of the file buffer, which the next thing to
                //      read a file will overwrite.
                {
                        p8 address_to keep = build_text_take(name_length + 1);

                        memory_copy(keep, walk.line + at, name_length);
                        keep[name_length] = end;
                        name = (string_address)keep;
                }

                at += name_length;

                while (at < walk.length && build_blank(walk.line[at]))
                        at++;

                {
                        positive length = walk.length - at;
                        p8 address_to keep = build_text_take(length + 1);

                        memory_copy(keep, walk.line + at, length);
                        keep[length] = end;
                        value = (string_address)keep;
                }

                build_setting_set(name, value);
        }
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
        p8 address_to into = build_text_take(BUILD_WORD_ROOM);
        p8 address_to store = build_text_take(BUILD_WORD_ROOM);
        p8 address_to write_at = into;
        positive left = BUILD_WORD_ROOM - 1;
        string_address marker = build_join("#> ", name, " ", null);
        positive marker_length = string_length(marker);
        build_lines walk;
        positive seen = 0;

        build_lines_open(address_of walk, buffer);

        while (build_lines_next(address_of walk))
        {
                string_address words[BUILD_ARGUMENT_ROOM];
                positive count;

                if (walk.length < marker_length ||
                    memory_compare(walk.line, marker, marker_length))
                        continue;

                seen++;
                count = build_words_of(walk.line + marker_length,
                                       walk.length - marker_length,
                                       (string_address address_to)words,
                                       BUILD_ARGUMENT_ROOM, store,
                                       BUILD_WORD_ROOM);

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

        build_config_loaded = build_slurp(path, build_config_buffer,
                                          BUILD_FILE_ROOM) >= 0;

        if (!build_config_loaded)
                build_config_buffer[0] = end;

        return build_config_loaded;
}

static string_address build_key(string_address name)
{
        if (!build_config_loaded)
                return "";

        return build_key_from((string_address)build_config_buffer, name, null);
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

        answer = build_key_from((string_address)build_config_buffer, name,
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
                        p8 address_to name = build_text_take(BUILD_WORD_ROOM);
                        positive length = build_pairs[first].name_length;

                        if (length > BUILD_WORD_ROOM - 1)
                                length = BUILD_WORD_ROOM - 1;

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
                                p8 address_to value = build_text_take(BUILD_WORD_ROOM);
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

                                        if (have > BUILD_WORD_ROOM - 1)
                                                have = BUILD_WORD_ROOM - 1;

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
        string_address artifacts = build_setting_get("artifacts");
        string_address target = build_join(artifacts, "/.config", null);
        string_address information = build_join(artifacts, "/info", null);
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
                        p8 address_to store = build_text_take(BUILD_WORD_ROOM);
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
                                               BUILD_WORD_ROOM);

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
        string_address target;
        string_address temporary = build_join(output, ".asm_tmp", null);
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

//      The spelling printf gives %#x, which is what the packer's line has
//      always shown. Zero would print bare there; nothing here is ever zero,
//      because the caller refuses an image whose base or entry is.
static string_address build_hex(positive value)
{
        p8 address_to into = build_text_take(32);
        positive length;

        into[0] = '0';
        into[1] = 'x';
        length = positive_into_base(into + 2, value, 16, false);
        into[2 + length] = end;

        return (string_address)into;
}

//      A working directory nobody else has. mktemp is one of ours, but its
//      answer arrives on its standard output, and redirecting a tool's output
//      to read it back costs more than the two syscalls the name needs: the
//      process id is what makes it unique and it is already here.
static string_address build_temporary_directory(string_address tag)
{
        string_address root = string_get_environment(environ, "TMPDIR");
        string_address path;

        if (!root || !*root)
                root = "/tmp";

        path = build_join(root, "/", tag, ".", build_number((positive)getpid()),
                          null);

        if (mkdir(path, 0700) < 0 && !build_is_directory(path))
                return null;

        return path;
}

//      Removing a tree is our rm, called rather than spawned. If ours is
//      wrong the build leaves debris, which is the point of using it.
static fn build_remove_tree(string_address path)
{
        build_tool("rm", "-rf", path, null);
}

//      Zeroes from where the region's content ended to where the page it
//      occupies does. Every spark region is a whole number of pages.
static bool build_pad(b32 handle, positive from, positive to)
{
        p8 zeroes[4096];

        memory_zero(zeroes, sizeof(zeroes));

        while (from < to)
        {
                positive want = to - from;
                bipolar put;

                if (want > sizeof(zeroes))
                        want = sizeof(zeroes);

                put = write(handle, zeroes, want);

                if (put <= 0)
                        return false;

                from += (positive)put;
        }

        return true;
}

/*
        Splitting a value that arrived as one word.

        Flag lists ride in the configuration as a single key -- "#> flags -a -b"
        -- and reach a compiler as separate arguments. The shell got that by
        leaving the expansion unquoted, which is also how it got the empty
        string turning into no argument at all rather than one empty one.
*/
static positive build_split(string_address text, string_address address_to into,
                            positive room)
{
        p8 address_to store = build_text_take(BUILD_WORD_ROOM);

        if (!text)
                return 0;

        return build_words_of(text, string_length(text), into, room, store,
                              BUILD_WORD_ROOM);
}

//      Appending split words onto an argument vector under construction.
static positive build_add_split(string_address address_to words, positive count,
                                positive room, string_address text)
{
        string_address pieces[BUILD_ARGUMENT_ROOM];
        positive found = build_split(text, (string_address address_to)pieces,
                                     BUILD_ARGUMENT_ROOM);

        for (positive at = 0; at < found && count + 1 < room; at++)
                words[count++] = pieces[at];

        return count;
}

/*
        Linking a program of this tree's own shape.

        Two link recipes live here because the tree has two kinds of program.
        The freestanding one is an ordinary static ELF and takes link-time
        optimisation. The spark one must not: -flto discards the section
        layout the linker script depends on, and the packer below reads that
        layout back out of the object.

        Neither recipe names this project. The script, the entry symbol and
        the per-architecture flags are settings; a tree with another linker
        script and another entry gets the same two recipes.
*/
static string_address build_compiler()
{
        string_address named = build_key_one("compiler", null);

        if (named && *named)
                return named;

        named = string_get_environment(environ, "CC");

        return named && *named ? named : (string_address)"gcc";
}

/*
        binutils has to match the compiler, not the machine this runs on.

        objdump, readelf and objcopy all read the ELF the compiler just
        produced. Building for another architecture with the host's copies
        gets as far as "objcopy: Unable to recognise the architecture of the
        input file", so the prefix comes off the compiler's own name:
        aarch64-linux-gnu-gcc means aarch64-linux-gnu-objcopy. A plain "gcc"
        leaves the prefix empty, which is the native case.

        Prefer the matching tool, fall back to the host's.
        x86_64-linux-gnu-gcc is a perfectly ordinary way to name a native
        compiler and there is usually no x86_64-linux-gnu-objdump beside it,
        only objdump -- which is the same program. Insisting on the prefix
        breaks the native build to fix the cross one.
*/
static string_address build_binutil(string_address compiler,
                                    string_address name)
{
        p8 address_to prefix = build_text_take(string_length(compiler) + 1);
        positive length = string_length(compiler);
        string_address candidate;

        if (length > 3 && !memory_compare(compiler + length - 3, "gcc", 3))
                length -= 3;
        else if (length > 5 && !memory_compare(compiler + length - 5, "clang", 5))
                length -= 5;
        else if (length > 2 && !memory_compare(compiler + length - 2, "cc", 2))
                length -= 2;

        memory_copy(prefix, compiler, length);
        prefix[length] = end;
        candidate = build_join((string_address)prefix, name, null);

        if (build_have(candidate))
                return candidate;

        if (build_have(name))
                return name;

        string_format(log_error, "spark: neither %s%s nor %s found\n",
                      (string_address)prefix, name, name);
        log_flush();

        return null;
}

/*
        Compiling C to the flat spark format.

        The layout is described in src/spark.c, which the kernel loader
        includes too, so the two sides cannot drift apart -- and this file
        includes it as well, so the header written here is that struct rather
        than a second description of it. Every region is a whole number of
        pages; see the linker script for why.
*/
static bool build_hex_field(string_address text, positive length,
                            positive address_to answer)
{
        positive value = 0;
        positive at = 0;

        if (length > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
                at = 2;

        if (at >= length)
                return false;

        for (; at < length; at++)
        {
                p8 byte = text[at];
                positive digit;

                if (byte >= '0' && byte <= '9')
                        digit = byte - '0';
                else if (byte >= 'a' && byte <= 'f')
                        digit = byte - 'a' + 10;
                else if (byte >= 'A' && byte <= 'F')
                        digit = byte - 'A' + 10;
                else
                        return false;

                value = value * 16 + digit;
        }

        address_to answer = value;

        return true;
}

//      objdump -h has a stable column layout; readelf -S splits "[ 1]" into
//      two fields for single digit indices and one for double, which does not
//      parse. The table is read once: invoking objdump and awk once per field
//      made the packer parse the same ELF five times.
static bool build_section(string_address table, string_address name,
                          positive address_to size, positive address_to where)
{
        p8 address_to store = build_text_take(BUILD_WORD_ROOM);
        build_lines walk;

        address_to size = 0;
        address_to where = 0;

        build_lines_open(address_of walk, table);

        while (build_lines_next(address_of walk))
        {
                string_address words[BUILD_ARGUMENT_ROOM];
                positive count = build_words_of(walk.line, walk.length,
                                                (string_address address_to)words,
                                                BUILD_ARGUMENT_ROOM, store,
                                                BUILD_WORD_ROOM);

                if (count < 4 || !word_is(words[1], name))
                        continue;

                return build_hex_field(words[2], string_length(words[2]), size) &&
                       build_hex_field(words[3], string_length(words[3]), where);
        }

        return true;
}

static positive build_page_up(positive value)
{
        return ((value + SPARK_PAGE - 1) / SPARK_PAGE) * SPARK_PAGE;
}

static b32 build_spark(string_address source, string_address output,
                       string_address mode)
{
        string_address compiler = build_compiler();
        string_address arch = build_key_one("arch", null);
        string_address script = build_setting_get("link_script");
        string_address entry_flag;
        string_address objdump;
        string_address objcopy;
        string_address readelf;
        string_address words[BUILD_ARGUMENT_ROOM];
        string_address work;
        string_address elf;
        string_address text_binary;
        string_address data_binary;
        positive count = 0;
        positive text_bytes = 0;
        positive text_where = 0;
        positive data_bytes = 0;
        positive data_where = 0;
        positive bss_bytes = 0;
        positive bss_where = 0;
        positive base;
        positive entry = 0;
        positive text_end;
        positive text_size;
        positive data_size;
        positive bss_size;
        struct header head;

        if (!arch || !*arch)
        {
                string_format(log_error, "spark: no '#> arch' in %s\n",
                              build_in("artifacts", ".config"));
                log_flush();
                return 1;
        }

        objdump = build_binutil(compiler, "objdump");
        readelf = build_binutil(compiler, "readelf");
        objcopy = build_binutil(compiler, "objcopy");

        if (!objdump || !readelf || !objcopy)
                return 1;

        if (!build_is_file(script))
        {
                string_format(log_error, "spark: missing linker script at %s\n",
                              script);
                log_flush();
                return 1;
        }

        build_label(BUILD_YELLOW, "EXPERIMENTAL! C compiled to spark format");
        string_format(log, BUILD_BOLD "Compiling %s" BUILD_RESET "\n", output);
        log_flush();

        work = build_temporary_directory("spark");

        if (!work)
                return build_die("spark: cannot make a working directory");

        elf = build_join(work, "/image.elf", null);
        text_binary = build_join(work, "/text.bin", null);
        data_binary = build_join(work, "/data.bin", null);
        entry_flag = build_join("-Wl,-e,", build_setting_get("entry"), null);

        words[count++] = compiler;
        words[count++] = build_join(source, ".c", null);
        words[count++] = "-o";
        words[count++] = elf;

        //      -flto discards the section layout the linker script depends on,
        //      and everything below reads that layout back.
        {
                string_address pieces[BUILD_ARGUMENT_ROOM];
                positive found = build_split(build_key("program_flags"),
                                             (string_address address_to)pieces,
                                             BUILD_ARGUMENT_ROOM);

                for (positive at = 0; at < found && count + 1 < BUILD_ARGUMENT_ROOM;
                     at++)
                        if (!word_is(pieces[at], "-flto"))
                                words[count++] = pieces[at];
        }

        if (mode && word_is(mode, "debug"))
                words[count++] = "-g";

        count = build_add_split((string_address address_to)words, count,
                                BUILD_ARGUMENT_ROOM,
                                string_get_environment(environ,
                                                       "SPARK_CPPFLAGS"));

        words[count++] = "-static";
        words[count++] = "-nostdlib";
        words[count++] = "-nostartfiles";
        words[count++] = "-T";
        words[count++] = script;
        words[count++] = "-Wl,--build-id=none";
        words[count++] = entry_flag;
        words[count++] = "-Wl,--no-warn-rwx-segments";
        words[count] = null;

        if (build_run_words((string_address address_to)words, null))
        {
                build_remove_tree(work);
                string_format(log_error, "spark: compilation failed\n");
                log_flush();
                return 1;
        }

        count = 0;
        words[count++] = objdump;
        words[count++] = "-h";
        words[count++] = elf;
        words[count] = null;

        if (build_capture_words((string_address address_to)words, build_file_one,
                                BUILD_FILE_ROOM) < 0)
        {
                build_remove_tree(work);
                return 1;
        }

        build_section((string_address)build_file_one, ".text",
                      address_of text_bytes, address_of text_where);
        build_section((string_address)build_file_one, ".data",
                      address_of data_bytes, address_of data_where);
        build_section((string_address)build_file_one, ".bss",
                      address_of bss_bytes, address_of bss_where);

        count = 0;
        words[count++] = readelf;
        words[count++] = "-h";
        words[count++] = "-W";
        words[count++] = elf;
        words[count] = null;

        if (build_capture_words((string_address address_to)words, build_file_two,
                                BUILD_FILE_ROOM) >= 0)
        {
                p8 address_to store = build_text_take(BUILD_WORD_ROOM);
                build_lines walk;

                build_lines_open(address_of walk, (string_address)build_file_two);

                while (build_lines_next(address_of walk))
                {
                        string_address found[BUILD_ARGUMENT_ROOM];
                        positive parts;

                        if (!memory_search(walk.line, walk.length, "Entry point", 11))
                                continue;

                        parts = build_words_of(walk.line, walk.length,
                                               (string_address address_to)found,
                                               BUILD_ARGUMENT_ROOM, store,
                                               BUILD_WORD_ROOM);

                        if (parts)
                                build_hex_field(found[parts - 1],
                                                string_length(found[parts - 1]),
                                                address_of entry);
                }
        }

        //      The image is mapped from SPARK_HEADER_SIZE bytes before .text:
        //      that is where the header sits, and the linker script reserves
        //      exactly that much.
        base = text_where - SPARK_HEADER_SIZE;

        if (!text_where || !entry)
        {
                build_remove_tree(work);
                string_format(log_error,
                              "spark: could not read base/entry from the linked image\n");
                log_flush();
                return 1;
        }

        //      A program need not have every section: duck has no .data at
        //      all. Text runs up to whichever region actually follows it, or
        //      to its own end if none does.
        if (data_bytes > 0)
                text_end = data_where;
        else if (bss_bytes > 0)
                text_end = bss_where;
        else
                text_end = text_where + text_bytes;

        text_size = build_page_up(text_end - base);
        data_size = build_page_up(data_bytes);
        bss_size = build_page_up(bss_bytes);

        if (!text_size)
        {
                build_remove_tree(work);
                string_format(log_error,
                              "spark: computed a non positive text size (%p)\n",
                              text_size);
                log_flush();
                return 1;
        }

        if (build_run(objcopy, "-O", "binary", "--only-section=.text", elf,
                      text_binary, null))
        {
                build_remove_tree(work);
                return 1;
        }

        if (build_run(objcopy, "-O", "binary", "--only-section=.data", elf,
                      data_binary, null))
                build_write_file(data_binary, "", 0);

        head.magic = SPARK_MAGIC;
        head.version = SPARK_VERSION;
        head.flags = 0;
        head.base = base;
        head.entry = entry;
        head.text_size = text_size;
        head.data_size = data_size;
        head.bss_size = bss_size;
        head.reserved[0] = 0;
        head.reserved[1] = 0;

        {
                b32 handle = open(output, O_WRONLY | O_CREAT | O_TRUNC, 0755);
                bipolar got;
                bool good;

                if (handle < 0)
                {
                        build_remove_tree(work);
                        string_format(log_error, "spark: cannot write %s\n",
                                      output);
                        log_flush();
                        return 1;
                }

                //      The header occupies the first SPARK_HEADER_SIZE bytes of
                //      the text region itself, so the image carries no page
                //      that nothing maps.
                good = write(handle, address_of head, SPARK_HEADER_SIZE) ==
                       SPARK_HEADER_SIZE;
                got = build_slurp(text_binary, build_file_one, BUILD_FILE_ROOM);

                if (got > 0)
                        good = good && write(handle, build_file_one,
                                             (positive)got) == got;

                good = good && build_pad(handle, (positive)(got > 0 ? got : 0),
                                         text_size - SPARK_HEADER_SIZE);

                if (data_size > 0)
                {
                        got = build_slurp(data_binary, build_file_one,
                                          BUILD_FILE_ROOM);

                        if (got > 0)
                                good = good && write(handle, build_file_one,
                                                     (positive)got) == got;

                        good = good && build_pad(handle,
                                                 (positive)(got > 0 ? got : 0),
                                                 data_size);
                }

                close(handle);
                build_remove_tree(work);

                if (!good)
                {
                        string_format(log_error, "spark: writing %s failed\n",
                                      output);
                        log_flush();
                        return 1;
                }
        }

        string_format(log, "spark: base=%s entry=%s text=%p data=%p bss=%p\n",
                      build_hex(base), build_hex(entry), text_size,
                      data_size, bss_size);
        log_flush();
        build_size(output);
        string_format(log, "\n");
        log_flush();

        return 0;
}

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
static b32 build_watch_watcher;

static fn build_stop_application()
{
        if (build_watch_application <= 0)
                return;

        kill(build_watch_application, BUILD_SIGNAL_TERMINATE);
        build_wait(build_watch_application);
        build_watch_application = 0;
}

/*
        Being told to stop.

        Both children are ours and neither ends on its own: the watcher runs
        until it is killed and the replacement runs until it is replaced. A
        watch that exits without taking them leaves two processes spinning on
        somebody's machine, which is what the shell's EXIT/INT/TERM traps were
        there to prevent. The statuses are the shell's too -- 130 for an
        interrupt, 143 for a termination -- because that is what a caller
        reads to tell one from the other.
*/
static fn build_watch_caught(b32 number)
{
        build_stop_application();

        if (build_watch_watcher > 0)
        {
                kill(build_watch_watcher, BUILD_SIGNAL_TERMINATE);
                build_wait(build_watch_watcher);
                build_watch_watcher = 0;
        }

        exit(number == SIGNAL_INTERRUPT ? 130 : 143);
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
                build_watch_watcher = child;
                system_signal_install(SIGNAL_INTERRUPT,
                                      (positive)build_watch_caught,
                                      SIGNAL_CATCH_FLAGS, SIGNAL_CATCH_RESTORER,
                                      null);
                system_signal_install(BUILD_SIGNAL_TERMINATE,
                                      (positive)build_watch_caught,
                                      SIGNAL_CATCH_FLAGS, SIGNAL_CATCH_RESTORER,
                                      null);

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

                if (build_watch_watcher > 0)
                {
                        kill(build_watch_watcher, BUILD_SIGNAL_TERMINATE);
                        build_wait(build_watch_watcher);
                        build_watch_watcher = 0;
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
                p8 address_to into;

                while (version[major] && version[major] != '.')
                        major++;

                into = build_text_take(major + 4);
                into[0] = 'v';
                memory_copy(into + 1, version, major);
                into[major + 1] = '.';
                into[major + 2] = 'x';
                into[major + 3] = end;
                series = (string_address)into;
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
        //      Appended to below, one -D per component that is off.
        string_address flags = "";

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

        //      Ours, asked in a forked child, because the answer is a word
        //      this needs rather than a line somebody reads.
        {
                string_address system = build_tool_answer("uname", null);

                if (!word_is(system, "Linux"))
                        return build_die(build_join(
                                "building a kernel wants a Linux toolchain and a case\n"
                                "sensitive filesystem, and this is ", system,
                                ". Name a machine that has them with\n"
                                "--host, or set MOONWATER_BUILD_HOST.", null));
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

//      grep -qw, for the one-word-per-line answers QEMU gives to -accel help
//      and -display help.
static bool build_word_listed(string_address text, string_address word)
{
        build_lines walk;
        p8 address_to store = build_text_take(BUILD_WORD_ROOM);

        if (!text)
                return false;

        build_lines_open(address_of walk, text);

        while (build_lines_next(address_of walk))
        {
                string_address found[BUILD_ARGUMENT_ROOM];
                positive parts = build_words_of(walk.line, walk.length,
                                                (string_address address_to)found,
                                                BUILD_ARGUMENT_ROOM, store,
                                                BUILD_WORD_ROOM);

                for (positive at = 0; at < parts; at++)
                        if (word_is(found[at], word))
                                return true;
        }

        return false;
}

/*
        The same checksum the shell took of this tree's path.

        One build directory per source tree, not one per machine. This used to
        be one path for everybody: two people, or two sessions, or a person and
        an agent building at the same time wrote their objects and their image
        into the same place and neither was told. An incremental build then
        reuses whatever is there -- the userspace half from one tree and the
        kernel module from another, linked into one image that matches no
        checkout anybody has, and every measurement taken off it is about a
        tree that does not exist.

        The suffix is a checksum of this tree's own path, so the same checkout
        always gets the same directory and two checkouts never share one. It is
        our cksum, called as a function on the bytes rather than run on a file
        written only to be checksummed, and it is the POSIX one, so a directory
        made by the shell build is the directory this finds.
*/
static positive build_path_mark(string_address text)
{
        positive length = string_length(text);
        p32 crc;
        p64 count = length;

        cksum_crc_prepare();
        crc = cksum_crc_block((p8 address_to)text, length, 0);

        while (count)
        {
                p8 byte = (p8)count;

                crc = (crc << 8) ^ cksum_crc_table[0][(crc >> 24) ^ byte];
                count >>= 8;
        }

        return (positive)(p32)~crc;
}

//      ssh takes shell source, not an argument vector. Keep empty words,
//      quotes and newlines intact.
static string_address build_quote(string_address address_to words, positive count)
{
        positive total = 0;
        p8 address_to into;
        p8 address_to at;

        for (positive which = 0; which < count; which++)
                total += string_length(words[which]) * 4 + 4;

        into = build_text_take(total + 1);
        at = into;

        for (positive which = 0; which < count; which++)
        {
                string_address word = words[which];

                if (which)
                        *at++ = ' ';

                *at++ = '\'';

                for (positive step = 0; word[step]; step++)
                {
                        if (word[step] != '\'')
                        {
                                *at++ = word[step];
                                continue;
                        }

                        *at++ = '\'';
                        *at++ = '\\';
                        *at++ = '\'';
                        *at++ = '\'';
                }

                *at++ = '\'';
        }

        *at = end;

        return (string_address)into;
}

/*
        Building somewhere else.

        Only reached when a host was named. The remote command carries no host
        of its own and ssh does not forward the environment, so the build over
        there is an ordinary local one and this cannot recurse.
*/
static string_address build_remote_image;

static b32 build_remote(string_address host, string_address remote,
                        string_address address_to profiles, positive count)
{
        string_address quoted = build_quote(address_of remote, 1);
        string_address arguments = build_quote(profiles, count);
        string_address stock = string_get_environment(environ, "MOONWATER_STOCK");

        build_say(build_join("Checking ", host, null));

        if (build_run("ssh", "-n", "-o", "BatchMode=yes", "-o",
                      "ConnectTimeout=20", host, "true", null))
                return build_die(build_join("cannot reach ", host,
                                            " over ssh", null));

        build_say(build_join("Copying the tree to ", host, ":", remote, null));

        /*
                The kernel source, its artifacts and the built filesystem stay
                on the build host: they are large, and none of them belong to
                this checkout. The upstream tree is not part of this
                repository.
        */
        {
                string_address words[BUILD_ARGUMENT_ROOM];
                string_address excluded = build_setting_get("remote_excludes");
                string_address names[BUILD_ARGUMENT_ROOM];
                p8 address_to store = build_text_take(BUILD_WORD_ROOM);
                positive many = build_words_of(excluded, string_length(excluded),
                                               (string_address address_to)names,
                                               BUILD_ARGUMENT_ROOM, store,
                                               BUILD_WORD_ROOM);
                positive at = 0;

                words[at++] = "rsync";
                words[at++] = "-az";
                words[at++] = "--delete";
                words[at++] = build_join("--rsync-path=mkdir -p -- ", quoted,
                                         " && cd -- ", quoted, " && rsync", null);

                for (positive which = 0; which < many; which++)
                {
                        words[at++] = "--exclude";
                        words[at++] = names[which];
                }

                words[at++] = "./";
                words[at++] = build_join(host, ":./", null);
                words[at] = null;

                if (build_run_words((string_address address_to)words, null))
                        return build_die("copying the tree failed");
        }

        build_say(build_join("Building on ", host, ": ", arguments, null));

        /*
                -n so the build does not swallow this program's stdin. Without
                it the USB prompts read nothing, because ssh forwards whatever
                is on stdin to the remote command.

                sudo drops the environment, so anything the remote build has to
                know is named here. env rather than a VAR=value prefix, which
                sudo only passes when it has been configured to.
        */
        if (build_run("ssh", "-n", host,
                      build_join("cd -- ", quoted, " && sudo env ",
                                 stock && *stock ? "MOONWATER_STOCK=1" : "",
                                 " sh build.sh ", arguments, null),
                      null))
                return build_die(build_join("the build failed on ", host, null));

        /*
                The host which built the configured profile is authoritative
                about its export. A stale local configuration may describe
                another architecture entirely -- an ARM Mac commonly names
                kernel8.img.
        */
        {
                string_address words[8];
                positive at = 0;

                words[at++] = "ssh";
                words[at++] = "-n";
                words[at++] = host;
                words[at++] = build_join("cd -- ", quoted,
                                         " && ./build key-one kernel_export", null);
                words[at] = null;

                if (build_capture_words((string_address address_to)words,
                                        build_file_two, BUILD_FILE_ROOM) < 0)
                        return build_die("could not identify the built image");

                {
                        positive length = string_length((string_address)build_file_two);

                        while (length && (build_file_two[length - 1] == '\n' ||
                                          build_file_two[length - 1] == '\r'))
                                build_file_two[--length] = end;

                        build_remote_image = build_join((string_address)build_file_two,
                                                        null);
                }
        }

        {
                string_address expected = build_join(build_setting_get("output"),
                                                     "/", null);
                positive length = string_length(expected);

                if (string_length(build_remote_image) <= length ||
                    memory_compare(build_remote_image, expected, length))
                        return build_die(build_join(
                                "remote build reported an invalid image path: ",
                                build_remote_image, null));
        }

        build_say(build_join("Fetching ", build_remote_image, null));
        build_tool("mkdir", "-p", build_directory_of(build_remote_image), null);

        {
                string_address words[8];
                positive at = 0;
                b32 handle;
                b32 child;

                words[at++] = "ssh";
                words[at++] = "-n";
                words[at++] = host;
                words[at++] = build_join("cd -- ", quoted, " && cat -- ",
                                         build_quote(address_of build_remote_image, 1),
                                         null);
                words[at] = null;

                handle = open(build_remote_image, O_WRONLY | O_CREAT | O_TRUNC,
                              0644);

                if (handle < 0)
                        return build_die("could not fetch the built image");

                log_flush();
                child = fork();

                if (child == 0)
                {
                        string_address found = build_resolve(words[0]);

                        dup2(handle, 1);
                        close(handle);

                        if (found)
                                execve(found, (string_address address_to)words,
                                       environ);

                        exit(127);
                }

                close(handle);

                if (build_wait(child))
                        return build_die("could not fetch the built image");
        }

        return 0;
}

/*
        Removing what a build produced.

        The artifacts directory keeps the downloaded kernel tarball and is left
        alone on purpose: throwing it away means fetching a hundred and fifty
        megabytes again to get back where you were.
*/
static b32 build_clean()
{
        string_address artifacts = build_setting_get("artifacts");
        string_address leftovers = build_setting_get("clean_patterns");
        string_address names[BUILD_ARGUMENT_ROOM];
        p8 address_to store = build_text_take(BUILD_WORD_ROOM);
        positive many;

        build_say("Removing build output");

        build_tool("rm", "-rf", build_setting_get("output"),
                   build_setting_get("image_root"),
                   build_setting_get("kernel_tree"),
                   build_join(artifacts, "/merge.config", null),
                   build_join(artifacts, "/.config", null),
                   build_join(artifacts, "/info", null),
                   build_join(artifacts, "/asm.applied", null),
                   build_join(artifacts, "/asm.arch", null),
                   build_join(artifacts, "/asm.requested", null), null);

        /*
                The build products beside the module's source, including the
                .S each .asm becomes -- which kbuild writes there because that
                directory is the kernel tree's own module directory.

                The patterns begin [!.] because a shell glob does not match a
                leading dot and find's -name does. kbuild's own .o.cmd files
                are dotfiles and the shell never removed them; neither does
                this.
        */
        many = build_words_of(leftovers, string_length(leftovers),
                              (string_address address_to)names,
                              BUILD_ARGUMENT_ROOM, store, BUILD_WORD_ROOM);

        for (positive at = 0; at < many; at++)
                build_tool("find", build_setting_get("module_root"), "-maxdepth",
                           "1", "-name", names[at], "-delete", null);

        return 0;
}

/*
        Writing to a USB stick.

        The image is already an EFI application -- the kernel is built with the
        EFI stub, which is why it is called bootx64.efi -- so firmware can load
        it directly and there is no bootloader to install. It goes at the path
        the UEFI spec reserves for removable media, \\EFI\\BOOT\\BOOTX64.EFI,
        which is what a machine looks for when told to boot from USB.

        This lists the candidates and prints the command rather than running
        it. Writing to a raw block device with the wrong name destroys the
        wrong disk, and there is no honest way to claim care against an
        untested lsblk and dd, so the last step stays in your hands.
*/
static b32 build_usb(string_address image)
{
        build_say("Removable disks");

        if (build_have("lsblk"))
        {
                string_address words[8];
                positive at = 0;
                build_lines walk;
                p8 address_to store = build_text_take(BUILD_WORD_ROOM);

                words[at++] = "lsblk";
                words[at++] = "-dno";
                words[at++] = "NAME,SIZE,RM,MODEL";
                words[at] = null;

                if (build_capture_words((string_address address_to)words,
                                        build_file_one, BUILD_FILE_ROOM) >= 0)
                {
                        build_lines_open(address_of walk,
                                         (string_address)build_file_one);

                        while (build_lines_next(address_of walk))
                        {
                                string_address found[BUILD_ARGUMENT_ROOM];
                                positive parts = build_words_of(
                                        walk.line, walk.length,
                                        (string_address address_to)found,
                                        BUILD_ARGUMENT_ROOM, store,
                                        BUILD_WORD_ROOM);

                                if (parts < 3 || !word_is(found[2], "1"))
                                        continue;

                                string_format(log, "  /dev/%s  %s  %s\n",
                                              found[0], found[1],
                                              parts > 3 ? found[3]
                                                        : (string_address)"");
                        }
                }
        }
        else
                string_format(log, "  (lsblk is missing; find the device yourself)\n");

        string_format(log, "\n");
        string_format(log, "Write it with, replacing sdX with the stick:\n");
        string_format(log, "\n");
        string_format(log,
                      "  sudo mkfs.vfat -F32 /dev/sdX1        # after partitioning it GPT/ESP\n");
        string_format(log, "  sudo mount /dev/sdX1 /mnt\n");
        string_format(log, "  sudo mkdir -p /mnt/EFI/BOOT\n");
        string_format(log, "  sudo cp %s /mnt/EFI/BOOT/BOOTX64.EFI\n", image);
        string_format(log, "  sudo umount /mnt\n");
        string_format(log, "\n");
        string_format(log,
                      "Check the device name twice. This erases whatever it names.\n");
        log_flush();

        return 0;
}

/*
        Booting it here.

        Every one of these flags is a requirement rather than a preference, and
        they are settings so another tree can boot its own image with its own:

        virtio-gpu rather than the default VGA, because it is the only device
        here that offers a hardware cursor plane, which is what lets the
        compositor move the pointer without repainting anything. usb-tablet
        reports absolute positions, so the pointer inside the guest follows the
        one on the host instead of drifting. -vga none matters: without it QEMU
        also creates a standard VGA device, the window shows that one because
        it is the boot VGA, and the compositor ends up drawing on the other
        card where nobody can see it.

        -cpu Nehalem, not the default. The kernel is compiled -march=x86-64-v2,
        whose floor is Nehalem, and QEMU's default model is qemu64 -- SSE3-era,
        no POPCNT. There are 334 popcnt instructions in vmlinux, so on the
        default model the image takes an invalid opcode before the console
        exists and prints nothing whatsoever. This line is what stands between
        that and here.

        drm_client_lib.active= on the command line stops the fbdev client
        claiming the display. It has to be built, but it must not take the
        screen, or the compositor is drawing underneath something else.
*/
static b32 build_boot(string_address image, bool console)
{
        string_address emulator = build_setting_get("emulator");
        string_address words[BUILD_ARGUMENT_ROOM];
        string_address accelerators = "";
        string_address display = null;
        positive count = 0;

        if (!build_have(emulator))
                return build_die(build_join(emulator, " is not installed", null));

        build_say(build_join("Booting ", image, null));
        build_size(image);

        words[count++] = emulator;
        count = build_add_split((string_address address_to)words, count,
                                BUILD_ARGUMENT_ROOM,
                                build_setting_get("emulator_flags"));
        words[count++] = "-kernel";
        words[count++] = image;
        count = build_add_split((string_address address_to)words, count,
                                BUILD_ARGUMENT_ROOM,
                                build_setting_get("emulator_devices"));

        //      Hardware acceleration where this QEMU has it. -cpu host
        //      replaces the model above, which is what you want when the guest
        //      is running on the real one.
        {
                string_address ask[4];

                ask[0] = emulator;
                ask[1] = "-accel";
                ask[2] = "help";
                ask[3] = null;

                if (build_capture_words((string_address address_to)ask,
                                        build_file_one, BUILD_FILE_ROOM) >= 0)
                        accelerators = (string_address)build_file_one;
        }

        if (build_word_listed(accelerators, "hvf"))
        {
                words[count++] = "-accel";
                words[count++] = "hvf";
                words[count++] = "-cpu";
                words[count++] = "host";
        }
        else if (build_word_listed(accelerators, "kvm") &&
                 access("/dev/kvm", W_OK) >= 0)
        {
                words[count++] = "-accel";
                words[count++] = "kvm";
                words[count++] = "-cpu";
                words[count++] = "host";
        }

        words[count++] = "-append";
        words[count++] = build_setting_get("kernel_cmdline");

        if (console)
        {
                build_say("Console on this terminal, ctrl-a x to quit");
                words[count++] = "-display";
                words[count++] = "none";
                words[count++] = "-serial";
                words[count++] = "mon:stdio";
                words[count] = null;

                return build_run_words((string_address address_to)words, null);
        }

        {
                string_address ask[4];
                string_address available = "";

                ask[0] = emulator;
                ask[1] = "-display";
                ask[2] = "help";
                ask[3] = null;

                if (build_capture_words((string_address address_to)ask,
                                        build_file_two, BUILD_FILE_ROOM) >= 0)
                        available = (string_address)build_file_two;

                if (build_word_listed(available, "gtk"))
                        display = "gtk";
                else if (build_word_listed(available, "sdl"))
                        display = "sdl";
                else
                        return build_die(
                                "this QEMU has no graphical display backend -- use --shell");
        }

        build_say("Window opening, ctrl-alt-g releases the mouse");
        words[count++] = "-display";
        words[count++] = display;
        words[count++] = "-serial";
        words[count++] = "mon:stdio";
        words[count] = null;

        return build_run_words((string_address address_to)words, null);
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
                      "\n"
                      "--set name=value overrides one setting, anywhere on the line.\n"
                      "    build key-one <name>                    the same, refusing two\n"
                      "    build size <path>                       bytes, KB and MB\n");
        log_flush();
}

static string_address build_words_kept[BUILD_ARGUMENT_ROOM];

b32 main()
{
        string_address address_to arguments = program_argument_list();
        positive count = (positive)program_argument_count();
        string_address command;

        /*
                --set name=value, read before anything else looks at the
                arguments, so it reaches every sub-command and not only the
                build. The words are taken out of the vector here rather than
                skipped in each parser below, which is the whole reason this
                is one pass over the arguments and not five.
        */
        build_settings_read();

        {
                positive kept = 0;

                for (positive at = 0; at < count && kept + 1 < BUILD_ARGUMENT_ROOM;
                     at++)
                {
                        string_address word = arguments[at];
                        string_address pair = null;
                        p8 address_to cut;

                        if (at && word_is(word, "--set") && at + 1 < count)
                                pair = arguments[++at];
                        else if (at && !memory_compare(word, "--set=", 6))
                                pair = word + 6;
                        else
                        {
                                build_words_kept[kept++] = word;
                                continue;
                        }

                        pair = build_join(pair, null);
                        cut = (p8 address_to)string_first_of(pair, '=');

                        if (!cut)
                                return build_die("--set wants name=value");

                        address_to cut = end;
                        build_setting_set(pair, (string_address)(cut + 1));
                }

                build_words_kept[kept] = null;
                arguments = (string_address address_to)build_words_kept;
                count = kept;
        }

        command = count > 1 ? arguments[1] : null;

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

        if (command && word_is(command, "spark"))
        {
                build_is_safe();
                build_config_load();

                if (count < 4)
                {
                        string_format(log_error,
                                      "spark: usage: spark <source_without_extension> <output> [debug]\n");
                        log_flush();
                        return 1;
                }

                return build_spark(arguments[2], arguments[3],
                                   count > 4 ? arguments[4] : null);
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

        if (command && word_is(command, "freestanding"))
        {
                build_config_load();

                return build_freestanding((string_address address_to)(arguments + 2),
                                          count - 2);
        }

        if (command && word_is(command, "floor"))
        {
                build_is_safe();

                return build_floor(count > 2 ? arguments[2] : null);
        }

        if (command && (word_is(command, "--help") || word_is(command, "-h")))
        {
                build_usage();
                return 0;
        }

        /*
                The build.

                Anything that is not an option is a profile name, so the two
                can be mixed in any order: `build --run desktop`.
        */
        {
                string_address profiles[BUILD_ARGUMENT_ROOM];
                string_address host = string_get_environment(environ,
                                                             "MOONWATER_BUILD_HOST");
                string_address remote = string_get_environment(environ,
                                                               "MOONWATER_BUILD_DIR");
                string_address image = null;
                positive chosen = 0;
                bool run = false;
                bool make = true;
                bool clean = false;
                bool usb = false;
                bool console = false;

                build_is_safe();

                for (positive at = 1; at < count; at++)
                {
                        string_address word = arguments[at];

                        if (word_is(word, "--clean"))
                                clean = true;
                        else if (word_is(word, "--run"))
                                run = true;
                        else if (word_is(word, "--boot"))
                        {
                                run = true;
                                make = false;
                        }
                        else if (word_is(word, "--shell"))
                                console = true;
                        else if (word_is(word, "--usb"))
                                usb = true;
                        else if (word_is(word, "--host"))
                        {
                                if (at + 1 >= count)
                                        return build_die(
                                                "--host wants a machine to build on");

                                host = arguments[++at];
                        }
                        else if (!memory_compare(word, "--host=", 7))
                                host = word + 7;
                        else if (word[0] == '-' && word[1] == '-')
                                return build_die(build_join("unknown option ",
                                                            word, null));
                        else if (chosen + 1 < BUILD_ARGUMENT_ROOM)
                                profiles[chosen++] = word;
                }

                profiles[chosen] = null;

                if (!remote || !*remote)
                        remote = build_join("/tmp/", build_setting_get("name"),
                                            "-",
                                            build_name_of(build_working_directory()),
                                            "-",
                                            build_number(build_path_mark(
                                                    build_working_directory())),
                                            null);

                if (clean)
                        return build_clean();

                build_config_load();

                if (make)
                {
                        if (host && *host)
                        {
                                if (build_remote(host, remote,
                                                 (string_address address_to)profiles,
                                                 chosen))
                                        return 1;

                                image = build_remote_image;
                        }
                        else if (build_local((string_address address_to)profiles,
                                             chosen))
                                return 1;
                }

                if (!usb && !run)
                        return 0;

                /*
                        Where the image ended up. A remote build set this from
                        its own generated configuration. A local build, or
                        --boot without a build, asks the local configuration
                        and finally falls back to the default export.
                */
                if (!image)
                {
                        build_config_load();
                        image = build_key_one("kernel_export", null);
                }

                if (!image || !*image)
                        image = build_setting_get("default_image");

                if (!build_is_file(image))
                        return build_die(build_join("no image at ", image,
                                                    " -- build one first, or drop --boot",
                                                    null));

                if (usb)
                        return build_usb(image);

                return build_boot(image, console);
        }
}
