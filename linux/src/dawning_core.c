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
        if (bprm->buf == NULL)
                return -ENOEXEC;

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

        if (i_size_read(file_inode(bprm->file)) < (loff_t)(SPARK_PAGE + header->text_size + header->data_size))
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

        // text + rodata, mapped straight from the file. MAP_FIXED because the
        // image is not position independent.
        text = vm_mmap(bprm->file, header->base, header->text_size,
                       PROT_READ | PROT_EXEC,
                       MAP_PRIVATE | MAP_FIXED | MAP_EXECUTABLE,
                       SPARK_PAGE);

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
                               MAP_PRIVATE | MAP_FIXED,
                               SPARK_PAGE + header->text_size);

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