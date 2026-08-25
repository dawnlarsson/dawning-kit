#include "../std/library.c"
#include "../std/spark.c"

// Reports how long the kernel takes from a pointer event arriving to the
// cursor being on screen. Move the mouse, then run this.
b32 main()
{
        b32 device = system_call_4(syscall(openat), AT_FDCWD,
                                   (positive)SPARK_DEVICE, FILE_READ_WRITE, 0);

        if (device < 0)
        {
                string_format(log, "cannot open %s: %b\n", SPARK_DEVICE, device);
                log_flush();
                return 1;
        }

        struct input_stats stats;

        if (system_call_3(syscall(ioctl), device, SPARK_IOCTL_INPUT_STATS,
                          (positive)address_of stats) != 0)
        {
                string_format(log, "could not read input stats\n");
                log_flush();
                return 1;
        }

        if (!stats.events)
        {
                string_format(log, "no pointer movement seen yet\n");
                log_flush();
                return 0;
        }

        string_format(log, "pointer events   %p\n", stats.events);
        string_format(log, "event to screen  %p ns mean, %p ns worst\n",
                      stats.mean_ns, stats.worst_ns);
        string_format(log, "  queued         %p ns\n", stats.queue_ns);
        string_format(log, "  drawing        %p ns\n", stats.draw_ns);
        string_format(log, "  flush          %p ns\n", stats.flush_ns);
        log_flush();
        return 0;
}
