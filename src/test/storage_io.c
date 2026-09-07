#include "../compiler_memory.c"
#include "counted.inc"

/* Inject only positional reads. Everything else reaches the real syscall
   ABI, so stdbuf can open/read/close a real ELF-header fixture while its
   out-of-line program table receives deterministic short reads and EINTR. */
static positive storage_test_mode, storage_test_calls, storage_test_number;
static positive storage_test_offset, storage_test_length, storage_test_used;
static bool storage_test_arguments, storage_test_preclear;
static p8 address_to storage_test_destination;

static bipolar storage_test_call4(positive number, positive handle,
                                  positive into, positive length,
                                  positive offset)
{
        if (number != storage_test_number || !storage_test_mode)
                return (system_call_4)(number, handle, into, length, offset);

        p8 address_to bytes = (p8 address_to)into;
        if (!storage_test_calls && storage_test_mode != 5)
                for (positive i = 0; i < storage_test_length; i++)
                        if (storage_test_destination[i] != 0xa5)
                                storage_test_preclear = true;

        if (offset != storage_test_offset + storage_test_used ||
            length != storage_test_length - storage_test_used ||
            (storage_test_destination &&
             bytes != storage_test_destination + storage_test_used))
                storage_test_arguments = false;

        storage_test_calls++;
        if (storage_test_calls == 1 &&
            (storage_test_mode == 1 || storage_test_mode == 5))
                return -4;
        if (storage_test_mode == 3 ||
            (storage_test_mode == 4 && storage_test_used == 7))
                return -5;
        if (storage_test_mode == 2 && storage_test_used == 7)
                return 0;

        positive take = min(length, (positive)7);
        if (number == syscall(pread64))
                for (positive i = 0; i < take; i++)
                        bytes[i] = storage_test_mode == 5
                                       ? (storage_test_used + i == 0 ? 3 : 0)
                                       : (p8)(storage_test_used + i + 1);
        storage_test_used += take;
        return (bipolar)take;
}

#define system_call_4(...) storage_test_call4(__VA_ARGS__)
#include "../spark.c"
#include "../sh/shell.c"
#undef system_call_4

static fn storage_test_begin(positive mode, positive length, positive offset,
                             p8 address_to bytes)
{
        storage_test_mode = mode;
        storage_test_number = syscall(pread64);
        storage_test_calls = storage_test_used = 0;
        storage_test_offset = offset;
        storage_test_length = length;
        storage_test_arguments = true;
        storage_test_preclear = false;
        storage_test_destination = bytes;
}

static fn storage_test_read(positive mode, positive wanted)
{
        p8 guarded[34];
        memory_fill(guarded, 0xa5, sizeof guarded);
        storage_test_begin(mode, 32, 123, guarded + 1);
        positive got = storage_read(12345, guarded + 1, 32, 123);
        storage_test_mode = 0;

        check("positional read count", got == wanted);
        check("positional short reads advance pointer and offset",
              storage_test_arguments);
        check("destination guards", guarded[0] == 0xa5 && guarded[33] == 0xa5);
        check("full probe is not cleared before read", !storage_test_preclear);
        for (positive i = 0; i < 32; i++)
                check("read bytes survive and unread tail is zero",
                      guarded[i + 1] == (i < got ? (p8)(i + 1) : 0));
}

static fn storage_test_exact(void)
{
        for (positive write = 0; write < 2; write++)
                for (positive mode = 1; mode <= 4; mode++)
                        for (positive length = 0; length <= 32; length += 32)
                        {
                                p8 guarded[34];
                                memory_fill(guarded, 0xa5, sizeof guarded);
                                positive offset = ((positive)1 << 32) + 123;
                                storage_test_begin(mode, length, offset, guarded + 1);
                                storage_test_number = write ? syscall(pwrite64) : syscall(pread64);
                                bipolar got = write
                                    ? storage_write(12345, guarded + 1, length, offset)
                                    : file_transfer_exact(syscall(pread64), 12345,
                                                          guarded + 1, length, offset);
                                storage_test_mode = 0;
                                check("exact positional return", got ==
                                      (!length || mode == 1 ? (bipolar)length : -5));
                                check("exact positional offset/pointer/length", storage_test_arguments);
                                check("exact positional zero does not call kernel", length || !storage_test_calls);
                                check("exact positional guards", guarded[0] == 0xa5 && guarded[33] == 0xa5);
                                for (positive i = 0; i < 32; i++)
                                        check("exact positional bytes are not cleared", guarded[i + 1] ==
                                              (!write && i < storage_test_used ? (p8)(i + 1) : 0xa5));
                        }
}

