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

#ifdef CONFIG_X86_64
#include <asm/cpufeature.h>
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
#endif

#define STANDARD_MODERN_C_KERNEL
#include "compiler_memory.c"
#include "spark.c"

// Defined below, next to the rest of the spawning, and called only by the
// compositor when it has a screen to put something on.
#ifdef CONFIG_MOONWATER_CANVAS
static int spawn_program(const char *path);
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
        struct pane *pane;
};

#ifdef CONFIG_MOONWATER_CANVAS
#include <linux/workqueue.h>
#include <linux/kthread.h>
#include <linux/pm_qos.h>
#include <linux/input.h>
#include <linux/math64.h>
#include <linux/minmax.h>
#include <linux/list_sort.h>
#include <linux/hrtimer.h>
#include <linux/font.h>
#include <drm/drm_file.h>
#include <drm/drm_rect.h>
#include "canvas/canvas.c"
#endif

// The assembly in this directory. Each .asm is its own object -- assembly
// cannot be included into this translation unit the way display.c is
// -- so the compiler is told its shape here, in the file that calls it.
//
// the free running counter, in library.c
u64 get_cpu_time(void);



int path_mount(const char *dev_name, struct path *path,
               const char *type_page, unsigned long flags, void *data_page);

// Declared rather than included: linux/init_syscalls.h also declares an
// init_mount, and this file has one of its own.
int init_mkdir(const char *pathname, umode_t mode);

#define log_k(fmt, ...) \
        pr_alert("[moonwater] " fmt, ##__VA_ARGS__)

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

// Spawns are serialised by the caller waiting on each one, so plain counters
// are accurate enough here and cost nothing.
static unsigned long stat_spawns;
static unsigned long stat_task_ns;
static unsigned long stat_exec_ns;
static unsigned long stat_loader_ns;
static unsigned long stat_loads;
static unsigned long stat_map_ns;

static int execute_spark(struct linux_binprm *bprm);

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
        unsigned long text, data, bss, stack_addr, span;
        unsigned long text_populate, data_populate, bss_populate;
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
        data = bss = 0;
        text_populate = data_populate = bss_populate = 0;

        if (header->version != SPARK_VERSION)
        {
                log_k("unsupported spark version %u\n", header->version);
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
                log_k("setup_arg_pages failed: %d\n", ret);
                force_fatal_sig(SIGKILL);
                return ret;
        }

        map_started = ktime_get_ns();

        // vm_mmap takes and drops mmap_lock around every call. This address
        // space was created moments ago and nothing else can see it yet, so
        // the three regions go up under one write lock instead of three
        // acquire/release cycles, using do_mmap directly.
        //
        // do_mmap does not populate; it reports how much wants populating and
        // the caller does it after dropping the lock.
        if (mmap_write_lock_killable(current->mm))
        {
                force_fatal_sig(SIGKILL);
                return -EINTR;
        }

        text = do_mmap(bprm->file, header->base, header->text_size,
                       PROT_READ | PROT_EXEC,
                       MAP_PRIVATE | MAP_FIXED, 0, 0, &text_populate, NULL);

        if (IS_ERR_VALUE(text))
        {
                mmap_write_unlock(current->mm);
                log_k("mapping text failed: %ld\n", (long)text);
                force_fatal_sig(SIGKILL);
                return (int)text;
        }

        if (header->data_size)
        {
                data = do_mmap(bprm->file, header->base + header->text_size,
                               header->data_size,
                               PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_FIXED,
                               0, header->text_size >> PAGE_SHIFT,
                               &data_populate, NULL);

                if (IS_ERR_VALUE(data))
                {
                        mmap_write_unlock(current->mm);
                        log_k("mapping data failed: %ld\n", (long)data);
                        force_fatal_sig(SIGKILL);
                        return (int)data;
                }
        }

        // bss is anonymous, so it arrives zeroed and there is no tail of a
        // file backed page to clear by hand.
        if (header->bss_size)
        {
                bss = do_mmap(NULL,
                              header->base + header->text_size + header->data_size,
                              header->bss_size,
                              PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS,
                              0, 0, &bss_populate, NULL);

                if (IS_ERR_VALUE(bss))
                {
                        mmap_write_unlock(current->mm);
                        log_k("mapping bss failed: %ld\n", (long)bss);
                        force_fatal_sig(SIGKILL);
                        return (int)bss;
                }
        }

        mmap_write_unlock(current->mm);

        // Populating eagerly was measured to move about 800ns out of page
        // faults and into here, with no end to end difference at these sizes,
        // so the regions are left to fault in. The hooks stay because a larger
        // image may well tip the other way -- do_mmap reports what wants
        // populating and MAP_POPULATE is all it takes to turn back on.
        if (text_populate)
                mm_populate(header->base, text_populate);
        if (data_populate)
                mm_populate(header->base + header->text_size, data_populate);

        stat_map_ns += ktime_get_ns() - map_started;

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
                log_k("could not lay out the arguments: %d\n", ret);
                force_fatal_sig(SIGKILL);
                return ret;
        }

