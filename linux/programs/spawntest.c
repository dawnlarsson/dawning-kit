#include "../../standard/library.c"
#include "../../standard/spark.c"

// Proves /dev/spark actually spawns: the child has to produce output and its
// exit status has to come back through wait4 like any other child.
positive spawn(b32 device, string_address path, positive status_out)
{
        struct spawn request;
        p8 block[256];
        positive length = string_length(path);

        memory_copy(block, path, length + 1);

        request.path = (unsigned long)path;
        request.argv = (unsigned long)block;
        request.argv_bytes = length + 1;
        request.argv_count = 1;

        bipolar child = system_call_3(syscall(ioctl), device, SPARK_IOCTL_SPAWN,
                                      (positive)address_of request);

        if (child < 0)
        {
                string_format(log, "  spawn %s -> failed %b\n", path, child);
                return 0;
        }

        positive status = 0;
        system_call_4(syscall(wait4), child, (positive)address_of status, 0, 0);
        string_format(log, "  spawn %s -> pid %p exit %p\n", path, child, status >> 8 & 0xff);
        return 1;
}

b32 main()
{
        b32 device = system_call_4(syscall(openat), AT_FDCWD,
                                   (positive)SPARK_DEVICE, FILE_READ_WRITE, 0);

        string_format(log, "/dev/spark fd %b\n", device);
        log_flush();

        if (device < 0)
                return 1;

        spawn(device, "/duck", 0);
        log_flush();
        spawn(device, "/exit7", 0);
        log_flush();
        spawn(device, "/sparktest", 0);
        log_flush();
        spawn(device, "/nosuchprogram", 0);
        log_flush();

        return 0;
}
