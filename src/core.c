#include <linux/module.h>
#include <linux/init.h>
#include <linux/namei.h>
#include <linux/binfmts.h>
#include <linux/sched/task_stack.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/mman.h>
#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/sched/task.h>
#include <linux/initrd.h>
#include <linux/console.h>
#include <linux/refcount.h>
#include <linux/cred.h>
#include <linux/file.h>
// Before the library's own spellings below: poll.h names a bool of its own.
#include <linux/poll.h>
#include <linux/kernel_stat.h>
#include <linux/cpumask.h>
#include <linux/timekeeping.h>
#include <linux/sched/loadavg.h>
#include <linux/swap.h>
#include <linux/netdevice.h>
#include <linux/nsproxy.h>
#include <net/net_namespace.h>
// Bindings: the machine's own keys and events, not the compositor's.
#include <linux/input.h>
#include <linux/reboot.h>
#include <linux/kmod.h>
#include <linux/sched/signal.h>
#include <linux/notifier.h>
#include <linux/workqueue.h>
#ifdef CONFIG_VT
#include <linux/keyboard.h>
#include <linux/vt_kern.h>
#endif
#ifdef CONFIG_PM
#include <linux/suspend.h>
#endif
#include <linux/io.h>
#ifdef CONFIG_EFI
#include <linux/efi.h>
#endif

#ifdef CONFIG_X86_64
#include <asm/cpufeature.h>
#include <asm/fpu/xcr.h>
#elif defined(CONFIG_ARM64)
#include <asm/cpufeature.h>
#elif defined(CONFIG_RISCV)
#include <asm/cpufeature.h>
#include <asm/vector.h>
#endif

// The graphics headers must precede library.c: it defines "end" as a macro
// and asm/io.h, reached through drm_client.h, uses that word as a variable.
#ifdef CONFIG_MOONWATER_CANVAS
#include <drm/drm_client.h>
#include <drm/drm_crtc.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_mode.h>
#include <drm/drm_modes.h>
#include <drm/drm_connector.h>
#include <drm/drm_modeset_lock.h>
#include <drm/drm_plane.h>
#include <drm/drm_print.h>
// The console keyboard's mode, which Canvas turns off while it has the keys.
#ifdef CONFIG_VT
#include <linux/kd.h>
#include <linux/vt_kern.h>
#endif
#endif

#define STANDARD_MODERN_C_KERNEL
#include "compiler_memory.c"
#include "spark.c"

// Defined below, next to the rest of the spawning, and called only by the
// compositor when it has a screen to put something on.
#ifdef CONFIG_MOONWATER_CANVAS
static int spawn_terminal(void);
#endif

struct spawn_strings
{
        refcount_t references;
        char **vector;
};

struct pane;

/* One open device is one independent launch/cache and window context. */
struct device_context
{
        struct mutex spawn_lock;
        struct spawn_strings *environment;
        unsigned long environment_generation;
        struct pid *environment_owner;
        /*
                The credentials the cached environment was copied under.

                tgid survives execve, so it alone cannot tell "the same shell
                asking again" from "that shell after it exec'd something
                setuid". Without this a non-CLOEXEC descriptor carried across
                a privilege change lets the stale, unprivileged environment be
                substituted for the one the now-privileged caller passed in.

                A cred is immutable and refcounted, so identity is the pointer
                and the fast path stays a single compare.
        */
        const struct cred *environment_cred;
        struct pane *pane;
};

#ifdef CONFIG_MOONWATER_CANVAS
#include <linux/workqueue.h>
#include <linux/kthread.h>
#include <linux/rtmutex.h>
#include <uapi/linux/sched/types.h>
#include <linux/pm_qos.h>
#include <linux/input.h>
#include <linux/math64.h>
#include <linux/minmax.h>
#include <linux/list_sort.h>
#include <linux/hrtimer.h>
#include <linux/font.h>
#include <drm/drm_file.h>
#include <drm/drm_rect.h>
static void bind_fire(unsigned int event);
static _Bool bind_key_swallowed(unsigned int code, int value);
#include "canvas/canvas.c"
#endif

// The assembly in this directory. Each .asm is its own object -- assembly
// cannot be included into this translation unit the way display.c is
// -- so the compiler is told its shape here, in the file that calls it.
//

int path_mount(const char *dev_name, struct path *path,
               const char *type_page, unsigned long flags, void *data_page);

// Declared rather than included: linux/init_syscalls.h also declares an
// init_mount, and this file has one of its own.
int init_mkdir(const char *pathname, umode_t mode);


/* Caller-triggered failures are rate limited; boot and teardown are not. */

typedef struct
{
        string_address filesystem;
        string_address path;
        positive mount_flags;

} MountPoints;

/*
        devtmpfs is what populates /dev. The kernel will not mount it itself
        when booting from an initramfs, so without this /dev holds only the
        handful of nodes the initramfs was built with -- no /dev/dri, and so
        nothing for the compositor to open.
*/
static const MountPoints mounts[] = {
    {"proc", "/proc", 0},
    {"sysfs", "/sys", 0},
    {"devtmpfs", "/dev", 0},

    /*
            Not devpts. It registers itself with module_init, which for
            built-in code is device_initcall -- the level this file starts at,
            and kernel/ links before fs/. Even late_initcall was too early.
            init mounts it, which is where a system does it anyway, and it
            needs no pty before then.
    */
    {null, null},
};

// Callers and spawned workers update these counters concurrently.
static atomic_long_t stat_spawns = ATOMIC_LONG_INIT(0);
static atomic_long_t stat_task_ns = ATOMIC_LONG_INIT(0);
static atomic_long_t stat_exec_ns = ATOMIC_LONG_INIT(0);
static atomic_long_t stat_loader_ns = ATOMIC_LONG_INIT(0);
static atomic_long_t stat_loads = ATOMIC_LONG_INIT(0);
static atomic_long_t stat_map_ns = ATOMIC_LONG_INIT(0);

static int execute_spark(struct linux_binprm *bprm);

#ifdef CONFIG_X86_64
static unsigned long __ro_after_init spark_cpu_features;

/* Cache enabled instruction/state capabilities once, not on every exec.
   xgetbv touches only general registers and runs after the OSXSAVE gate. */
static void __init spark_cpu_features_start(void)
{
        /* xmm state is always saved, so these two need no state gate. */
        if (cpu_feature_enabled(X86_FEATURE_PCLMULQDQ))
                spark_cpu_features |= SPARK_CPU_PCLMUL;
        if (cpu_feature_enabled(X86_FEATURE_AES))
                spark_cpu_features |= SPARK_CPU_AES;
        if (!cpu_feature_enabled(X86_FEATURE_OSXSAVE) ||
            !cpu_feature_enabled(X86_FEATURE_AVX))
                return;

        u64 state = xgetbv(XCR_XFEATURE_ENABLED_MASK);
        if ((state & 6) != 6)
                return;
        if (cpu_feature_enabled(X86_FEATURE_VPCLMULQDQ))
                spark_cpu_features |= SPARK_CPU_VPCLMUL;
        if (cpu_feature_enabled(X86_FEATURE_VAES))
                spark_cpu_features |= SPARK_CPU_VAES;
        if (cpu_feature_enabled(X86_FEATURE_FMA))
                spark_cpu_features |= SPARK_CPU_FMA;
        if (!cpu_feature_enabled(X86_FEATURE_AVX2))
                return;
        spark_cpu_features |= SPARK_CPU_AVX2;
        if ((state & 0xe6) == 0xe6 &&
            cpu_feature_enabled(X86_FEATURE_AVX512F) &&
            cpu_feature_enabled(X86_FEATURE_AVX512BW) &&
            cpu_feature_enabled(X86_FEATURE_AVX512VL))
        {
                spark_cpu_features |= SPARK_CPU_AVX512;
                if (cpu_feature_enabled(X86_FEATURE_AVX512VBMI))
                        spark_cpu_features |= SPARK_CPU_AVX512_VBMI;
        }
}
#endif

#if defined(CONFIG_ARM64)
static unsigned long __ro_after_init spark_cpu_features;

/* The Boolean byte lanes from bit 0, as spark.inc lays them out: PMULL,
   then AES. Both are system capabilities, final once the CPUs are up. */
static void __init spark_cpu_features_start(void)
{
        if (cpu_have_named_feature(PMULL))
                spark_cpu_features |= SPARK_CPU_CLMUL_LANE;
        if (cpu_have_named_feature(AES))
                spark_cpu_features |= SPARK_CPU_AES_LANE;
}
#elif defined(CONFIG_RISCV)
/* Zbc, then Zvkned with the vector state this task may use. Asked at each
   exec rather than once, because whether userspace may touch V is a per-task
   control. */
static unsigned long spark_cpu_features_now(void)
{
        unsigned long lanes = 0;

        if (riscv_has_extension_unlikely(RISCV_ISA_EXT_ZBC))
                lanes |= SPARK_CPU_CLMUL_LANE;
        if (has_vector() && riscv_v_vstate_ctrl_user_allowed() &&
            riscv_has_extension_unlikely(RISCV_ISA_EXT_ZVKNED))
                lanes |= SPARK_CPU_AES_LANE;
        return lanes;
}
#endif