#ifdef CONFIG_X86_64
        /* CPUID and XGETBV are serialising startup work whose answer the
           kernel already has.  Spark's private entry ABI hands that answer
           to _start; an image run by an older loader simply misses the magic
           and retains its userspace detection fallback. */
        regs->r12 = SPARK_START_MAGIC;
        regs->r13 = 0;
        regs->r14 = task_pid_nr(current);

        if (cpu_feature_enabled(X86_FEATURE_AVX2))
                regs->r13 |= SPARK_CPU_AVX2;

        if (cpu_feature_enabled(X86_FEATURE_AVX512F) &&
            cpu_feature_enabled(X86_FEATURE_AVX512BW) &&
            cpu_feature_enabled(X86_FEATURE_AVX512VL))
                regs->r13 |= SPARK_CPU_AVX512;

        regs->ip = header->entry;
        regs->sp = stack_addr;
        regs->flags = 0x202; // IF flag set
        regs->cs = __USER_CS;
        regs->ss = __USER_DS;
#elif defined(CONFIG_ARM64)
        regs->regs[19] = SPARK_START_MAGIC;
        regs->regs[20] = 0;
        regs->regs[21] = task_pid_nr(current);
        regs->pc = header->entry;
        regs->sp = stack_addr;
        regs->pstate = PSR_MODE_EL0t;
#elif defined(CONFIG_RISCV)
        regs->s2 = SPARK_START_MAGIC;
        regs->s3 = 0;
        regs->s4 = task_pid_nr(current);
        regs->epc = header->entry;
        regs->sp = stack_addr;
        regs->status = SR_SPIE;
#endif

        finalize_exec(bprm);

        // Everything before this in kernel_execve is the generic prologue:
        // allocating a bprm, opening the file, building a throwaway mm to hold
        // argv and then transplanting its stack. This counter is only our part.
        stat_loader_ns += ktime_get_ns() - loader_started;
        stat_loads++;

        return 0;
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
};

static void spawn_strings_put(struct spawn_strings *strings)
{
        if (strings && refcount_dec_and_test(&strings->references))
                kvfree(strings);
}

static void spawn_free(struct spawn_work *work)
{
        spawn_strings_put(work->environment);
        spawn_strings_put(work->arguments);
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
static int spawn_program(const char *path)
{
        struct spawn_work *work = kzalloc(sizeof(*work), GFP_KERNEL);

        if (!work)
                return -ENOMEM;

        work->path = kstrdup(path, GFP_KERNEL);
        work->arguments = kvmalloc(sizeof(*work->arguments) +
                                   2 * sizeof(char *), GFP_KERNEL);

        if (!work->path || !work->arguments)
        {
                spawn_free(work);
                return -ENOMEM;
        }

        refcount_set(&work->arguments->references, 1);
        work->arguments->vector = (char **)(work->arguments + 1);
        work->arguments->vector[0] = work->path;
        work->arguments->vector[1] = NULL;
        work->argc = 1;

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
        int ret;

        spawn_default_signals();

        ret = kernel_execve(work->path,
                            (const char *const *)work->arguments->vector,
                            work->environment
                              ? (const char *const *)work->environment->vector
                              : empty_envp);

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
                unsigned int i;

                script_argv = kcalloc((size_t)work->argc + 2,
                                      sizeof(*script_argv), GFP_KERNEL);

                if (!script_argv)
                        ret = -ENOMEM;
                else
                {
                        script_argv[0] = "/bin/sh";
                        script_argv[1] = work->path;

                        for (i = 1; i < work->argc; i++)
                                script_argv[i + 1] = work->arguments->vector[i];

                        ret = kernel_execve(script_argv[0], script_argv,
                                            work->environment
                                              ? (const char *const *)work->environment->vector
                                              : empty_envp);
                        kfree(script_argv);
                }
        }

        stat_exec_ns += ktime_get_ns() - started;

        // kernel_execve has copied everything it needs by now, so the request
        // can go before anything else touches it.
        spawn_free(work);

        if (ret)
        {
                // The task exists by the time exec is attempted, so a bad path
                // cannot come back as an ioctl error. Exiting 127 is what a
                // shell reports for "could not run it", and it keeps the
                // caller from mistaking the failure for a clean exit.
                log_k("spawn: exec failed: %d\n", ret);
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
        strings = kvmalloc(sizeof(*strings) + pointer_bytes + bytes, GFP_KERNEL);
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
                size_t remaining;
                size_t length;

                if (walk >= block + bytes)
                {
                        kvfree(strings);
                        return -EINVAL;
                }

                remaining = (size_t)(block + bytes - walk);
                length = strnlen(walk, remaining);

                if (length == remaining)
                {
                        kvfree(strings);
                        return -EINVAL;
                }

                vector[i] = walk;
                walk += length + 1;
        }
        vector[count] = NULL;

        *out = strings;
        return 0;
}