static fn storage_test_replay(void)
{
        bipolar handle = system_call_2(syscall(memfd_create),
                                        (positive)"replay-edge-test", 0);
        check("replay fixture descriptor", handle >= 0);
        if (handle < 0)
                return;
        static p8 bytes[66564];
        static const positive headers[] = {18, 4094, 4095, 4096, 65534, 65535, 65536};
        static const positive widths[] = {0, 1, 1022, 1023, 1024};
        for (positive h = 0; h < array_count(headers); h++)
                for (positive w = 0; w < array_count(widths); w++)
                        for (positive ending = 0; ending < 3; ending++)
                        {
                                positive header = headers[h], width = widths[w];
                                memory_fill(bytes, 'h', header);
                                memory_copy(bytes, "Script started on ", 18);
                                bytes[header] = '\n';
                                memory_fill(bytes + header + 1, 'q', width);
                                positive size = header + 1 + width;
                                if (ending == 2) bytes[size++] = '\r';
                                if (ending) bytes[size++] = '\n';
                                bool prepared = system_seek(handle, 0, FILE_SEEK_SET) == 0 &&
                                    system_truncate_handle(handle, size) == 0 &&
                                    system_write_all(handle, bytes, size) == size &&
                                    system_seek(handle, 0, FILE_SEEK_SET) == 0;
                                check("replay fixture prepared", prepared);
                                if (!prepared) continue;
                                process_replay_reader reader = {.handle = handle};
                                bool accepted = process_replay_skip_header(address_of reader);
                                check("replay header bound", accepted == (header < 65536));
                                if (!accepted) continue;
                                p8 line[1026];
                                memory_fill(line, 0xa5, sizeof(line));
                                bipolar got = process_replay_line(address_of reader, line + 1, 1024);
                                bool fits = width + (ending == 2) < 1024;
                                bipolar expected = fits ? (width || ending ? (bipolar)width + 1 : 0) : -1;
                                check("replay line bound/CRLF/EOF", got == expected && reader.failed == !fits);
                                check("replay line output guards", line[0] == 0xa5 && line[1025] == 0xa5);
                                if (got > 0)
                                        check("replay line bytes and terminator", line[width + 1] == 0 &&
                                              memory_span_byte(line + 1, 'q', width) == width);
                        }
        system_close(handle);
}

static fn utility_test_decimal(void)
{
        static const string_address cases[] = {"", "+1", "-1", " 1", "0", "7", "0042", "42tail",
            "18446744073709551614", "18446744073709551615", "18446744073709551616",
            "18446744073709551616tail", "9999999999999999999999999999999999999999999999"};
        for (positive i = 0; i < array_count(cases); i++)
                for (positive saturate = 0; saturate < 2; saturate++)
                {
                        string_address text = cases[i], at = text;
                        positive expected = 0, digits = 0;
                        bool overflow = false;
                        while (text[digits] >= '0' && text[digits] <= '9')
                        {
                                positive digit = text[digits++] - '0';
                                if (expected > (positive_max - digit) / 10)
                                        overflow = true;
                                expected = overflow ? positive_max : expected * 10 + digit;
                        }
                        positive value = 123;
                        bool good = file_decimal_read(address_of at, saturate, address_of value);
                        bool want = digits && (saturate || !overflow);
                        check("decimal checked/saturated answer", good == want);
                        check("decimal cursor and output rollback", want
                              ? at == text + digits && value == expected
                              : at == text && value == 123);
                }
}

static fn storage_test_elf(void)
{
        bipolar handle = system_call_2(syscall(memfd_create),
                                        (positive)"storage-elf-test", 0);
        check("ELF fixture descriptor", handle >= 0);
        if (handle < 0)
                return;

        p8 head[64] = {0x7f, 'E', 'L', 'F', 2, 1};
        head[16] = 2;
        positive machine = stdbuf_elf_machine();
        head[18] = (p8)machine;
        head[19] = (p8)(machine >> 8);
        head[33] = 32; /* e_phoff = 8192, beyond the initial read. */
        head[54] = 56;
        head[56] = 1;
        check("ELF fixture written", system_write_all((positive)handle,
              head, sizeof head) == sizeof head);

        p8 path[64];
        memory_copy_apart(path, "/proc/self/fd/", 14);
        positive_into_string(path + 14, (positive)handle);
        storage_test_begin(5, 56, 8192, null);
        b32 kind = stdbuf_target_kind(path);
        storage_test_mode = 0;
        system_close((positive)handle);
        check("stdbuf retries interrupted and short ELF table reads",
              kind == STDBUF_ELF_DYNAMIC && storage_test_used == 56 &&
              storage_test_calls == 9 && storage_test_arguments);
}