/*
        The top of the address space belongs to the stack, and an image may
        not be mapped into it. Generous on purpose: this only has to be larger
        than any stack setup_arg_pages will build, and being larger costs an
        image nothing -- there is no machine where a flat binary wants to live
        within a gigabyte of STACK_TOP.
*/
#define SPARK_STACK_ROOM (1UL << 30)

static struct linux_binfmt format = {
    .module = THIS_MODULE,
    .load_binary = execute_spark,
};

/*
        The vector a program finds its arguments through.

        setup_arg_pages copies the strings onto the new stack and leaves
        bprm->p pointing at the first of them, but that is all it does. What
        was missing is the thing create_elf_tables builds for an ELF: below the
        strings, a count, then a pointer to each argument, a null, then a
        pointer to each environment entry, and another null. Without it the
        stack pointer a program starts on points at raw text, so reading the
        count read the first eight bytes of its own path -- which is why every
        main() here took no arguments and the shell had nowhere to send what it
        had parsed.

        No auxiliary vector. A spark image is mapped at a fixed base by the
        loader above and has no interpreter to inform, which is the whole of
        what auxv is for here.
*/
static int spark_stack(struct linux_binprm *bprm, unsigned long *out)
{
        unsigned long walk = bprm->p;
        unsigned long __user *slot;
        unsigned long bottom;
        int count = bprm->argc + bprm->envc;
        int i;

        // A count, every pointer, and the two nulls that end each list.
        bottom = (walk - (unsigned long)(count + 3) * sizeof(unsigned long)) & ~15UL;
        slot = (unsigned long __user *)bottom;

        if (put_user((unsigned long)bprm->argc, slot++))
                return -EFAULT;

        for (i = 0; i < count; i++)
        {
                long length;

                if (i == bprm->argc && put_user(0UL, slot++))
                        return -EFAULT;

                if (put_user(walk, slot++))
                        return -EFAULT;

                length = strnlen_user((void __user *)walk, MAX_ARG_STRLEN);

                if (length <= 0)
                        return -EFAULT;

                walk += (unsigned long)length;
        }

        // The null after argv when there was no environment to start one.
        if (!bprm->envc && put_user(0UL, slot++))
                return -EFAULT;

        if (put_user(0UL, slot))
                return -EFAULT;

        *out = bottom;
        return 0;
}

/* Keeping one epilogue lets the cheap format-rejection gate precede all work;
   GCC shrink wrapping otherwise emits one restore island per validation exit. */
static __attribute__((optimize("no-shrink-wrap-separate")))
int execute_spark(struct linux_binprm *bprm)
{
        u64 loader_started;
        u64 map_started;
        struct pt_regs *regs;
        const struct header *header;
        unsigned long stack_addr, span, address, populate[3] = {0};
        int ret;

        // Everything up to begin_new_exec runs while the old process is still
        // intact, so a file that is not ours must be rejected here: returning
        // -ENOEXEC lets the next handler try, and leaves the caller alive.
        // The kernel has already read the first BINPRM_BUF_SIZE bytes for us.
        header = (const struct header *)bprm->buf;

        if (header->magic != SPARK_MAGIC)
                return -ENOEXEC;

        loader_started = ktime_get_ns();
        regs = task_pt_regs(current);

        if (header->version != SPARK_VERSION)
        {
                pr_alert_ratelimited("[moonwater] " "unsupported spark version %u\n", header->version);
                return -ENOEXEC;
        }

        if (header->flags != 0)
                return -ENOEXEC;

        // Every region is a page multiple by construction, and the entry has
        // to land inside the text it points into. A malformed image must fail
        // here rather than after the mm has been torn down.
        if (header->base == 0 || (header->base & (SPARK_PAGE - 1)))
                return -ENOEXEC;

        if (header->text_size == 0 || (header->text_size & (SPARK_PAGE - 1)))
                return -ENOEXEC;

        if ((header->data_size & (SPARK_PAGE - 1)) || (header->bss_size & (SPARK_PAGE - 1)))
                return -ENOEXEC;

        /*
                Every size below comes from the file, so the arithmetic has to
                assume it is hostile. Adding two of them can wrap, and a wrapped
                sum compares small enough to pass a bound it should have failed.
                Each total is therefore checked against what remains rather than
                being formed first.

                SPARK_MAX_IMAGE is not a real limit on anything: it is far more
                than a flat binary has any business being, and it means the
                sums below cannot come near overflowing.
        */
        if (header->text_size > SPARK_MAX_IMAGE ||
            header->data_size > SPARK_MAX_IMAGE ||
            header->bss_size > SPARK_MAX_IMAGE)
                return -ENOEXEC;

        span = header->text_size + header->data_size + header->bss_size;

        if (span > SPARK_MAX_IMAGE)
                return -ENOEXEC;

        // The whole image has to fit above base without wrapping, and inside
        // the address space the process will actually have.
        if (header->base > TASK_SIZE || span > TASK_SIZE - header->base)
                return -ENOEXEC;

        /*
                And not where the stack is about to be.

                setup_arg_pages builds the stack below, before the three
                regions go up, and those are MAP_FIXED: an image based high
                enough is mapped straight over the stack that was just built
                for it. Text landing there is caught by accident, because
                put_user then faults against a read-only mapping and the task
                dies. Data landing there is not caught at all -- that region
                is writable, so the argument vector is written into the
                program's own data and the program is started on a stack
                pointer inside its own image.

                Every other field is checked against something; this was the
                one piece of geometry taken on trust. Refused here rather than
                after begin_new_exec, because here there is still a caller to
                return -ENOEXEC to.

                A fixed reserve rather than RLIMIT_STACK: the limit is what
                the stack may grow to, and reading it here would tie this
                check to a value the caller chooses. Any initial stack fits
                far inside a gigabyte, and growth past it is the guard gap's
                job rather than this one's.
        */
        if (STACK_TOP > SPARK_STACK_ROOM)
        {
                unsigned long floor = STACK_TOP - SPARK_STACK_ROOM;

                if (header->base >= floor || span > floor - header->base)
                        return -ENOEXEC;
        }

        if (header->entry < header->base ||
            header->entry - header->base >= header->text_size)
                return -ENOEXEC;

        // What must be present in the file, as opposed to zero filled.
        if (i_size_read(file_inode(bprm->file)) <
            (loff_t)(header->text_size + header->data_size))
                return -ENOEXEC;

        // Past this point the old mm is gone. Nothing below may return a plain
        // error code -- there is no process left to return it to -- so every
        // failure has to kill the task instead.
        ret = begin_new_exec(bprm);
        if (ret)
                return ret;

        setup_new_exec(bprm);

        ret = setup_arg_pages(bprm, STACK_TOP, EXSTACK_DEFAULT);
        if (ret < 0)
        {
                pr_alert_ratelimited("[moonwater] " "setup_arg_pages failed: %d\n", ret);
                goto fatal;
        }

        map_started = ktime_get_ns();

        // vm_mmap takes and drops mmap_lock around every call. This address
        // space was created moments ago and nothing else can see it yet, so
        // the three regions go up under one write lock instead of three
        // acquire/release cycles, using do_mmap directly.
        //
        // do_mmap does not populate; it reports how much wants populating and
        // the caller does it after dropping the lock.
        ret = mmap_write_lock_killable(current->mm);
        if (ret)
                goto fatal;

        const unsigned long sizes[] = {
                header->text_size, header->data_size, header->bss_size
        };
        static const char *const regions[] = {"text", "data", "bss"};
        address = header->base;
        for (unsigned int i = 0; i < array_count(sizes); i++)
        {
                if (!sizes[i])
                        continue;

                // Bss is anonymous and already zero. File regions carry their
                // offset from the image base; all three share one lock scope.
                unsigned long mapped = do_mmap(i == 2 ? NULL : bprm->file,
                    address, sizes[i], PROT_READ | (i ? PROT_WRITE : PROT_EXEC),
                    MAP_PRIVATE | MAP_FIXED | (i == 2 ? MAP_ANONYMOUS : 0),
                    0, i == 2 ? 0 : (address - header->base) >> PAGE_SHIFT,
                    &populate[i], NULL);
                if (IS_ERR_VALUE(mapped))
                {
                        mmap_write_unlock(current->mm);
                        ret = (int)mapped;
                        pr_alert_ratelimited("[moonwater] " "mapping %s failed: %d\n", regions[i], ret);
                        goto fatal;
                }
                address += sizes[i];
        }
        mmap_write_unlock(current->mm);

        // do_mmap reports eager population separately from establishing the
        // mappings. Keep the same walk for all regions, including anonymous bss.
        address = header->base;
        for (unsigned int i = 0; i < array_count(sizes); i++)
        {
                if (populate[i])
                        mm_populate(address, populate[i]);
                address += sizes[i];
        }

        atomic_long_add(ktime_get_ns() - map_started, &stat_map_ns);

        current->mm->start_code = header->base;
        current->mm->end_code = header->base + header->text_size;
        current->mm->start_data = header->base + header->text_size;
        current->mm->end_data = header->base + header->text_size + header->data_size;
        current->mm->brk = current->mm->start_brk =
            header->base + header->text_size + header->data_size + header->bss_size;

        set_binfmt(&format);

        ret = spark_stack(bprm, &stack_addr);

        if (ret)
        {
                pr_alert_ratelimited("[moonwater] " "could not lay out the arguments: %d\n", ret);
                goto fatal;
        }

#ifdef CONFIG_X86_64
        /* CPUID and XGETBV are serialising startup work whose answer the
           kernel already has.  Spark's private entry ABI hands that answer
           to _start; an image run by an older loader simply misses the magic
           and retains its userspace detection fallback. */
        regs->r12 = SPARK_START_MAGIC;
        regs->r13 = spark_cpu_features;
        regs->r14 = task_pid_nr(current);

        regs->ip = header->entry;
        regs->sp = stack_addr;
        regs->flags = 0x202; // IF flag set
        regs->cs = __USER_CS;
        regs->ss = __USER_DS;
#elif defined(CONFIG_ARM64)
        regs->regs[19] = SPARK_START_MAGIC;
        regs->regs[20] = spark_cpu_features;
        regs->regs[21] = task_pid_nr(current);
        regs->pc = header->entry;
        regs->sp = stack_addr;
        regs->pstate = PSR_MODE_EL0t;
#elif defined(CONFIG_RISCV)
        regs->s2 = SPARK_START_MAGIC;
        regs->s3 = spark_cpu_features_now();
        regs->s4 = task_pid_nr(current);
        regs->epc = header->entry;
        regs->sp = stack_addr;
        regs->status = SR_SPIE;
#endif

        finalize_exec(bprm);

        // Everything before this in kernel_execve is the generic prologue:
        // allocating a bprm, opening the file, building a throwaway mm to hold
        // argv and then transplanting its stack. This counter is only our part.
        atomic_long_add(ktime_get_ns() - loader_started, &stat_loader_ns);
        atomic_long_inc(&stat_loads);

        return 0;
fatal:
        force_fatal_sig(SIGKILL);
        return ret;
}