static long do_spawn(struct file *file, struct spawn __user *request,
                     bool shell_fallback)
{
        struct device_context *context = file->private_data;
        struct spawn args;
        struct spawn_work *work;
        struct spawn_strings *old_environment = NULL;
        struct pid *old_owner = NULL;
        long ret;
        pid_t pid;

        if (copy_from_user(&args, request, sizeof(args)))
                return -EFAULT;

        work = kzalloc(sizeof(*work), GFP_KERNEL);
        if (!work)
                return -ENOMEM;

        work->path = strndup_user((const char __user *)args.path, PATH_MAX);
        if (IS_ERR(work->path))
        {
                ret = PTR_ERR(work->path);
                work->path = NULL;
                goto fail;
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
                                refcount_inc(&work->environment->references);
                                context->environment = work->environment;
                                context->environment_generation =
                                        args.envp_generation;
                                context->environment_owner =
                                        get_pid(task_tgid(current));
                                mutex_unlock(&context->spawn_lock);
                                spawn_strings_put(old_environment);
                                put_pid(old_owner);
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
                stat_task_ns += ktime_get_ns() - started;
                stat_spawns++;
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
            .spawns = stat_spawns,
            .task_ns = stat_task_ns,
            .exec_ns = stat_exec_ns,
            .loader_ns = stat_loader_ns,
            .loads = stat_loads,
            .map_ns = stat_map_ns,
        };

        if (copy_to_user(out, &stats, sizeof(stats)))
                return -EFAULT;

        return 0;
}

#ifdef CONFIG_MOONWATER_CANVAS
static long report_input(struct input_stats __user *out)
{
        struct input_stats stats;

        canvas_input_stats(&stats);

        if (copy_to_user(out, &stats, sizeof(stats)))
                return -EFAULT;

        return 0;
}

static long report_cursor(struct cursor_stats __user *out)
{
        struct cursor_stats stats;

        canvas_cursor_stats(&stats);

        if (copy_to_user(out, &stats, sizeof(stats)))
                return -EFAULT;

        return 0;
}
#endif

static long device_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
        switch (cmd)
        {
        case SPARK_IOCTL_SPAWN:
                return do_spawn(file, (struct spawn __user *)arg, false);
        case SPARK_IOCTL_SPAWN_SHELL:
                return do_spawn(file, (struct spawn __user *)arg, true);
        case SPARK_IOCTL_STATS:
                return report_stats((struct stats __user *)arg);
#ifdef CONFIG_MOONWATER_CANVAS
        case SPARK_IOCTL_INPUT_STATS:
                return report_input((struct input_stats __user *)arg);
        case SPARK_IOCTL_CURSOR_STATS:
                return report_cursor((struct cursor_stats __user *)arg);
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
                        log_k("Mounting %s to %s failed with error: %d\n", mount->filesystem, mount->path, ret);
                        mount++;
                        continue;
                }

                ret = path_mount(mount->filesystem, &path, mount->filesystem, mount->mount_flags, null);
                path_put(&path);

                if (ret)
                        log_k("Mounting %s on %s failed with error: %d\n",
                              mount->filesystem, mount->path, ret);
                else
                        log_k("Mounted %s to %s\n", mount->filesystem, mount->path);

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
static void __init check_ticks(void)
{
        u64 first = get_cpu_time();
        u64 second;

        barrier();
        second = get_cpu_time();

        if (second > first)
                log_k("ticks: counting, %llu between two reads\n",
                      (unsigned long long)(second - first));
        else
                log_k("ticks: did not advance (%llu then %llu)\n",
                      (unsigned long long)first, (unsigned long long)second);
}

// Likewise: an initcall does not need external linkage.
static b32 __init start()
{
        /*
                KERNEL_MODE emits only the scalar bodies: the feature gates
                become direct branches to them and the userspace SIMD bodies
                are absent from the object. Nothing in this build reads the
                feature bytes, so there is nothing to detect at init time.
        */
        log_k("Moonwater starting...\n");

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

        check_ticks();
        init_mount();

        register_binfmt(&format);

        if (misc_register(&device))
                log_k("could not register /dev/spark\n");

#if defined(CONFIG_MOONWATER_CANVAS) && \
    defined(CONFIG_MOONWATER_CANVAS_AUTOSTART)
        canvas_start_probing();
#endif

        return 0;
}

static void __exit exit_module(void)
{
#ifdef CONFIG_MOONWATER_CANVAS
        // Before anything else: printk must stop being pointed at cells that
        // are about to be freed.
        console_stop();
#endif

        misc_deregister(&device);
        unregister_binfmt(&format);
        log_k("Spark format unregistered\n");
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
