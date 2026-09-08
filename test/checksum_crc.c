#include "../src/compiler_memory.c"
#include "../src/spark.c"
#include "../src/sh/shell.c"
#include "counted.inc"

static p32 crc_model(p8 address_to bytes, positive length, p32 crc)
{
        while (length--)
        {
                crc ^= (p32)*bytes++ << 24;
                for (positive bit = 0; bit < 8; bit++)
                        crc = (crc << 1) ^
                              (crc >> 31 ? CKSUM_POLYNOMIAL : 0);
        }
        return crc;
}

b32 main(void)
{
        positive page = system_page_size();
        positive room = ((8192 + page - 1) / page) * page;
        p8 address_to bytes = memory(room + page);
        if (!bytes || system_call_3(syscall(mprotect),
                                    (positive)(bytes + room), page, 0) < 0)
                return 1;
        bytes += room - 8192;
        for (positive at = 0; at < 8192; at++)
                bytes[at] = (p8)((at * 73) ^ (at >> 3));
        cksum_crc_prepare();
        positive modes = 1;
#if X64
        modes = cksum_crc_hardware();
#endif
        for (positive mode = 1; mode <= modes; mode++)
        {
#if X64
                cksum_crc_pclmul_state = (p8)mode;
#endif
                for (positive size = 0; size <= 257; size++)
                        for (positive offset = 0; offset < 64; offset++)
                        {
                                p32 seed = (p32)(size * 0x9e3779b9u + offset);
                                p8 address_to at = bytes + offset;
                                check("CRC alignment and arbitrary seed",
                                      cksum_crc_block(at, size, seed) ==
                                          crc_model(at, size, seed));
                        }
                for (positive size = 0; size <= 8192; size += size < 257 ? 1 : 127)
                {
                        p8 address_to at = bytes + 8192 - size;
                        p32 seed = 0xdeadbeef;
                        p32 want = crc_model(at, size, seed);
                        check("CRC protected tail",
                              cksum_crc_block(at, size, seed) == want);
                        positive split = size / 3;
                        p32 first = cksum_crc_block(at, split, seed);
                        check("CRC incremental split",
                              cksum_crc_block(at + split, size - split,
                                               first) == want);
                }
        }
        return test_report(null);
}