static fn storage_test_copy(void)
{
        bipolar in = system_call_2(syscall(memfd_create), (positive)"copy-in", 0);
        bipolar out = system_call_2(syscall(memfd_create), (positive)"copy-out", 0);
        check("copy fixture descriptors", in >= 0 && out >= 0);
        if (in < 0 || out < 0)
                goto done;

        p8 bytes[257], copied[257];
        for (positive i = 0; i < sizeof(bytes); i++)
                bytes[i] = (p8)(i * 197 + 1);
        check("copy fixture offset", system_seek(in, 8192, FILE_SEEK_SET) == 8192);
        check("copy fixture data", system_write_all(in, bytes, sizeof(bytes)) == sizeof(bytes));

        // Start at unrelated descriptor positions. Each capability setting
        // must copy the explicit extent, preserving the hole before it.
        for (positive mode = 0; mode < 3; mode++)
        {
                bool range = mode == 0, send = mode < 2;
                check("copy output reset", system_truncate_handle(out, 0) == 0);
                check("copy input position", system_seek(in, 5, FILE_SEEK_SET) == 5);
                check("copy output position", system_seek(out, 7, FILE_SEEK_SET) == 7);
                check("extent through range/sendfile/buffer", file_copy_extent(
                    in, out, 8192, sizeof(bytes), address_of range, address_of send));
                check("copied extent bytes", system_call_4(syscall(pread64), out,
                    (positive)copied, sizeof(copied), 8192) == sizeof(copied) &&
                    !memory_compare(bytes, copied, sizeof(bytes)));
                check("copy preserves preceding hole", system_call_4(
                    syscall(pread64), out, (positive)copied, 1, 0) == 1 && !copied[0]);
                check("bounded copy refuses premature EOF", !file_copy_extent(
                    in, out, 8192 + sizeof(bytes), 1, address_of range, address_of send));
        }
done:
        if (in >= 0) system_close(in);
        if (out >= 0) system_close(out);
}

static fn storage_test_lsfd(void)
{
        string_address words[] = {"lsfd", "-p", "4294967295", "-n", "-o", "PID", null};
        program_arguments_use(words, 6);
        check("lsfd empty selection", util_linux_lsfd() == 0);
        check("lsfd does not retain unrelated PID records",
              !ul_lsfd_snapshot.header.process_count);
        program_arguments_own();
        p8 resident;
        check("lsfd keeps borrowed arena mapped", text_arena &&
              system_call_3(syscall(mincore), (positive)text_arena, 1,
                            (positive)address_of resident) == 0);

        static const string_address samples[] = {
            "pos:\t123\nflags:\t0100002\nmnt_id:\t77\n",
            "other: ignored\nmnt_id:77\nflags:0100002\npos:123",
            "pos:123\npos:999\nflags:0100002\nflags:0\nmnt_id:77\n",
        };
        for (positive i = 0; i < array_count(samples); i++)
        {
                p8 bytes[128];
                positive length = string_length(samples[i]);
                memory_copy_apart_end(bytes, samples[i], length);
                bytes[length] = end;
                ul_lsfd_entry entry = {0};
                ul_lsfd_fdinfo_parse(address_of entry, bytes, length);
                check("fdinfo first fields and record boundaries",
                      entry.access_known && entry.access == 2 &&
                      entry.position_known && entry.position == 123 &&
                      entry.mount_known && entry.mount_id == 77);
        }
        p8 invalid[] = "pos:123\0hidden\npos:12\nflags:8\nflags:0\nmnt_id:3 \nmnt_id:9\n";
        ul_lsfd_entry entry = {.mount_id = 41, .mount_known = true};
        ul_lsfd_fdinfo_parse(address_of entry, invalid, sizeof(invalid) - 1);
        check("fdinfo malformed first occurrence is final",
              !entry.access_known && !entry.position_known &&
              entry.mount_known && entry.mount_id == 41);
}

static fn storage_test_consumed_mounts(void)
{
        storage_mount entries[] = {
            {.source = "gone", .target = null},
            {.source = "live", .target = "/live"},
        };
        storage_mount_table table = {.entry = entries, .count = array_count(entries)};
        string_address missing = storage_umount_target(address_of table, "gone");
        check("unmounted source cannot produce a null target",
              missing && string_equals(missing, "gone"));
        check("later target lookup skips consumed mount entries",
              string_equals(storage_umount_target(address_of table, "/live"), "/live"));
}

static fn storage_test_link_state(void)
{
        net_holding held = {0};
        check("new down interface can be configured", net_link_news(11, 0, address_of held));
        check("initial carrier can be configured", net_link_news(11, IFF_RUNNING, address_of held));
        held.index = 11;
        check("unchanged carrier keeps lease", !net_link_news(11, IFF_RUNNING, address_of held));
        check("second live interface keeps lease", !net_link_news(12, IFF_RUNNING, address_of held));
        check("carrier loss invalidates the sole configured state",
              net_link_news(11, 0, address_of held) && !held.index);
        check("failed reconfiguration cannot revive lost interface",
              net_auto(-1, address_of held) != 0 && !held.index);
        check("new interface can retry after configuration failure",
              net_link_news(13, 0, address_of held));
        netlink_forget(address_of net_states);
        net_state_count = 0;
}

b32 main(void)
{
        storage_test_read(1, 32);
        storage_test_read(2, 7);
        storage_test_read(3, 0);
        storage_test_read(4, 7);
        storage_test_exact();
        storage_test_replay();
        utility_test_decimal();
        storage_test_elf();
        storage_test_copy();
        storage_test_lsfd();
        storage_test_consumed_mounts();
        storage_test_link_state();
        return test_report(null);
}