/*
        Spawning without the fork

        The usual path forks -- duplicating the caller's address space, page
        tables and file table -- and then execs, which immediately tears the
        address space back down. Nothing ever reads the copy.

        user_mode_thread creates a task with no address space to copy, and
        kernel_execve then builds the new one directly. It is the same pair the
        kernel uses to start /init. The result is a normal child of the caller:
        it reports through SIGCHLD and is reaped with wait4 like any other.
*/

struct spawn_work
{
        char *path;
        struct spawn_strings *arguments;
        struct spawn_strings *environment;
        unsigned int argc;
        bool shell_fallback;
        bool path_owned;
        // spawn_terminal's, whose first window takes the keyboard.
        bool terminal;
        struct file *stdio[3];
};

static void spawn_strings_put(struct spawn_strings *strings)
{
        if (strings && refcount_dec_and_test(&strings->references))
                kvfree(strings);
}

static void spawn_free(struct spawn_work *work)
{
        for (unsigned int i = 0; i < array_count(work->stdio); i++)
                if (work->stdio[i])
                        fput(work->stdio[i]);
        spawn_strings_put(work->environment);
        spawn_strings_put(work->arguments);
        if (work->path_owned)
                kfree(work->path);
        kfree(work);
}

/*
        Starts one program with no arguments and no environment.

        The ioctl path exists for a program that wants to start another; this
        is for the kernel starting the first one, which is a much smaller
        request and needs none of the copying from userspace.
*/
static int spawn_enter(void *data);

#ifdef CONFIG_MOONWATER_CANVAS
static int spawn_terminal(void)
{
        struct spawn_work *work = kzalloc(sizeof(*work), GFP_KERNEL);

        if (!work)
                return -ENOMEM;

        /* Borrow the literal like do_spawn borrows its fixed /shell path;
           copying it for a worker whose next action is execve adds a slab
           round trip and no lifetime.

           /term is a link to the shell that the image makes for every applet
           in the SYSTEM category, so this is the one shell image reached
           under the name of the applet wanted -- the same multicall
           convention as every other name at the root, and it stops working
           the moment term stops being a SYSTEM applet. Nothing said the two
           had to agree until the image_nodes harness did. */
        work->path = SPARK_TERMINAL_PROGRAM;
        work->arguments = kvmalloc(sizeof(*work->arguments) +
                                   2 * sizeof(char *), GFP_KERNEL);

        /* spawn_free handles either allocation failing. */
        if (work->arguments)
                refcount_set(&work->arguments->references, 1);

        if (!work->arguments)
        {
                spawn_free(work);
                return -ENOMEM;
        }

        work->arguments->vector = (char **)(work->arguments + 1);
        work->arguments->vector[0] = work->path;
        work->arguments->vector[1] = NULL;
        work->argc = 1;
        work->terminal = true;

        if (user_mode_thread(spawn_enter, work, SIGCHLD) <= 0)
        {
                spawn_free(work);
                return -EAGAIN;
        }

        return 0;
}
#endif

/*
        A program starts able to be interrupted.

        execve resets handled signals to default but carries ignored ones
        across, so a shell that ignores SIGINT so it survives control-C would
        hand that same deafness to everything it runs, and nothing could ever
        be cancelled.
*/
static void spawn_default_signals(void)
{
        struct k_sigaction *action = current->sighand->action;
        int signal;

        spin_lock_irq(&current->sighand->siglock);

        for (signal = 0; signal < _NSIG; signal++)
                if (action[signal].sa.sa_handler == SIG_IGN)
                        action[signal].sa.sa_handler = SIG_DFL;

        spin_unlock_irq(&current->sighand->siglock);
}

static int spawn_enter(void *data)
{
        u64 started = ktime_get_ns();

        struct spawn_work *work = data;
        static const char *const empty_envp[] = {NULL};
        const char *const *environment = work->environment
                ? (const char *const *)work->environment->vector : empty_envp;
        int ret;

        spawn_default_signals();

#ifdef CONFIG_MOONWATER_CANVAS
        /*
                The compositor's terminal says who it is before it becomes
                /term, so the window it opens can be told from any other
                program's. Recorded by the task itself, which is what puts it
                ahead of that window; a newer terminal replaces one that never
                opened a window at all.
        */
        if (work->terminal)
                put_pid(xchg(&canvas_spawned, get_pid(task_tgid(current))));
#endif

        /* Without the close-on-exec flag, so these three outlive the load
           while every other descriptor the caller happened to hold does
           not. A pipeline's other ends are among those. */
        for (unsigned int i = 0; i < array_count(work->stdio); i++)
                if (work->stdio[i] && (ret = replace_fd(i, work->stdio[i], 0)))
                        goto finished;

        ret = kernel_execve(work->path,
                            (const char *const *)work->arguments->vector,
                            environment);

        /*
         * The shell promises more than execve: ENOEXEC for an executable text
         * file means interpret it, not reject it. The ioctl normally cannot
         * return that error because this worker already exists by the time
         * kernel_execve sees the file, so the retry has to happen here.
         *
         * argv becomes { /bin/sh, script, original arguments after argv[0] }.
         * The raw spawn opcode never takes this branch.
         */
        if (ret == -ENOEXEC && work->shell_fallback)
        {
                const char **script_argv;

                script_argv = kcalloc((size_t)work->argc + 2,
                                      sizeof(*script_argv), GFP_KERNEL);

                if (!script_argv)
                        ret = -ENOMEM;
                else
                {
                        script_argv[0] = "/bin/sh";
                        script_argv[1] = work->path;

                        memory_copy_apart(script_argv + 2, work->arguments->vector + 1,
                                          (work->argc - 1) * sizeof(*script_argv));

                        ret = kernel_execve(script_argv[0], script_argv,
                                            environment);
                        kfree(script_argv);
                }
        }

finished:
        atomic_long_add(ktime_get_ns() - started, &stat_exec_ns);

        // kernel_execve has copied everything it needs by now, so the request
        // can go before anything else touches it.
        spawn_free(work);

        if (ret)
        {
                // The task exists by the time exec is attempted, so a bad path
                // cannot come back as an ioctl error. Exiting 127 is what a
                // shell reports for "could not run it", and it keeps the
                // caller from mistaking the failure for a clean exit.
                pr_alert_ratelimited("[moonwater] " "spawn: exec failed: %d\n", ret);
                do_exit(127 << 8);
        }

        return 0;
}

// argv and envp arrive the same way: one flat block of NUL terminated strings
// plus a count, so a single copy_from_user brings each across and the pointer
// array is built by walking it.
static int copy_strings(unsigned long user_block, unsigned int bytes,
                        unsigned int count, struct spawn_strings **out)
{
        struct spawn_strings *strings;
        char *block;
        char **vector;
        char *walk;
        size_t pointer_bytes;
        unsigned int i;

        if (count == 0 || count > SPARK_SPAWN_MAX_STRINGS || bytes == 0 ||
            bytes > SPARK_SPAWN_MAX_BYTES)
                return -EINVAL;

        /* The limits above put this below 3 MiB on every supported 64-bit
           architecture, so none of the size arithmetic can overflow. */
        pointer_bytes = ((size_t)count + 1) * sizeof(char *);

        /* The immutable bytes and their pointers have exactly the same
           lifetime. One allocation removes a slab round trip from each of
           argv and envp; kvmalloc keeps generated long commands on the fast
           path without demanding physically contiguous megabytes. */
        /* A generated environment can remain pinned to an open descriptor. */
        strings = kvmalloc(sizeof(*strings) + pointer_bytes + bytes,
                           GFP_KERNEL_ACCOUNT);
        if (!strings)
                return -ENOMEM;

        refcount_set(&strings->references, 1);
        vector = (char **)(strings + 1);
        strings->vector = vector;
        block = (char *)vector + pointer_bytes;

        if (copy_from_user(block, (const void __user *)user_block, bytes))
        {
                kvfree(strings);
                return -EFAULT;
        }

        walk = block;
        for (i = 0; i < count; i++)
        {
                size_t remaining = (size_t)(block + bytes - walk);
                size_t length = string_length_max(walk, remaining);

                if (length == remaining)
                        goto malformed;

                vector[i] = walk;
                walk += length + 1;
        }
        vector[count] = NULL;

        *out = strings;
        return 0;

malformed:
        kvfree(strings);
        return -EINVAL;
}

