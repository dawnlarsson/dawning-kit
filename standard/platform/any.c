#include "../library.c"

#define MS_NONE 0
#define MS_RDONLY 1
#define MS_NOSUID 2
#define MS_NODEV 4
#define MS_NOEXEC 8
#define MS_SYNCHRONOUS 16
#define MS_REMOUNT 32
#define MS_MANDLOCK 64
#define MS_DIRSYNC 128
#define MS_NOSYMFOLLOW 256
#define MS_NOATIME 1024
#define MS_NODIRATIME 2048
#define MS_BIND 4096
#define MS_MOVE 8192
#define MS_REC 16384
#define MS_SILENT 32768
#define MS_POSIXACL (1 << 16)
#define MS_UNBINDABLE (1 << 17)
#define MS_PRIVATE (1 << 18)
#define MS_SLAVE (1 << 19)
#define MS_SHARED (1 << 20)
#define MS_RELATIME (1 << 21)
#define MS_KERNMOUNT (1 << 22)
#define MS_I_VERSION (1 << 23)
#define MS_STRICTATIME (1 << 24)

#define DT_DIR 4
#define DT_REG 8
#define DT_LNK 10
#define DT_FIFO 1
#define DT_SOCK 12
#define DT_CHR 2
#define DT_BLK 6

#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_CHILD_SETTID 0x01000000

struct timespec
{
        positive tv_sec;
        positive tv_nsec;
};

// Logging ---------------------------------------------------

#define MAX_INPUT 4096

p8 log_writer_buffer[MAX_INPUT];
positive log_writer_buffer_length;

fn log_direct(address_any data, positive length)
{
        system_call_3(syscall(write), stdout, (positive)data, length);
}

fn log_flush()
{
        if (log_writer_buffer_length == 0)
                return;

        log_direct(log_writer_buffer, log_writer_buffer_length);

        log_writer_buffer_length = 0;
}

fn log(address_any data, positive length)
{
        if (length == 0)
                length = string_length(data);

        if (length + log_writer_buffer_length > MAX_INPUT)
                log_flush();

        if (length < MAX_INPUT)
        {
                memory_copy(log_writer_buffer + log_writer_buffer_length, data, length);
                log_writer_buffer_length += length;
                return;
        }

        log_direct(log_writer_buffer, log_writer_buffer_length);
        log_direct(data, length);
}