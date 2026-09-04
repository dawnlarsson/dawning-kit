/*
        POSIX cksum.

        This is deliberately separate from the optional AF_ALG digest
        adapter: the default CRC must work on every image, and routing a
        four-byte serial checksum through a crypto socket would add overhead
        rather than remove it. Eight dynamically prepared 1 KiB slices keep
        the inner step compact; four independent spans let an out-of-order
        core overlap their table lookups, then the same polynomial combines
        them. Short blocks retain the one-span floor. No large constant object
        is stored in the multicall image, and input/output remain the shell's
        shared blocks and writers.
*/

#define CKSUM_POLYNOMIAL 0x04c11db7u
#define CKSUM_ERROR_INTERRUPTED (-4)
#define CKSUM_SLICES 8

static p32 cksum_crc_table[CKSUM_SLICES][256];
static p32 cksum_crc_shift_power[positive_bits];
static bool cksum_crc_table_ready;
#if X64
/* 0 is unprobed, 1 is unavailable, 2 is ready. A long-lived shell should
   not serialize itself with CPUID again on every cksum call on an older CPU. */
static p8 cksum_crc_pclmul_state;
#endif

/* Multiplication in the same GF(2) field as the byte recurrence. It is cold:
   table preparation uses it once, and a full shared input block needs only
   three combines. */
static p32 cksum_crc_multiply(p32 left, p32 right)
{
        p32 result = 0;

        for (positive bit = 0; bit < 32; bit++)
        {
                if (right & 1)
                        result ^= left;

                right >>= 1;
                left = (left << 1) ^
                       ((left & 0x80000000u) ? CKSUM_POLYNOMIAL : 0);
        }

        return result;
}

static fn cksum_crc_prepare()
{
        if (cksum_crc_table_ready)
                return;

        for (positive value = 0; value < 256; value++)
        {
                p32 crc = (p32)value << 24;

                for (positive bit = 0; bit < 8; bit++)
                        crc = (crc << 1) ^
                              ((crc & 0x80000000u) ? CKSUM_POLYNOMIAL : 0);

                cksum_crc_table[0][value] = crc;
        }

        for (positive slice = 1; slice < CKSUM_SLICES; slice++)
                for (positive value = 0; value < 256; value++)
                {
                        p32 crc = cksum_crc_table[slice - 1][value];

                        cksum_crc_table[slice][value] =
                            (crc << 8) ^ cksum_crc_table[0][crc >> 24];
                }

        cksum_crc_shift_power[0] = 0x100;
        for (positive bit = 1; bit < positive_bits; bit++)
                cksum_crc_shift_power[bit] = cksum_crc_multiply(
                    cksum_crc_shift_power[bit - 1],
                    cksum_crc_shift_power[bit - 1]);

        cksum_crc_table_ready = true;
}

static inline INLINE p32 cksum_crc_word(p64 word, p32 crc)
{
        return cksum_crc_table[7][((crc >> 24) ^ word) & 255] ^
               cksum_crc_table[6][((crc >> 16) ^ (word >> 8)) & 255] ^
               cksum_crc_table[5][((crc >> 8) ^ (word >> 16)) & 255] ^
               cksum_crc_table[4][(crc ^ (word >> 24)) & 255] ^
               cksum_crc_table[3][(word >> 32) & 255] ^
               cksum_crc_table[2][(word >> 40) & 255] ^
               cksum_crc_table[1][(word >> 48) & 255] ^
               cksum_crc_table[0][word >> 56];
}

static p32 cksum_crc_serial(p8 address_to bytes, positive length, p32 crc)
{
        while (length >= 8)
        {
                p64 word = memory_load_unaligned(p64, bytes);

                crc = cksum_crc_word(word, crc);
                bytes += 8;
                length -= 8;
        }

        while (length--)
                crc = (crc << 8) ^
                      cksum_crc_table[0][(crc >> 24) ^ *bytes++];

        return crc;
}

#if X64
/* SSE's carry-less multiply evaluates the same polynomial 128 bytes at a
   time. The constants are x^128 and x^512 reduced modulo 0x104c11db7, from
   Intel's generic-polynomial CRC construction. SSSE3 reverses each vector
   because POSIX CRC is the non-reflected, most-significant-bit-first form.
   The table floor remains authoritative for the final folded vector and for
   every processor without both instructions. */