static long do_spawn(struct file *file, struct spawn __user *request)
{
        struct device_context *context = file->private_data;
        struct spawn args;
        struct spawn_work *work;
        struct spawn_strings *old_environment = NULL;
        struct pid *old_owner = NULL;
        const struct cred *old_cred = NULL;
        bool shell_fallback;
        const char *fixed_path;
        long ret;
        pid_t pid;

        if (copy_from_user(&args, request, sizeof(args)))
                return -EFAULT;

        /* Refusing what this kernel does not define keeps a flag added later
           from meaning "ignored" on an older loader. */
        if (args.flags & ~SPARK_SPAWN_FLAGS)
                return -EINVAL;

        shell_fallback = args.flags & SPARK_SPAWN_SHELL;
        fixed_path = args.flags & SPARK_SPAWN_TOOL ? SPARK_TOOL_PROGRAM : NULL;

        work = kzalloc(sizeof(*work), GFP_KERNEL);
        if (!work)
                return -ENOMEM;

        for (unsigned int i = 0; i < array_count(work->stdio); i++)
        {
                if (args.stdio[i] < 0)
                        continue;

                if (!(work->stdio[i] = fget(args.stdio[i])))
                {
                        ret = -EBADF;
                        goto fail;
                }
        }

        if (fixed_path)
                work->path = (char *)fixed_path;
        else
        {
                work->path = strndup_user((const char __user *)args.path,
                                          PATH_MAX);
                if (IS_ERR(work->path))
                {
                        ret = PTR_ERR(work->path);
                        work->path = NULL;
                        goto fail;
                }

                work->path_owned = true;
        }

        ret = copy_strings(args.argv, args.argv_bytes, args.argv_count,
                           &work->arguments);
        if (ret)
                goto fail;

        if (args.envp && args.envp_count)
        {
                mutex_lock(&context->spawn_lock);
                /* clone inherits the open file description and the shell's
                   generation counter.  The same generation in two process
                   branches is not the same environment, so identity is part
                   of the key.  Holding struct pid prevents numeric PID reuse
                   from making stale bytes look current later. */
                if (args.envp_generation && context->environment &&
                    context->environment_owner == task_tgid(current) &&
                    context->environment_cred == current_cred() &&
                    context->environment_generation == args.envp_generation)
                {
                        refcount_inc(&context->environment->references);
                        work->environment = context->environment;
                }
                mutex_unlock(&context->spawn_lock);

                if (!work->environment)
                {
                        ret = copy_strings(args.envp, args.envp_bytes,
                                           args.envp_count,
                                           &work->environment);
                        if (ret)
                                goto fail;

                        if (args.envp_generation)
                        {
                                mutex_lock(&context->spawn_lock);
                                old_environment = context->environment;
                                old_owner = context->environment_owner;
                                old_cred = context->environment_cred;
                                refcount_inc(&work->environment->references);
                                context->environment = work->environment;
                                context->environment_generation =
                                        args.envp_generation;
                                context->environment_owner =
                                        get_pid(task_tgid(current));
                                context->environment_cred =
                                        get_cred(current_cred());
                                mutex_unlock(&context->spawn_lock);
                                spawn_strings_put(old_environment);
                                put_pid(old_owner);
                                put_cred(old_cred);
                        }
                }
        }

        work->argc = args.argv_count;
        work->shell_fallback = shell_fallback;

        // SIGCHLD alone, so the new task is an ordinary child of the caller
        // and wait4 works on it the same way it does for a fork.
        {
                u64 started = ktime_get_ns();
                pid = user_mode_thread(spawn_enter, work, SIGCHLD);
                atomic_long_add(ktime_get_ns() - started, &stat_task_ns);
                atomic_long_inc(&stat_spawns);
        }

        if (pid < 0)
        {
                ret = pid;
                goto fail;
        }

        // work is owned by the new task from here.
        return pid;

fail:
        spawn_free(work);
        return ret;
}

static long report_stats(struct stats __user *out)
{
        struct stats stats = {
            .spawns = atomic_long_read(&stat_spawns),
            .task_ns = atomic_long_read(&stat_task_ns),
            .exec_ns = atomic_long_read(&stat_exec_ns),
            .loader_ns = atomic_long_read(&stat_loader_ns),
            .loads = atomic_long_read(&stat_loads),
            .map_ns = atomic_long_read(&stat_map_ns),
        };

        return copy_to_user(out, &stats, sizeof(stats)) ? -EFAULT : 0;
}

/*
        Bindings: what the machine's own events run.

        One input handler, not grabbing, so a key Canvas does not swallow still
        types. The callback is interrupt context and only debounces and queues;
        the line runs from system_dfl_long_wq as `/shell -c`, through
        user_mode_thread and kernel_wait, never call_usermodehelper: waiting
        there holds helper_lock, and a command that then stops the machine
        stalls five seconds in usermodehelper_disable.

        poweroff and reboot must not fail quietly: a line that could not start,
        or returned without stopping the machine, falls back to orderly_poweroff
        or orderly_reboot. The shell's poweroff is tried first because it
        remounts the disks read-only, and /sbin/poweroff is not in this image.
*/
#define BIND_MOD_CTRL 1u
#define BIND_MOD_ALT 2u

struct bind_row {
        const char *name;
        const char *def;
        unsigned int event;
        unsigned int debounce;
        unsigned int boot;
        unsigned int drop_busy;
        char command[SPARK_BIND_COMMAND_MAX];
        atomic_t bound;
        atomic_t runs;
        atomic_t busy;
        unsigned long last;
        struct work_struct work;
};

struct bind_spawn {
        char command[SPARK_BIND_COMMAND_MAX];
        char event[SPARK_BIND_NAME_MAX];
};

struct bind_handle {
        struct input_handle handle;
        unsigned int mods;
};

static struct bind_row bind_table[SPARK_BIND_EVENTS];
static DEFINE_SPINLOCK(bind_lock);
static atomic_t bind_ctrl;
static atomic_t bind_alt;
static unsigned int bind_held[8];
static unsigned bind_held_n;
static _Bool bind_handler_registered;
static struct work_struct bind_canvas_work;

/*
        Codes whose press or release might be a bound key. Typing is the
        common path; a bit test here is what keeps spin_lock_irqsave off it.
        KEY_RESTART is 408, so the map covers the kernel's KEY_MAX floor.
*/
#define BIND_CODES 768u
static unsigned long bind_watch[BIND_CODES / 64];

static const struct {
        const char *def;
        unsigned int debounce_ms;
        unsigned int boot;
        unsigned int drop_busy;
        unsigned int code;
} bind_spec[SPARK_BIND_EVENTS] = {
        {"poweroff", 1000, 1, 1, KEY_POWER},
        {"", 1000, 1, 1, KEY_SLEEP},
        {"reboot", 1000, 1, 1, KEY_RESTART},
        {"reboot", 1000, 1, 1, 0},
        {"", 500, 0, 1, 0},
        {"", 500, 0, 1, 0},
        {"", 0, 0, 0, KEY_VOLUMEUP},
        {"", 0, 0, 0, KEY_VOLUMEDOWN},
        {"", 0, 0, 0, KEY_MUTE},
        {"", 0, 0, 0, KEY_BRIGHTNESSUP},
        {"", 0, 0, 0, KEY_BRIGHTNESSDOWN},
        {"", 0, 0, 1, 0},
        {"", 0, 0, 1, 0},
};

static struct bind_row *bind_row(unsigned int event)
{
        if (!event || event > SPARK_BIND_EVENTS)
                return NULL;
        return bind_table + event - 1;
}

static void bind_watch_code(unsigned int code, _Bool on)
{
        unsigned int word;
        unsigned long bit, now;

        if (code >= BIND_CODES)
                return;
        word = code / 64;
        bit = 1UL << (code % 64);
        now = READ_ONCE(bind_watch[word]);
        WRITE_ONCE(bind_watch[word], on ? now | bit : now & ~bit);
}

static void bind_watch_row(struct bind_row *row, _Bool on)
{
        switch (row->event)
        {
        case SPARK_BIND_CTRL_ALT_DELETE:
                bind_watch_code(KEY_DELETE, on);
                bind_watch_code(KEY_KPDOT, on);
                return;
        case SPARK_BIND_SLEEP:
                bind_watch_code(KEY_SLEEP, on);
                bind_watch_code(KEY_SUSPEND, on);
                return;
        case SPARK_BIND_LID_CLOSE:
        case SPARK_BIND_LID_OPEN:
        case SPARK_BIND_CANVAS_ON:
        case SPARK_BIND_CANVAS_OFF:
                return;
        default:
                bind_watch_code(bind_spec[row->event - 1].code, on);
        }
}

static _Bool bind_watched(unsigned int code)
{
        return code < BIND_CODES &&
               (READ_ONCE(bind_watch[code / 64]) & (1UL << (code % 64))) != 0;
}

