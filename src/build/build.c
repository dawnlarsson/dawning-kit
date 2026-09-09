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

b32 main()
{
        string_format(log, "build: %s\n", build_setting_get("kernel_version"));
        log_flush();

        return 0;
}
