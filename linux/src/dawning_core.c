#include <linux/module.h>
#include <linux/init.h>
#include <linux/namei.h>
#include <linux/binfmts.h>
#include <linux/sched/task_stack.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/sched/task.h>

#define DAWN_MODERN_C_KERNEL
#include "../../standard/library.c"
#include "../../standard/spark.h"

int path_mount(const char *dev_name, struct path *path,
               const char *type_page, unsigned long flags, void *data_page);

#define log_k(fmt, ...) \
        pr_alert("[Dawning] " fmt, ##__VA_ARGS__)


typedef struct
{
        string_address filesystem;
        string_address path;
        positive mount_flags;
} MountPoints;

MountPoints mounts[] = {
    {"proc", "/proc", 0},
    {"sysfs", "/sys", 0},
    {null, null},
};

static int execute_spark(struct linux_binprm *bprm);

static struct linux_binfmt spark_format = {
    .module = THIS_MODULE,
    .load_binary = execute_spark,
};

static int execute_spark(struct linux_binprm *bprm)
{
        struct pt_regs *regs = task_pt_regs(current);
        const struct spark_header *header;
        unsigned long text, data, bss, stack_addr;
        int ret;

        // Everything up to begin_new_exec runs while the old process is still
        // intact, so a file that is not ours must be rejected here: returning
        // -ENOEXEC lets the next handler try, and leaves the caller alive.
        // The kernel has already read the first BINPRM_BUF_SIZE bytes for us.
        header = (const struct spark_header *)bprm->buf;

        if (header->magic != SPARK_MAGIC)
                return -ENOEXEC;

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

        if (header->entry < header->base || header->entry >= header->base + header->text_size)
                return -ENOEXEC;

        if (i_size_read(file_inode(bprm->file)) < (loff_t)(header->text_size + header->data_size))
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

        // text + rodata, mapped straight from the file at offset 0: the header
        // sits in the first 64 bytes of this mapping and entry points past it.
        // MAP_FIXED because the image is not position independent, and
        // MAP_POPULATE because these pages are always touched immediately --
        // faulting them in one at a time is pure latency for a short program.
        text = vm_mmap(bprm->file, header->base, header->text_size,
                       PROT_READ | PROT_EXEC,
                       MAP_PRIVATE | MAP_FIXED | MAP_POPULATE,
                       0);

        if (IS_ERR_VALUE(text))
        {
                log_k("mapping text failed: %ld\n", (long)text);
                force_fatal_sig(SIGKILL);
                return (int)text;
        }

        if (header->data_size)
        {
                data = vm_mmap(bprm->file, header->base + header->text_size,
                               header->data_size,
                               PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_FIXED | MAP_POPULATE,
                               header->text_size);

                if (IS_ERR_VALUE(data))
                {
                        log_k("mapping data failed: %ld\n", (long)data);
                        force_fatal_sig(SIGKILL);
                        return (int)data;
                }
        }

        // bss is anonymous, so it arrives zeroed and there is no tail of a
        // file backed page to clear by hand.
        if (header->bss_size)
        {
                bss = vm_mmap(NULL,
                              header->base + header->text_size + header->data_size,
                              header->bss_size,
                              PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS,
                              0);

                if (IS_ERR_VALUE(bss))
                {
                        log_k("mapping bss failed: %ld\n", (long)bss);
                        force_fatal_sig(SIGKILL);
                        return (int)bss;
                }
        }

        current->mm->start_code = header->base;
        current->mm->end_code = header->base + header->text_size;
        current->mm->start_data = header->base + header->text_size;
        current->mm->end_data = header->base + header->text_size + header->data_size;
        current->mm->brk = current->mm->start_brk =
            header->base + header->text_size + header->data_size + header->bss_size;

        set_binfmt(&spark_format);

        stack_addr = current->mm->start_stack;

#ifdef CONFIG_X86_64
        regs->ip = header->entry;
        regs->sp = stack_addr;
        regs->flags = 0x202; // IF flag set
        regs->cs = __USER_CS;
        regs->ss = __USER_DS;
#elif defined(CONFIG_ARM64)
        regs->pc = header->entry;
        regs->sp = stack_addr;
        regs->pstate = PSR_MODE_EL0t;
#elif defined(CONFIG_RISCV)
        regs->epc = header->entry;
        regs->sp = stack_addr;
        regs->status = SR_SPIE;
#endif

        finalize_exec(bprm);

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

struct spark_spawn_work {
        char *path;
        char **argv;
        char *argv_block;
        char **envp;
        char *envp_block;
};

static void spark_spawn_free(struct spark_spawn_work *work)
{
        if (!work)
                return;

        kfree(work->envp_block);
        kfree(work->envp);
        kfree(work->argv_block);
        kfree(work->argv);
        kfree(work->path);
        kfree(work);
}

static int spark_spawn_enter(void *data)
{
        struct spark_spawn_work *work = data;
        static const char *const empty_envp[] = {NULL};
        int ret;

        ret = kernel_execve(work->path, (const char *const *)work->argv,
                            work->envp ? (const char *const *)work->envp : empty_envp);

        // kernel_execve has copied everything it needs by now, so the request
        // can go before anything else touches it.
        spark_spawn_free(work);

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
static int spark_copy_strings(unsigned long user_block, unsigned int bytes,
                              unsigned int count, char **out_block, char ***out_vector)
{
        char *block;
        char **vector;
        char *walk;
        unsigned int i;

        if (count == 0 || count > 256 || bytes == 0 || bytes > PAGE_SIZE)
                return -EINVAL;

        block = kmalloc(bytes + 1, GFP_KERNEL);
        if (!block)
                return -ENOMEM;

        if (copy_from_user(block, (const void __user *)user_block, bytes))
        {
                kfree(block);
                return -EFAULT;
        }

        block[bytes] = 0;

        vector = kcalloc(count + 1, sizeof(char *), GFP_KERNEL);
        if (!vector)
        {
                kfree(block);
                return -ENOMEM;
        }

        walk = block;
        for (i = 0; i < count; i++)
        {
                if (walk >= block + bytes)
                {
                        kfree(vector);
                        kfree(block);
                        return -EINVAL;
                }

                vector[i] = walk;
                walk += strlen(walk) + 1;
        }
        vector[count] = NULL;

        *out_block = block;
        *out_vector = vector;
        return 0;
}

static long spark_do_spawn(struct spark_spawn __user *request)
{
        struct spark_spawn args;
        struct spark_spawn_work *work;
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

        ret = spark_copy_strings(args.argv, args.argv_bytes, args.argv_count,
                                 &work->argv_block, &work->argv);
        if (ret)
                goto fail;

        if (args.envp && args.envp_count)
        {
                ret = spark_copy_strings(args.envp, args.envp_bytes, args.envp_count,
                                         &work->envp_block, &work->envp);
                if (ret)
                        goto fail;
        }

        // SIGCHLD alone, so the new task is an ordinary child of the caller
        // and wait4 works on it the same way it does for a fork.
        pid = user_mode_thread(spark_spawn_enter, work, SIGCHLD);

        if (pid < 0)
        {
                ret = pid;
                goto fail;
        }

        // work is owned by the new task from here.
        return pid;

fail:
        spark_spawn_free(work);
        return ret;
}

static long spark_device_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
        if (cmd != SPARK_IOCTL_SPAWN)
                return -ENOTTY;

        return spark_do_spawn((struct spark_spawn __user *)arg);
}

static const struct file_operations spark_device_ops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = spark_device_ioctl,
    .llseek = noop_llseek,
};

// A fixed minor rather than MISC_DYNAMIC_MINOR: there is no devtmpfs here to
// materialise the node, so script/fs_setup mknods it into the initramfs and
// both sides have to agree on the number. 240-254 is the range set aside for
// local use.
static struct miscdevice spark_device = {
    .minor = SPARK_DEVICE_MINOR,
    .name = "spark",
    .fops = &spark_device_ops,
    .mode = 0666,
};

fn dawn_init_mount()
{
        MountPoints address_to mount = mounts;

        while (mount->filesystem)
        {
                struct path path;

                int ret = kern_path(mount->path, LOOKUP_FOLLOW, &path);

                if (ret)
                {
                        log_k("Mounting %s to %s failed with error: %d\n", mount->filesystem, mount->path, ret);
                        mount++;
                        continue;
                }

                ret = path_mount(mount->filesystem, &path, mount->filesystem, mount->mount_flags, null);
                path_put(&path);

                if (!ret)
                        log_k("Mounted %s to %s\n", mount->filesystem, mount->path);

                mount++;
        }
}

b32 __init dawn_start()
{
        log_k("Dawning Eos - starting...\n");

        dawn_init_mount();

        register_binfmt(&spark_format);

        if (misc_register(&spark_device))
                log_k("could not register /dev/spark\n");

        return 0;
}

static void __exit dawn_exit(void)
{
        misc_deregister(&spark_device);
        unregister_binfmt(&spark_format);
        log_k("Spark format unregistered\n");
}

// Use late_initcall for built-in, or module_init for module
#ifdef MODULE
module_init(dawn_start);
module_exit(dawn_exit);
MODULE_AUTHOR("Dawn Larsson");
MODULE_DESCRIPTION("Spark direct binary format");
#else
late_initcall(dawn_start);
#endif