static _Bool bind_command_is_default(struct bind_row *row, const char *command)
{
        return !strcmp(command, row->def);
}

static int bind_spawn_enter(void *data)
{
        struct bind_spawn *spawn = data;
        char event_env[sizeof("MOONWATER_EVENT=") + SPARK_BIND_NAME_MAX];
        char *argv[] = {SPARK_TOOL_PROGRAM, "-c", spawn->command, NULL};
        char *envp[] = {"HOME=/root", "PATH=/bin:/sbin:/usr/bin:/usr/sbin",
                        "TERM=linux", event_env, NULL};
        int ret;

        snprintf(event_env, sizeof(event_env), "MOONWATER_EVENT=%s", spawn->event);
        ret = kernel_execve(argv[0], (const char *const *)argv,
                            (const char *const *)envp);
        kfree(spawn);
        do_exit(ret);
}

static void bind_run(struct bind_row *row)
{
        struct bind_spawn *spawn;
        char command[SPARK_BIND_COMMAND_MAX];
        _Bool poweroff, reboot;
        pid_t pid;
        int ret = 0, stat = 0;
        unsigned long flags;

        spin_lock_irqsave(&bind_lock, flags);
        strscpy(command, row->command, sizeof(command));
        spin_unlock_irqrestore(&bind_lock, flags);

        atomic_set(&row->busy, 1);

        if (!command[0])
        {
                pr_info("[moonwater] " "%s: ignored\n", row->name);
                goto done;
        }

        poweroff = !strcmp(command, "poweroff");
        reboot = !strcmp(command, "reboot");
        pr_info("[moonwater] " "%s: %s\n", row->name, command);

        spawn = kzalloc(sizeof(*spawn), GFP_KERNEL);
        if (!spawn)
        {
                ret = -ENOMEM;
                goto fallback;
        }

        strscpy(spawn->command, command, sizeof(spawn->command));
        strscpy(spawn->event, row->name, sizeof(spawn->event));

        kernel_sigaction(SIGCHLD, SIG_DFL);
        pid = user_mode_thread(bind_spawn_enter, spawn, SIGCHLD);
        if (pid > 0)
                ret = kernel_wait(pid, &stat);
        else
        {
                kfree(spawn);
                ret = pid ? pid : -EAGAIN;
        }
        kernel_sigaction(SIGCHLD, SIG_IGN);

        if (!ret)
                goto done;

fallback:
        if (!poweroff && !reboot)
        {
                pr_warn("[moonwater] " "%s: %s did not start (%d)\n",
                        row->name, command, ret);
                goto done;
        }

        pr_warn("[moonwater] " "%s: %s answered %d, stopping the machine anyway\n",
                row->name, command, ret);

        if (reboot)
                orderly_reboot();
        else
                orderly_poweroff(true);
done:
        atomic_set(&row->busy, 0);
}

static void bind_work(struct work_struct *work)
{
        bind_run(container_of(work, struct bind_row, work));
}

static void bind_canvas_run(struct work_struct *work)
{
        struct bind_row *on = bind_row(SPARK_BIND_CANVAS_ON);
        struct bind_row *off = bind_row(SPARK_BIND_CANVAS_OFF);
        _Bool running = false;

        (void)work;
#ifdef CONFIG_MOONWATER_CANVAS
        running = canvas_is_on();
#endif
        bind_run(running ? on : off);
        atomic_set(&on->busy, 0);
        atomic_set(&off->busy, 0);
}

static void bind_queue(struct bind_row *row)
{
        struct work_struct *work;

        if (system_state != SYSTEM_RUNNING)
                return;

        if (row->event == SPARK_BIND_CANVAS_ON ||
            row->event == SPARK_BIND_CANVAS_OFF)
        {
                struct bind_row *on = bind_row(SPARK_BIND_CANVAS_ON);
                struct bind_row *off = bind_row(SPARK_BIND_CANVAS_OFF);

                if (atomic_read(&on->busy) || atomic_read(&off->busy) ||
                    work_pending(&bind_canvas_work))
                        return;
                atomic_set(&on->busy, 1);
                atomic_set(&off->busy, 1);
                work = &bind_canvas_work;
        }
        else
        {
                if (row->drop_busy &&
                    (atomic_read(&row->busy) || work_pending(&row->work)))
                        return;
                work = &row->work;
        }

        atomic_fetch_add(1, &row->runs);
        queue_work(system_dfl_long_wq, work);
}

static void bind_fire(unsigned int event)
{
        struct bind_row *row = bind_row(event);

        if (row && atomic_read(&row->bound))
                bind_queue(row);
}

static _Bool bind_debounce(struct bind_row *row)
{
        unsigned long now = jiffies | 1;
        unsigned long last = READ_ONCE(row->last);

        if (!row->debounce)
                return true;

        if (last && time_before(now, last + row->debounce))
                return false;

        return cmpxchg(&row->last, last, now) == last;
}

/*
        Direct by code, not a walk of bind_row. Each row carries a 256-byte
        command; thirteen of those is a cache line per comparison. The
        codes here are immediates, so the compiler's compare chain is the
        floor.
*/
static struct bind_row *bind_match(unsigned int type, unsigned int code, int value)
{
        unsigned int event;

        if (type == EV_SW)
                return code == SW_LID ? bind_row(value ? SPARK_BIND_LID_CLOSE
                                                       : SPARK_BIND_LID_OPEN)
                                      : NULL;

        if (type != EV_KEY || value != 1)
                return NULL;

        if (code == KEY_DELETE || code == KEY_KPDOT)
                return (atomic_read(&bind_ctrl) > 0 &&
                        atomic_read(&bind_alt) > 0)
                               ? bind_row(SPARK_BIND_CTRL_ALT_DELETE)
                               : NULL;

        switch (code)
        {
        case KEY_POWER:
                event = SPARK_BIND_POWEROFF;
                break;
        case KEY_SLEEP:
        case KEY_SUSPEND:
                event = SPARK_BIND_SLEEP;
                break;
        case KEY_RESTART:
                event = SPARK_BIND_RESET;
                break;
        case KEY_VOLUMEUP:
                event = SPARK_BIND_VOLUME_UP;
                break;
        case KEY_VOLUMEDOWN:
                event = SPARK_BIND_VOLUME_DOWN;
                break;
        case KEY_MUTE:
                event = SPARK_BIND_MUTE;
                break;
        case KEY_BRIGHTNESSUP:
                event = SPARK_BIND_BRIGHTNESS_UP;
                break;
        case KEY_BRIGHTNESSDOWN:
                event = SPARK_BIND_BRIGHTNESS_DOWN;
                break;
        default:
                return NULL;
        }

        return bind_row(event);
}

static _Bool bind_row_bound(struct bind_row *row)
{
        return row && atomic_read(&row->bound);
}

static _Bool bind_code_held(unsigned int code)
{
        unsigned at;

        for (at = 0; at < bind_held_n; at++)
                if (bind_held[at] == code)
                        return true;
        return false;
}

static void bind_hold(unsigned int code, int value)
{
        unsigned at;

        if (value)
        {
                if (bind_code_held(code) || bind_held_n >= ARRAY_SIZE(bind_held))
                        return;
                bind_held[bind_held_n++] = code;
                return;
        }

        for (at = 0; at < bind_held_n; at++)
                if (bind_held[at] == code)
                {
                        bind_held[at] = bind_held[--bind_held_n];
                        return;
                }
}

static _Bool bind_key_swallowed(unsigned int code, int value)
{
        struct bind_row *row;
        unsigned long flags;
        _Bool swallow = false;

        if (!bind_watched(code) && (value == 1 || !READ_ONCE(bind_held_n)))
                return false;

        spin_lock_irqsave(&bind_lock, flags);
        if (value == 1)
        {
                row = bind_match(EV_KEY, code, 1);
                if (bind_row_bound(row))
                {
                        bind_hold(code, 1);
                        swallow = true;
                }
        }
        else if (bind_code_held(code))
        {
                if (!value)
                        bind_hold(code, 0);
                swallow = true;
        }
        spin_unlock_irqrestore(&bind_lock, flags);
        return swallow;
}

static void bind_mods(struct bind_handle *bind, unsigned int code, int value)
{
        unsigned int bit = 0;

        if (code == KEY_LEFTCTRL || code == KEY_RIGHTCTRL)
                bit = BIND_MOD_CTRL;
        else if (code == KEY_LEFTALT || code == KEY_RIGHTALT)
                bit = BIND_MOD_ALT;
        else
                return;

        if (value)
        {
                if (!(bind->mods & bit))
                {
                        bind->mods |= bit;
                        if (bit == BIND_MOD_CTRL)
                                atomic_fetch_add(1, &bind_ctrl);
                        else
                                atomic_fetch_add(1, &bind_alt);
                }
        }
        else if (bind->mods & bit)
        {
                bind->mods &= ~bit;
                if (bit == BIND_MOD_CTRL)
                        atomic_fetch_sub(1, &bind_ctrl);
                else
                        atomic_fetch_sub(1, &bind_alt);
        }
}

