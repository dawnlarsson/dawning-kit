#include <linux/module.h>
#include <linux/init.h>
#include <linux/namei.h>
#include <linux/binfmts.h>
#include <linux/sched/task_stack.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/fs.h>
#include <linux/mount.h>

#define DAWN_MODERN_C_KERNEL
#include "../../standard/library.c"

#define log_k(fmt, ...) \
        pr_alert("[Dawning] " fmt, ##__VA_ARGS__)

#define SPARK_BASE_ADDR 0x400000

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
        unsigned long entry_addr, stack_addr;
        loff_t file_size;
        int ret;

        ret = begin_new_exec(bprm);
        if (ret)
        {
                log_k("begin_new_exec failed: %d\n", ret);
                return ret;
        }

        setup_new_exec(bprm);

        ret = setup_arg_pages(bprm, STACK_TOP, EXSTACK_DEFAULT);
        if (ret < 0)
        {
                log_k("setup_arg_pages failed: %d\n", ret);
                return ret;
        }

        file_size = i_size_read(file_inode(bprm->file));

        if (file_size <= 0)
        {
                log_k("Invalid file size\n");
                return -ENOEXEC;
        }

        entry_addr = vm_mmap(bprm->file, 0, file_size, PROT_READ | PROT_EXEC, MAP_PRIVATE, 0);

        if (IS_ERR_VALUE(entry_addr))
        {
                log_k("vm_mmap FAILED: %ld\n", (long)entry_addr);
                return entry_addr;
        }

        set_binfmt(&spark_format);

        stack_addr = current->mm->start_stack;

#ifdef CONFIG_X86_64
        regs->ip = entry_addr;
        regs->sp = stack_addr;
        regs->flags = 0x202; // IF flag set
        regs->cs = __USER_CS;
        regs->ss = __USER_DS;
#elif defined(CONFIG_ARM64)
        regs->pc = entry_addr;
        regs->sp = stack_addr;
        regs->pstate = PSR_MODE_EL0t;
#elif defined(CONFIG_RISCV)
        regs->epc = entry_addr;
        regs->sp = stack_addr;
        regs->status = SR_SPIE;
#endif

        finalize_exec(bprm);

        return 0;
}

fn dawn_init_mount()
{
        MountPoints address_to mount = mounts;

        while (mount->filesystem)
        {
                struct path path;
                int ret = kern_path(mount->path, LOOKUP_FOLLOW, &path);

                if (ret)
                {
                        log_k("Path lookup for %s failed: %d\n", mount->path, ret);
                        mount++;
                        continue;
                }

                // Use do_mount or newer APIs depending on kernel version
                // For modern kernels (5.9+), mounting is more restricted
                // This might need to be done from initramfs instead
                log_k("Would mount %s to %s\n",
                      mount->filesystem, mount->path);

                path_put(&path);
                mount++;
        }
}

b32 __init dawn_start()
{
        log_k("Dawning Eos - starting...\n");

        dawn_init_mount();

        register_binfmt(&spark_format);

        log_k("Spark format registered\n");

        return 0;
}

static void __exit dawn_exit(void)
{
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