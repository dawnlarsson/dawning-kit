/*
        A qemu plugin that counts guest instructions.

        Built and used by kit/insns. qemu ships one of these -- libinsn.so --
        but it is not installed everywhere and the whole of what is needed from
        it is the count, so it is here rather than assumed.

        Every translation block is asked how many instructions it holds when it
        is translated, and every execution of it adds that many. Single
        threaded, which the programs under kit/ are.
*/
#include <qemu-plugin.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

static uint64_t total;

static void tb_exec(unsigned int cpu, void *udata)
{
        total += (uint64_t)(uintptr_t)udata;
}

static void tb_trans(struct qemu_plugin_tb *tb, void *udata_unused)
{
        size_t n = qemu_plugin_tb_n_insns(tb);
        qemu_plugin_register_vcpu_tb_exec_cb(tb, tb_exec, QEMU_PLUGIN_CB_NO_REGS,
                                             (void *)(uintptr_t)n);
}

static void done(void *p)
{
        fprintf(stderr, "%llu\n", (unsigned long long)total);
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t *info,
                                           int argc, char **argv)
{
        qemu_plugin_register_vcpu_tb_trans_cb(id, tb_trans, NULL);
        qemu_plugin_register_atexit_cb(id, done, NULL);
        return 0;
}