static void bind_event(struct input_handle *handle, unsigned int type,
                       unsigned int code, int value)
{
        struct bind_handle *bind = container_of(handle, struct bind_handle, handle);
        struct bind_row *row;

        /*
                Mice and tablets match EV_KEY for their buttons, so this
                handler sees every motion report too. Those are not events
                we bind. A letter key is not in the watch map, so it never
                reaches bind_match's switch.
        */
        if (type == EV_KEY)
        {
                if (code <= KEY_RIGHTALT)
                        bind_mods(bind, code, value);
                if (value != 1)
                        return;
                if (!bind_watched(code))
                        return;
        }
        else if (type != EV_SW)
                return;

        row = bind_match(type, code, value);
        if (!row || !bind_row_bound(row) || !bind_debounce(row))
                return;

        bind_queue(row);
}

static int bind_connect(struct input_handler *handler, struct input_dev *dev,
                        const struct input_device_id *id)
{
        struct bind_handle *bind = kzalloc(sizeof(*bind), GFP_KERNEL);
        int ret;

        (void)id;

        if (!bind)
                return -ENOMEM;

        bind->handle.dev = dev;
        bind->handle.handler = handler;
        bind->handle.name = "moonwater-bind";

        ret = input_register_handle(&bind->handle);
        if (ret)
                goto free;

        ret = input_open_device(&bind->handle);
        if (ret)
                goto unregister;

        return 0;

unregister:
        input_unregister_handle(&bind->handle);
free:
        kfree(bind);
        return ret;
}

static void bind_disconnect(struct input_handle *handle)
{
        struct bind_handle *bind = container_of(handle, struct bind_handle, handle);

        bind_mods(bind, KEY_LEFTCTRL, 0);
        bind_mods(bind, KEY_LEFTALT, 0);
        input_close_device(handle);
        input_unregister_handle(handle);
        kfree(bind);
}

static const struct input_device_id bind_ids[] = {
        {
                .flags = INPUT_DEVICE_ID_MATCH_EVBIT,
                .evbit = {BIT_MASK(EV_KEY)},
        },
        {
                .flags = INPUT_DEVICE_ID_MATCH_EVBIT,
                .evbit = {BIT_MASK(EV_SW)},
        },
        {},
};

static struct input_handler bind_handler = {
        .event = bind_event,
        .connect = bind_connect,
        .disconnect = bind_disconnect,
        .name = "moonwater-bind",
        .id_table = bind_ids,
};

#ifdef CONFIG_VT
static int bind_keyboard_notify(struct notifier_block *nb, unsigned long code,
                                void *p)
{
        struct keyboard_notifier_param *param = p;

        (void)nb;
        if (code != KBD_KEYSYM || !param->down)
                return NOTIFY_DONE;

        // The raw keymap stores Boot as 0xf20c; K_BOOT is 0x020c.
        if ((KTYP(param->value) & 0x0f) == KT_SPEC &&
            KVAL(param->value) == KVAL(K_BOOT))
        {
                struct bind_row *row = bind_row(SPARK_BIND_CTRL_ALT_DELETE);

                if (bind_row_bound(row) && bind_debounce(row))
                        bind_queue(row);
                return NOTIFY_STOP;
        }

        return NOTIFY_DONE;
}

static struct notifier_block bind_kbd_nb = {
        .notifier_call = bind_keyboard_notify,
};
#endif

#ifdef CONFIG_PM
static int bind_pm_notify(struct notifier_block *nb, unsigned long event, void *p)
{
        unsigned at;

        (void)nb;
        (void)p;
        if (event != PM_POST_SUSPEND)
                return NOTIFY_DONE;

        for (at = 0; at < SPARK_BIND_EVENTS; at++)
                if (bind_table[at].drop_busy)
                        WRITE_ONCE(bind_table[at].last, jiffies | 1);

        return NOTIFY_OK;
}

static struct notifier_block bind_pm_nb = {.notifier_call = bind_pm_notify};
#endif

static void bind_start(void)
{
        unsigned at;

        INIT_WORK(&bind_canvas_work, bind_canvas_run);
        atomic_set(&bind_ctrl, 0);
        atomic_set(&bind_alt, 0);
        bind_held_n = 0;
        for (at = 0; at < ARRAY_SIZE(bind_watch); at++)
                bind_watch[at] = 0;

        for (at = 0; at < SPARK_BIND_EVENTS; at++)
        {
                struct bind_row *row = bind_table + at;

                row->name = spark_bind_event_name[at];
                row->def = bind_spec[at].def;
                row->event = at + 1;
                row->debounce = bind_spec[at].debounce_ms
                                        ? (unsigned int)msecs_to_jiffies(
                                                  bind_spec[at].debounce_ms)
                                        : 0;
                row->boot = bind_spec[at].boot;
                row->drop_busy = bind_spec[at].drop_busy;
                strscpy(row->command, row->def, sizeof(row->command));
                atomic_set(&row->bound, row->def[0] != 0);
                atomic_set(&row->runs, 0);
                atomic_set(&row->busy, 0);
                row->last = 0;
                INIT_WORK(&row->work, bind_work);
                bind_watch_row(row, row->def[0] != 0);
        }
}

static void bind_stop(void)
{
        unsigned at;

#ifdef CONFIG_VT
        unregister_keyboard_notifier(&bind_kbd_nb);
#endif
#ifdef CONFIG_PM
        unregister_pm_notifier(&bind_pm_nb);
#endif
        if (bind_handler_registered)
                input_unregister_handler(&bind_handler);

        cancel_work_sync(&bind_canvas_work);
        for (at = 0; at < SPARK_BIND_EVENTS; at++)
                cancel_work_sync(&bind_table[at].work);
}

static void bind_answer(struct bind_control *request, struct bind_row *row)
{
        unsigned long flags;

        spin_lock_irqsave(&bind_lock, flags);
        strscpy(request->name, row->name, sizeof(request->name));
        strscpy(request->command, row->command, sizeof(request->command));
        spin_unlock_irqrestore(&bind_lock, flags);

        request->runs = (unsigned int)atomic_read(&row->runs);
        request->count = SPARK_BIND_EVENTS;
        request->flags = 0;
        if (bind_command_is_default(row, request->command))
                request->flags |= SPARK_BIND_DEFAULT;
        if (atomic_read(&row->busy))
                request->flags |= SPARK_BIND_RUNNING;
        if (work_pending(&row->work) ||
            ((row->event == SPARK_BIND_CANVAS_ON ||
              row->event == SPARK_BIND_CANVAS_OFF) &&
             work_pending(&bind_canvas_work)))
                request->flags |= SPARK_BIND_PENDING;
        if (row->boot)
                request->flags |= SPARK_BIND_BOOT;
}

static long report_bind(struct bind_control __user *out)
{
        struct bind_control request;
        struct bind_row *row;
        unsigned long flags;

        if (copy_from_user(&request, out, sizeof(request)))
                return -EFAULT;
        if (request.op > SPARK_BIND_SET || request.reserved[0] ||
            request.reserved[1] || request.reserved[2])
                return -EINVAL;

        row = bind_row(request.event);
        if (!row)
                return -EINVAL;

        if (request.op == SPARK_BIND_SET)
        {
                if (!capable(CAP_SYS_ADMIN) ||
                    (row->boot && !capable(CAP_SYS_BOOT)))
                {
                        bind_answer(&request, row);
                        return copy_to_user(out, &request, sizeof(request))
                                       ? -EFAULT
                                       : -EPERM;
                }
                if (!memchr(request.command, 0, sizeof(request.command)))
                        return -ENAMETOOLONG;

                spin_lock_irqsave(&bind_lock, flags);
                if (!request.command[0])
                        strscpy(row->command, row->def, sizeof(row->command));
                else
                        strscpy(row->command, request.command, sizeof(row->command));
                atomic_set(&row->bound, row->command[0] != 0);
                bind_watch_row(row, row->command[0] != 0);
                spin_unlock_irqrestore(&bind_lock, flags);
        }

        bind_answer(&request, row);
        return copy_to_user(out, &request, sizeof(request)) ? -EFAULT : 0;
}

#ifdef CONFIG_MOONWATER_CANVAS
#define REPORT_CANVAS(name, type, collect)                                   \
        static long name(struct type __user *out)                            \
        {                                                                    \
                struct type stats;                                           \
                collect(&stats);                                             \
                return copy_to_user(out, &stats, sizeof(stats)) ? -EFAULT : 0; \
        }

REPORT_CANVAS(report_input, input_stats, canvas_input_stats)
REPORT_CANVAS(report_cursor, cursor_stats, canvas_cursor_stats)
REPORT_CANVAS(report_devices, input_devices, canvas_input_devices)
#undef REPORT_CANVAS

/*
        Canvas off and on, and what it holds.

        The state is copied back whatever the request answers, so a refused on
        can name the program that holds the display. Reading needs nothing;
        on and off stop and start the desktop everyone at the machine is
        using, so they need CAP_SYS_ADMIN.
*/
static long report_canvas(struct canvas_control __user *out)
{
        struct canvas_control control;
        unsigned int request;
        long answer = 0;

        if (copy_from_user(&control, out, sizeof(control)))
                return -EFAULT;

        request = control.request;
        if (request > SPARK_CANVAS_OFF)
                return -EINVAL;
        if (request != SPARK_CANVAS_STATUS && !capable(CAP_SYS_ADMIN))
                return -EPERM;

        memset(&control, 0, sizeof(control));
        control.request = request;

        if (request == SPARK_CANVAS_ON)
                answer = canvas_turn_on(&control);
        else if (request == SPARK_CANVAS_OFF)
                answer = canvas_turn_off();

        canvas_state(&control);

        if (copy_to_user(out, &control, sizeof(control)))
                return -EFAULT;

        return answer;
}
#endif

