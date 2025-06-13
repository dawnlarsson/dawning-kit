#include <linux/namei.h>
#include <linux/binfmts.h>
#include <linux/sched/task_stack.h>

// todo: investigate the requirement to manually include this
int path_mount(const char *dev_name, struct path *path,
               const char *type_page, unsigned long flags, void *data_page);

#define DAWN_MODERN_C_KERNEL
#include "../../standard/library.c"

#define log_k(fmt, ...) \
        pr_alert("[Dawning] " fmt, ##__VA_ARGS__)

typedef struct
{
        string_address filesystem;
        string_address path;
        positive mount_flags;
} MountPoints;

MountPoints mounts[] = {
    {"proc", "/proc", MNT_INTERNAL},
    {"sysfs", "/sys", MNT_INTERNAL},
    {null, null},
};

static int execute_spark(struct linux_binprm *bprm);

static struct linux_binfmt spark_format = {
    .load_binary = execute_spark,
};

//      Dawning Spark
//      is a simple direct binary executable format,
//      the very first byte of the file IS the entry point
//      and is DIRECTLY executed, this means no elf parsing overhead for each invocation!
//
//      potential future "spark+" format (this would be separate):
//      in the file descriptor of the executables, it's expected
//      to have defined the target architecture in a custom header
//
static int execute_spark(struct linux_binprm *bprm)
{
        log_k("Executing Spark binary: %s\n", bprm->file->f_path.dentry->d_name.name);

        int new_exec = begin_new_exec(bprm);

        if (new_exec)
                return new_exec;

        setup_new_exec(bprm);

        new_exec = setup_arg_pages(bprm, STACK_TOP, EXSTACK_DEFAULT);
        if (new_exec < 0)
                return new_exec;

        set_binfmt(&spark_format);

        finalize_exec(bprm);
        start_thread(task_pt_regs(current), (unsigned long)bprm->file->f_path.dentry, current->mm->start_stack);

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

late_initcall(dawn_start);