typedef p64 cksum_crc_vector
    __attribute__((vector_size(16), aligned(1), may_alias));
typedef long long cksum_crc_vector_signed __attribute__((vector_size(16)));
typedef char cksum_crc_bytes_signed __attribute__((vector_size(16)));

static bool cksum_crc_has_pclmul()
{
        p32 leaf = 1;
        p32 ebx;
        p32 features = 0;
        p32 edx;

        __asm__ volatile("cpuid"
                         : "+a"(leaf), "=b"(ebx), "+c"(features), "=d"(edx));
        (void)ebx;
        (void)edx;
        return (features & ((p32)1 << 1)) &&
               (features & ((p32)1 << 9));
}

static __attribute__((target("pclmul,ssse3"))) cksum_crc_vector
cksum_crc_reverse(cksum_crc_vector value)
{
        const cksum_crc_vector mask = {
            0x08090a0b0c0d0e0full, 0x0001020304050607ull};

        return (cksum_crc_vector)__builtin_ia32_pshufb128(
            (cksum_crc_bytes_signed)value,
            (cksum_crc_bytes_signed)mask);
}

static __attribute__((target("pclmul,ssse3"))) cksum_crc_vector
cksum_crc_fold(cksum_crc_vector value, cksum_crc_vector constant,
               cksum_crc_vector following)
{
        cksum_crc_vector low =
            (cksum_crc_vector)__builtin_ia32_pclmulqdq128(
                (cksum_crc_vector_signed)value,
                (cksum_crc_vector_signed)constant, 0x00);
        cksum_crc_vector high =
            (cksum_crc_vector)__builtin_ia32_pclmulqdq128(
                (cksum_crc_vector_signed)value,
                (cksum_crc_vector_signed)constant, 0x11);

        return low ^ high ^ following;
}

static __attribute__((target("pclmul,ssse3"))) p32
cksum_crc_pclmul(p8 address_to bytes, positive length, p32 crc)
{
        const cksum_crc_vector four = {0xe6228b11ull, 0x8833794cull};
        const cksum_crc_vector one = {0xe8a45605ull, 0xc5b9cd4cull};
        cksum_crc_vector first = cksum_crc_reverse(
            *(cksum_crc_vector address_to)(bytes));
        cksum_crc_vector second = cksum_crc_reverse(
            *(cksum_crc_vector address_to)(bytes + 16));
        cksum_crc_vector third = cksum_crc_reverse(
            *(cksum_crc_vector address_to)(bytes + 32));
        cksum_crc_vector fourth = cksum_crc_reverse(
            *(cksum_crc_vector address_to)(bytes + 48));
        const cksum_crc_vector initial = {0, (p64)crc << 32};

        first ^= initial;
        bytes += 64;
        length -= 64;

        while (length >= 64)
        {
                first = cksum_crc_fold(
                    first, four,
                    cksum_crc_reverse(
                        *(cksum_crc_vector address_to)(bytes)));
                second = cksum_crc_fold(
                    second, four,
                    cksum_crc_reverse(
                        *(cksum_crc_vector address_to)(bytes + 16)));
                third = cksum_crc_fold(
                    third, four,
                    cksum_crc_reverse(
                        *(cksum_crc_vector address_to)(bytes + 32)));
                fourth = cksum_crc_fold(
                    fourth, four,
                    cksum_crc_reverse(
                        *(cksum_crc_vector address_to)(bytes + 48)));
                bytes += 64;
                length -= 64;
        }

        first = cksum_crc_fold(first, one, second);
        first = cksum_crc_fold(first, one, third);
        first = cksum_crc_fold(first, one, fourth);
        first = cksum_crc_reverse(first);
        crc = cksum_crc_serial((p8 address_to)address_of first, 16, 0);
        return cksum_crc_serial(bytes, length, crc);
}
#endif

static p32 cksum_crc_shift(p32 crc, p64 bytes)
{
        positive bit = 0;

        while (bytes)
        {
                if (bytes & 1)
                        crc = cksum_crc_multiply(
                            crc, cksum_crc_shift_power[bit]);

                bytes >>= 1;
                bit++;
        }

        return crc;
}