/*
        Typed system state in one crossing.

        These sections are already world-readable through procfs.  The ioctl
        preserves the caller's network namespace and reports no task records;
        process visibility is governed by procfs mount and ptrace policy, so
        userspace deliberately keeps that section on the procfs fallback.
*/
struct snapshot_builder
{
        u8 *data;
        u32 capacity;
        u32 used;
        u32 required;
};

static DEFINE_MUTEX(snapshot_lock);
static u8 *snapshot;
static u32 snapshot_room;

static void *snapshot_append(struct snapshot_builder *build, u32 bytes)
{
        void *record = NULL;

        if (unlikely(bytes > U32_MAX - build->required))
        {
                build->required = U32_MAX;
                return NULL;
        }

        u32 stop = build->required + bytes;

        if (likely(stop <= build->capacity))
        {
                record = build->data + build->required;
                build->used = stop;
        }

        build->required = stop;
        return record;
}

static HOT void snapshot_system(struct snapshot_header *header)
{
        struct sysinfo info;
        unsigned long loads[3];

        si_meminfo(&info);
        header->memory_total = info.totalram * info.mem_unit;
        header->memory_available = (unsigned long)si_mem_available() * PAGE_SIZE;
        si_swapinfo(&info);
        header->swap_total = info.totalswap * info.mem_unit;
        header->swap_free = info.freeswap * info.mem_unit;

        get_avenrun(loads, FIXED_1 / 200, 0);
        for (unsigned int i = 0; i < 3; i++)
                header->load[i] = LOAD_INT(loads[i]) * 100 +
                                  LOAD_FRAC(loads[i]);
}

static HOT void snapshot_cpus(struct snapshot_builder *build,
                              struct snapshot_header *header)
{
        struct snapshot_cpu aggregate = {.id = ~0u};
        struct snapshot_cpu *aggregate_out;
        int cpu;

        header->cpu_offset = build->required;
        aggregate_out = snapshot_append(build, sizeof(*aggregate_out));
        header->cpu_count++;

        for_each_online_cpu(cpu)
        {
                struct kernel_cpustat stat;
                struct snapshot_cpu record = {.id = cpu};

                kcpustat_cpu_fetch(&stat, cpu);

                /* Guest time is already included in user/nice. */
                for (unsigned int field = 0; field <= CPUTIME_STEAL; field++)
                        record.total_ns += stat.cpustat[field];

                record.idle_ns = stat.cpustat[CPUTIME_IDLE] +
                                 stat.cpustat[CPUTIME_IOWAIT];
                aggregate.total_ns += record.total_ns;
                aggregate.idle_ns += record.idle_ns;

                struct snapshot_cpu *out =
                    snapshot_append(build, sizeof(record));

                if (likely(out))
                        *out = record;
                header->cpu_count++;
        }

        if (aggregate_out)
                *aggregate_out = aggregate;
}

static HOT void snapshot_networks(struct snapshot_builder *build,
                                  struct snapshot_header *header)
{
        struct net_device *device;
        struct net *net = current->nsproxy->net_ns;

        header->network_offset = build->required;

        rcu_read_lock();
        for_each_netdev_rcu(net, device)
        {
                struct rtnl_link_stats64 temporary;
                const struct rtnl_link_stats64 *stats =
                    dev_get_stats(device, &temporary);
                struct snapshot_network record = {0};

                strscpy(record.name, device->name, sizeof(record.name));
                record.received = stats->rx_bytes;
                record.transmitted = stats->tx_bytes;

                struct snapshot_network *out =
                    snapshot_append(build, sizeof(record));

                if (likely(out))
                        *out = record;
                header->network_count++;
        }
        rcu_read_unlock();
}

/*
        The settings this machine booted with.

        The EFI stub hands over the image's .mwset section as a configuration
        table; this keeps the newest of its two slots that checks, and nothing
        when neither does, which is the defaults. A torn write, a made-up
        table or an image with no section all start the machine the same way,
        and only spark_settings_check decides which is which.

        SET replaces the copy for the rest of the session, checked the same
        way; the moonwater command writes the image itself.
*/
static struct spark_settings *settings_current;
static DEFINE_MUTEX(settings_lock);

static COLD void __init settings_start(void)
{
#if defined(CONFIG_EFI) && !defined(MODULE)
        struct linux_efi_moonwater_settings *table;
        struct spark_settings *slots;
        u32 size;
        b32 newest;

        if (efi_moonwater_settings == EFI_INVALID_TABLE_ADDR)
                return;

        table = memremap(efi_moonwater_settings, sizeof(*table), MEMREMAP_WB);
        if (!table)
                return;

        size = table->size;
        memunmap(table);

        if (size < 2 * SPARK_SETTINGS_SLOT || size > 64 * SPARK_SETTINGS_SLOT)
        {
                pr_warn("[moonwater] " "a settings table of %u bytes is not one; booting on the defaults\n", size);
                return;
        }

        table = memremap(efi_moonwater_settings, sizeof(*table) + size, MEMREMAP_WB);
        if (!table)
                return;

        slots = kmalloc(2 * SPARK_SETTINGS_SLOT, GFP_KERNEL);
        if (slots)
                memory_copy_apart(slots, table->bytes, 2 * SPARK_SETTINGS_SLOT);
        memunmap(table);

        if (!slots)
                return;

        newest = spark_settings_newest(slots);
        if (newest < 0)
                pr_warn("[moonwater] " "both settings slots in the image are damaged; booting on the defaults\n");
        else
        {
                settings_current = kmemdup(slots + newest, SPARK_SETTINGS_SLOT, GFP_KERNEL);
                pr_info("[moonwater] " "settings: slot %d, generation %llu\n", newest,
                        (unsigned long long)slots[newest].generation);
        }

        kfree(slots);
#endif
}

static long settings_get(struct spark_settings_request __user *request)
{
        struct spark_settings_request asked;
        long answer = 0;

        if (!capable(CAP_SYS_ADMIN))
                return -EPERM;
        if (copy_from_user(&asked, request, sizeof(asked)))
                return -EFAULT;
        if (asked.flags)
                return -EINVAL;

        mutex_lock(&settings_lock);
        if (!settings_current)
                answer = -ENODATA;
        else if (copy_to_user((void __user *)asked.address, settings_current, SPARK_SETTINGS_SLOT))
                answer = -EFAULT;
        mutex_unlock(&settings_lock);

        return answer;
}

static long settings_set(struct spark_settings_request __user *request)
{
        struct spark_settings_request asked;
        struct spark_settings *settings;
        struct spark_settings *before;

        if (!capable(CAP_SYS_ADMIN))
                return -EPERM;
        if (copy_from_user(&asked, request, sizeof(asked)))
                return -EFAULT;
        if (asked.flags)
                return -EINVAL;

        settings = memdup_user((void __user *)asked.address, SPARK_SETTINGS_SLOT);
        if (IS_ERR(settings))
                return PTR_ERR(settings);

        if (spark_settings_check(settings) < 0)
        {
                kfree(settings);
                return -EINVAL;
        }

        mutex_lock(&settings_lock);
        before = settings_current;
        settings_current = settings;
        mutex_unlock(&settings_lock);

        kfree(before);
        return 0;
}

static HOT long report_snapshot(struct snapshot_request __user *out)
{
        struct snapshot_request request;
        struct snapshot_builder build;
        struct snapshot_header *header;
        long answer = 0;

        if (copy_from_user(&request, out, sizeof(request)))
                return -EFAULT;
        if (request.version != SPARK_SNAPSHOT_VERSION || request.reserved ||
            (request.flags & ~SPARK_SNAPSHOT_KERNEL) ||
            request.capacity > SPARK_SNAPSHOT_MAX_BYTES || !request.buffer)
                return -EINVAL;
        if (request.capacity < sizeof(*header))
        {
                request.required = sizeof(*header);
                request.used = 0;
                if (copy_to_user(out, &request, sizeof(request)))
                        return -EFAULT;
                return -ENOSPC;
        }

        mutex_lock(&snapshot_lock);
        // A caller's spare capacity is not live kernel data. Keep the warm
        // buffer, but grow beyond a page only after a capture needs more.
        u32 capacity = min_t(u32, request.capacity,
                             max_t(u32, snapshot_room, PAGE_SIZE));
retry:
        if (unlikely(capacity > snapshot_room))
        {
                u8 *larger = kvrealloc(snapshot, capacity, GFP_KERNEL);

                if (!larger)
                {
                        answer = -ENOMEM;
                        goto unlock;
                }

                snapshot = larger;
                snapshot_room = capacity;
        }

        build.data = snapshot;
        build.capacity = capacity;
        build.used = 0;
        build.required = 0;
        memory_fill(build.data, 0, sizeof(*header));
        header = snapshot_append(&build, sizeof(*header));

        header->version = SPARK_SNAPSHOT_VERSION;
        header->flags = request.flags;
        header->page_size = PAGE_SIZE;
        header->monotonic_ns = ktime_get_ns();
        header->realtime_seconds = ktime_get_real_seconds();
        header->uptime_ns = ktime_get_boottime_ns();

        if (request.flags & SPARK_SNAPSHOT_SYSTEM)
                snapshot_system(header);
        if (request.flags & SPARK_SNAPSHOT_CPU)
                snapshot_cpus(&build, header);
        if (request.flags & SPARK_SNAPSHOT_NETWORK)
                snapshot_networks(&build, header);

        if (build.required > capacity && capacity < request.capacity)
        {
                // Inventory can grow during capture. Doubling bounds this to
                // 12 retries from one page to the ABI's 16 MiB ceiling and
                // leaves ENOSPC exclusively for the caller's own capacity.
                capacity = min(max(build.required, capacity * 2), request.capacity);
                goto retry;
        }

        header->bytes = build.used;
        request.used = build.used;
        request.required = build.required;

        if (copy_to_user((void __user *)request.buffer, build.data,
                         build.used))
        {
                answer = -EFAULT;
                goto done;
        }

        if (build.required > build.capacity)
                answer = -ENOSPC;

        if (copy_to_user(out, &request, sizeof(request)))
                answer = -EFAULT;
done:
unlock:
        mutex_unlock(&snapshot_lock);
        return answer;
}