static HOT __attribute__((noinline)) p32 cksum_crc_block(
    p8 address_to bytes, positive length, p32 crc)
{
#if X64
        if (length >= 128 && cksum_crc_pclmul_state == 2)
                return cksum_crc_pclmul(bytes, length, crc);
#endif

        positive span = (length >> 2) & ~(positive)7;

        if (span < 256)
                return cksum_crc_serial(bytes, length, crc);

        p8 address_to second = bytes + span;
        p8 address_to third = second + span;
        p8 address_to fourth = third + span;
        p32 first_crc = crc;
        p32 second_crc = 0;
        p32 third_crc = 0;
        p32 fourth_crc = 0;

        for (positive at = 0; at < span; at += 8)
        {
                first_crc = cksum_crc_word(
                    memory_load_unaligned(p64, bytes + at), first_crc);
                second_crc = cksum_crc_word(
                    memory_load_unaligned(p64, second + at), second_crc);
                third_crc = cksum_crc_word(
                    memory_load_unaligned(p64, third + at), third_crc);
                fourth_crc = cksum_crc_word(
                    memory_load_unaligned(p64, fourth + at), fourth_crc);
        }

        positive fourth_length = length - span * 3;
        fourth_crc = cksum_crc_serial(fourth + span,
                                      fourth_length - span, fourth_crc);
        crc = cksum_crc_shift(first_crc, span) ^ second_crc;
        crc = cksum_crc_shift(crc, span) ^ third_crc;
        return cksum_crc_shift(crc, fourth_length) ^ fourth_crc;
}

static bool cksum_crc_path(string_address path, p32 address_to result,
                           p64 address_to size)
{
        bool standard = !path ||
                        (string_is(path, '-') && !string_get(path + 1));
        bipolar input = 0;

        if (!standard)
        {
                do
                        input = system_open_at(AT_FDCWD, path,
                                               FILE_READ | O_CLOEXEC);
                while (input == CKSUM_ERROR_INTERRUPTED);

                if (input < 0)
                {
                        text_error(path, file_reason(input));
                        return false;
                }
        }

        p32 crc = 0;
        p64 bytes = 0;
        bipolar got;

        while ((got = system_read_retry((positive)input, file_transfer,
                                        FILE_TRANSFER_SIZE)) > 0)
        {
                crc = cksum_crc_block(file_transfer, (positive)got, crc);
                bytes += (positive)got;
        }

        if (!standard)
                system_close((positive)input);

        if (got < 0)
        {
                text_error(path, file_reason(got));
                return false;
        }

        p64 length = bytes;

        while (length)
        {
                p8 byte = (p8)length;

                crc = (crc << 8) ^
                      cksum_crc_table[0][(crc >> 24) ^ byte];
                length >>= 8;
        }

        address_to result = ~crc;
        address_to size = bytes;
        return true;
}

static fn cksum_crc_put(p32 crc, p64 bytes, string_address name, bool named)
{
        positive_to_string(text_put, crc);
        text_put_character(' ');
        positive_to_string(text_put, (positive)bytes);

        if (named)
        {
                text_put_character(' ');
                text_put_string(name);
        }

        text_put_character('\n');
}

static const file_long cksum_longs[] = {
    {(string_address) "algorithm", 'a'},
    {null, 0},
};

static b32 cksum_main()
{
        file_taking taking = {
            .program = (string_address) "cksum",
            .allowed = (string_address) "a",
            .valued = (string_address) "a",
            .longs = cksum_longs,
            .operand = text_file_add,
        };

        text_begin("cksum");

        if (!file_take(address_of taking) || !text_files_ready())
                return text_done(1);

        string_address algorithm = file_option_value(address_of taking, 'a');

        if (algorithm && !string_equals(algorithm, "crc"))
                return text_refuse(
                    algorithm,
                    "algorithm is not supported; only POSIX crc is available",
                    1);

        cksum_crc_prepare();
#if X64
        if (!cksum_crc_pclmul_state)
                cksum_crc_pclmul_state = cksum_crc_has_pclmul() ? 2 : 1;
#endif

        bool named = text_files_count != 0;
        b32 inputs = text_input_count();
        b32 answer = 0;

        for (b32 i = 0; i < inputs; i++)
        {
                string_address name = text_file_name(i);
                p32 crc;
                p64 bytes;

                if (!cksum_crc_path(name, address_of crc, address_of bytes))
                {
                        answer = 1;
                        continue;
                }

                cksum_crc_put(crc, bytes, name, named);
        }

        return text_done(answer);
}