static long device_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
        switch (cmd)
        {
        case SPARK_IOCTL_SPAWN:
                return do_spawn(file, (struct spawn __user *)arg);
        case SPARK_IOCTL_STATS:
                return report_stats((struct stats __user *)arg);
        case SPARK_IOCTL_SNAPSHOT:
                return report_snapshot((struct snapshot_request __user *)arg);
        case SPARK_IOCTL_BIND:
                return report_bind((struct bind_control __user *)arg);
        case SPARK_IOCTL_SETTINGS_GET:
                return settings_get((struct spark_settings_request __user *)arg);
        case SPARK_IOCTL_SETTINGS_SET:
                return settings_set((struct spark_settings_request __user *)arg);
#ifdef CONFIG_MOONWATER_CANVAS
        case SPARK_IOCTL_INPUT_STATS:
                return report_input((struct input_stats __user *)arg);
        case SPARK_IOCTL_CURSOR_STATS:
                return report_cursor((struct cursor_stats __user *)arg);
        case SPARK_IOCTL_INPUT_DEVICES:
                return report_devices((struct input_devices __user *)arg);
        case SPARK_IOCTL_CANVAS:
                return report_canvas((struct canvas_control __user *)arg);
        case WINDOW_IOCTL_CREATE:
                return window_ioctl_create(file, arg);
        case WINDOW_IOCTL_COMMIT:
                return window_ioctl_commit(file);
#endif
        }

        return -ENOTTY;
}

/*
        misc_open leaves the miscdevice in private_data. Replace it with one
        context per open: its environment snapshot belongs to that launcher,
        and its pane belongs to that window client.
*/
static int device_open(struct inode *inode, struct file *file)
{
        struct device_context *context = kzalloc(sizeof(*context), GFP_KERNEL);

        if (!context)
                return -ENOMEM;

        mutex_init(&context->spawn_lock);
        file->private_data = context;
        return 0;
}

static int device_close(struct inode *inode, struct file *file)
{
        struct device_context *context = file->private_data;

#ifdef CONFIG_MOONWATER_CANVAS
        window_release(file);
#endif
        spawn_strings_put(context->environment);
        put_pid(context->environment_owner);
        put_cred(context->environment_cred);
        kfree(context);
        return 0;
}

static const struct file_operations device_ops = {
    .owner = THIS_MODULE,
    .open = device_open,
    .unlocked_ioctl = device_ioctl,
    .release = device_close,
#ifdef CONFIG_MOONWATER_CANVAS
    .mmap = window_mmap,
    .poll = window_poll,
#endif
    .llseek = noop_llseek,
};

// A fixed minor rather than MISC_DYNAMIC_MINOR: there is no devtmpfs here to
// materialise the node, so build.sh mknods it into the initramfs and
// both sides have to agree on the number. 240-254 is the range set aside for
// local use.
static struct miscdevice device = {
    .minor = SPARK_DEVICE_MINOR,
    .name = "spark",
    .fops = &device_ops,
    .mode = 0666,
};

// static, because the kernel has its own init_mount in fs/init.c and the
// module's symbols share one namespace with it. Nothing outside this file
// calls it, so internal linkage is the answer rather than a prefix.
static fn init_mount()
{
        const MountPoints address_to mount = mounts;

        while (mount->filesystem)
        {
                struct path path;

                int ret = kern_path(mount->path, LOOKUP_FOLLOW, &path);

                if (ret == -ENOENT && !init_mkdir(mount->path, 0755))
                        ret = kern_path(mount->path, LOOKUP_FOLLOW, &path);

                if (ret)
                {
                        pr_alert("[moonwater] " "Mounting %s to %s failed with error: %d\n", mount->filesystem, mount->path, ret);
                        mount++;
                        continue;
                }

                ret = path_mount(mount->filesystem, &path, mount->filesystem, mount->mount_flags, null);
                path_put(&path);

                //      Only the failures. Three lines saying a mount that
                //      was always going to work did work is three console
                //      writes on the boot path of every machine, at
                //      KERN_ALERT so no loglevel can turn them off, and
                //      nothing reads them -- the evidence that /proc mounted
                //      is /proc.
                if (ret)
                        pr_alert("[moonwater] " "Mounting %s on %s failed with error: %d\n", mount->filesystem, mount->path, ret);

                mount++;
        }
}

/*
        Proves the assembly runs.

        A .asm that assembles and links is not a .asm that works: until
        something calls it, the only thing the build has shown is that the
        file is syntactically valid for this architecture. This reads the
        counter twice with a barrier between, which catches the two ways a
        wrong block fails -- a counter that never advances, and one that goes
        backwards because the halves were put together the wrong way round.

        Two reads and no delay. The delta is printed rather than the value,
        because a raw counter says nothing and a delta says it is counting.
*/
// Likewise: an initcall does not need external linkage.
static b32 __init start()
{
        int ret;

        /*
                KERNEL_MODE emits only the scalar bodies: the feature gates
                become direct branches to them and the userspace SIMD bodies
                are absent from the object. Nothing in this build reads the
                feature bytes, so there is nothing to detect at init time.
        */
        pr_alert("[moonwater] " "Moonwater starting...\n");

        // Before anything that starts from them: Canvas asks at its probe.
        settings_start();

        /*
                The initramfs is unpacked on a workqueue, not inline, so at
                device_initcall time the root filesystem may still be empty --
                and mounting /proc onto a directory that does not exist yet
                fails with ENOENT rather than waiting. This is the call that
                exists to close that race, and every other early user of the
                rootfs makes it.

                It was missing and nothing went wrong, because the unpack
                happened to finish first. Tuning the kernel for latency made
                the rest of the boot quick enough to lose that race, which
                looked like the compositor breaking: no /dev, so no
                /dev/dri/card0, so nothing to attach to.
        */
        wait_for_initramfs();

#if defined(CONFIG_X86_64) || defined(CONFIG_ARM64)
        spark_cpu_features_start();
#endif
        init_mount();

        register_binfmt(&format);

        ret = misc_register(&device);
        if (ret)
        {
                pr_alert("[moonwater] " "could not register /dev/spark: %d\n", ret);
                unregister_binfmt(&format);
                return ret;
        }

        // Before the compositor: bindings have to work with no screen.
        bind_start();
        if (input_register_handler(&bind_handler))
                pr_alert("[moonwater] " "could not watch the machine's keys\n");
        else
                bind_handler_registered = true;
#ifdef CONFIG_VT
        register_keyboard_notifier(&bind_kbd_nb);
#endif
#ifdef CONFIG_PM
        register_pm_notifier(&bind_pm_nb);
#endif

#if defined(CONFIG_MOONWATER_CANVAS) && \
    defined(CONFIG_MOONWATER_CANVAS_AUTOSTART)
        canvas_start_probing();
#endif

        return 0;
}

static void __exit exit_module(void)
{
#ifdef CONFIG_MOONWATER_CANVAS
#ifdef CONFIG_MOONWATER_CANVAS_AUTOSTART
        // A probe that has not found a card owns no DRM client (and therefore
        // no module reference) to keep this callback's text resident.
        cancel_delayed_work_sync(&canvas_probe_work);
#endif
        // Before anything else: printk must stop being pointed at cells that
        // are about to be freed.
        console_stop();
        put_pid(xchg(&canvas_spawned, NULL));
#endif

        bind_stop();

        misc_deregister(&device);
        kvfree(snapshot);
        kfree(settings_current);
        unregister_binfmt(&format);
        pr_alert("[moonwater] " "Spark format unregistered\n");
}

/*
        device_initcall, the same level the display drivers register at.

        Link order puts kernel/ ahead of drivers/, so this runs before them:
        /dev is mounted and the poll for a display starts while the drivers are
        still coming up, and the compositor takes the device the moment it
        appears rather than a hundred milliseconds later. Waiting until every
        driver had finished cost exactly that.

        Anything earlier is not possible: the initramfs is not unpacked until
        rootfs_initcall, so before this point there is no /dev to mount onto.
*/
// Use device_initcall for built-in, or module_init for module
#ifdef MODULE
module_init(start);
module_exit(exit_module);
MODULE_AUTHOR("Dawn Larsson");
MODULE_DESCRIPTION("Spark direct binary format");
#else
device_initcall(start);
#endif
