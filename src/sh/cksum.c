/*
        POSIX cksum.

        The CRC is deliberately separate from the digest engine the named
        sums share: a four-byte serial checksum wants its own folds, not a
        block core's buffering. Eight dynamically prepared 1 KiB slices keep
        the inner step compact; four independent spans let an out-of-order
        core overlap their table lookups, then the same polynomial combines
        them. Short blocks retain the one-span floor. No large constant object
        is stored in the multicall image, and input/output remain the shell's
        shared blocks and writers.
*/

#define CKSUM_POLYNOMIAL 0x04c11db7u
#define CKSUM_SLICES 8

static p32 cksum_crc_table[CKSUM_SLICES][256];
static p32 cksum_crc_shift_power[positive_bits];
static bool cksum_crc_table_ready;
#if X64
/* 0 is unprobed, 1 is unavailable, 2 is PCLMUL, 3 is VPCLMUL. A shell should
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

static p8 cksum_crc_hardware()
{
        p32 leaf = 1;
        p32 ebx;
        p32 features = 0;
        p32 edx;

        __asm__ volatile("cpuid"
                         : "+a"(leaf), "=b"(ebx), "+c"(features), "=d"(edx));
        (void)ebx;
        (void)edx;
        if (!(features & ((p32)1 << 1)) || !(features & ((p32)1 << 9)))
                return 1;
        if (cpu_has_avx512)
        {
                leaf = 7;
                features = 0;
                __asm__ volatile("cpuid"
                                 : "+a"(leaf), "=b"(ebx), "+c"(features),
                                   "=d"(edx));
                if (features & ((p32)1 << 10))
                        return 3;
        }
        return 2;
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

/* The same four polynomial chains as the SSE path, packed into one ZMM.
   Only the lane width changes; final reduction stays in the shared floor.
   Dispatch requires both OS-enabled AVX-512 and the separate VPCLMUL bit. */
typedef p64 cksum_crc_wide
    __attribute__((vector_size(64), aligned(1), may_alias));
typedef long long cksum_crc_wide_signed __attribute__((vector_size(64)));
typedef char cksum_crc_wide_bytes __attribute__((vector_size(64)));

static __attribute__((target("avx512f,avx512bw,vpclmulqdq,pclmul,ssse3"))) p32
cksum_crc_vpclmul(p8 address_to bytes, positive length, p32 crc)
{
        const cksum_crc_wide four = {
            0xe6228b11ull, 0x8833794cull, 0xe6228b11ull, 0x8833794cull,
            0xe6228b11ull, 0x8833794cull, 0xe6228b11ull, 0x8833794cull};
        const cksum_crc_wide mask = {
            0x08090a0b0c0d0e0full, 0x0001020304050607ull,
            0x08090a0b0c0d0e0full, 0x0001020304050607ull,
            0x08090a0b0c0d0e0full, 0x0001020304050607ull,
            0x08090a0b0c0d0e0full, 0x0001020304050607ull};
        cksum_crc_wide value = {0, (p64)crc << 32, 0, 0, 0, 0, 0, 0};
        bool first = true;

        do
        {
                cksum_crc_wide next = (cksum_crc_wide)
                    __builtin_ia32_pshufb512_mask(
                        (cksum_crc_wide_bytes)*(cksum_crc_wide address_to)bytes,
                        (cksum_crc_wide_bytes)mask,
                        (cksum_crc_wide_bytes){0}, (p64)-1);
                if (!first)
                {
                        cksum_crc_wide low = (cksum_crc_wide)
                            __builtin_ia32_vpclmulqdq_v8di(
                                (cksum_crc_wide_signed)value,
                                (cksum_crc_wide_signed)four, 0x00);
                        cksum_crc_wide high = (cksum_crc_wide)
                            __builtin_ia32_vpclmulqdq_v8di(
                                (cksum_crc_wide_signed)value,
                                (cksum_crc_wide_signed)four, 0x11);
                        value = low ^ high;
                }
                value ^= next;
                first = false;
                bytes += 64;
                length -= 64;
        } while (length >= 64);

        const cksum_crc_vector one = {0xe8a45605ull, 0xc5b9cd4cull};
        cksum_crc_vector folded = {value[0], value[1]};
        folded = cksum_crc_fold(folded, one,
                                (cksum_crc_vector){value[2], value[3]});
        folded = cksum_crc_fold(folded, one,
                                (cksum_crc_vector){value[4], value[5]});
        folded = cksum_crc_fold(folded, one,
                                (cksum_crc_vector){value[6], value[7]});
        folded = cksum_crc_reverse(folded);
        crc = cksum_crc_serial((p8 address_to)address_of folded, 16, 0);
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
        if (length >= 128 && cksum_crc_pclmul_state == 3)
                return cksum_crc_vpclmul(bytes, length, crc);
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
        bool standard;
        bipolar input = checksum_open(path, address_of standard);

        if (input < 0)
                return string_diagnostic(address_of text_diagnostic, 0, path, file_reason(input));

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
                return string_diagnostic(address_of text_diagnostic, 0, path, file_reason(got));

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

        text_put_character(checksum_zero ? '\0' : '\n');
}

/*
        -a crc32b, bsd and sysv: the other three sums that are not digests.

        crc32b is the reflected IEEE CRC gzip uses, hash_crc32, without the
        length the POSIX CRC folds in. bsd is sum -r's rotating 16-bit sum
        and sysv is sum -s's byte total folded to 16 bits, over 1024- and
        512-byte blocks; text.c's sum computes both the same way.
*/
static bool cksum_other_path(p8 kind, string_address path, bool debug,
                             p32 address_to result, p64 address_to size)
{
        bool standard;
        bipolar input = checksum_open(path, address_of standard);

        if (input < 0)
                return string_diagnostic(address_of text_diagnostic, 0, path, file_reason(input));

        // coreutils names the crc32b machinery for every input it reads.
        if (debug && kind == 'c')
        {
                text_flush();
                string_format(writer_stderr, "cksum: using %s hardware support\n",
#if !defined(KERNEL_MODE) && (X64 || ARM64 || RISCV64)
                              cpu_has_pclmul ? (string_address) "pclmul" :
#endif
                              (string_address) "generic");
        }

        p32 sum = kind == 'c' ? ~(p32)0 : 0;
        p64 bytes = 0;
        bipolar got;

        while ((got = system_read_retry((positive)input, file_transfer,
                                        FILE_TRANSFER_SIZE)) > 0)
        {
                if (kind == 'c')
                        sum = hash_crc32(sum, file_transfer, (positive)got);
                else if (kind == 'b')
                        sum = memory_checksum_bsd16(file_transfer, (positive)got, sum);
                else
                        sum += (p32)memory_sum_bytes(file_transfer, (positive)got);
                bytes += (positive)got;
        }

        if (!standard)
                system_close((positive)input);

        if (got < 0)
                return string_diagnostic(address_of text_diagnostic, 0, path, file_reason(got));

        if (kind == 'c')
                sum = ~sum;
        else if (kind == 's')
        {
                p32 folded = (sum & 0xffff) + (sum >> 16);

                sum = (folded & 0xffff) + (folded >> 16);
        }

        address_to result = sum;
        address_to size = bytes;
        return true;
}

static fn cksum_other_put(p8 kind, p32 sum, p64 bytes, string_address name,
                          bool named)
{
        // --raw is the sum in network order: four bytes of CRC, two of the rest.
        if (checksum_raw)
        {
                p8 wire[4];

                if (kind == 'c')
                {
                        network_store_32(wire, sum);
                        text_put(wire, 4);
                }
                else
                {
                        network_store_16(wire, (p16)sum);
                        text_put(wire, 2);
                }
                return;
        }

        if (kind == 'c')
        {
                cksum_crc_put(sum, bytes, name, named);
                return;
        }

        positive block = kind == 's' ? 512 : 1024;
        p64 blocks = bytes / block + (bytes % block != 0);

        if (kind == 's')
        {
                positive_to_string(text_put, sum);
                text_put_character(' ');
                positive_to_string(text_put, (positive)blocks);
        }
        else
        {
                positive_to_padded(text_put, sum, 5, '0', 0);
                text_put_character(' ');
                positive_to_padded(text_put, (positive)blocks, 5, ' ', 0);
        }

        if (named)
        {
                text_put_character(' ');
                text_put_string(name);
        }

        text_put_character(checksum_zero ? '\0' : '\n');
}

static b32 cksum_others(p8 kind, bool debug)
{
        bool named = text_files_count != 0;
        b32 inputs = text_input_count();
        b32 answer = 0;

        for (b32 i = 0; i < inputs; i++)
        {
                string_address name = text_file_name(i);
                p32 sum;
                p64 bytes;

                if (!cksum_other_path(kind, name ? name : (string_address) "-",
                                      debug, address_of sum, address_of bytes))
                {
                        answer = 1;
                        continue;
                }

                cksum_other_put(kind, sum, bytes, name ? name : (string_address) "-", named);
        }

        return text_done(answer);
}

/* -a sha2 -l N: the SHA-2 digest N bits wide, or null for any other N. */
static const checksum_algorithm address_to cksum_sha2_width(positive bits)
{
        for (positive which = CHECKSUM_SHA2_FIRST; which <= CHECKSUM_SHA2_LAST; which++)
                if ((positive)checksum_algorithms[which].bytes * 8 == bits)
                        return checksum_algorithms + which;
        return null;
}

static const argument_option cksum_options[] = {
    {"algorithm", 'a', ARGUMENT_REQUIRED},
    {"untagged", 'U', 0, 1},
    {"tag", 'T', 0, 1},
    {"raw", 'R'},
    {"base64", 'B'},
    {"zero", 'z'},
    {"length", 'l', ARGUMENT_REQUIRED},
    {"check", 'c'},
    {"ignore-missing", 'i'},
    {"quiet", 'q', 0, ARGUMENT_SELECT(checksum_selection, verify)},
    {"status", 's', 0, ARGUMENT_SELECT(checksum_selection, verify)},
    {"strict", 'S'},
    {"warn", 'w', 0, ARGUMENT_SELECT(checksum_selection, verify)},
    {"debug", 'D'},
    {null},
};

/* Each algorithm is checked as it is read, so an unknown one is refused
   even when a later --algorithm would supersede it. */
static bool cksum_option_seen(p8 letter, string_address value)
{
        static const string_address known[] = {
            "bsd", "sysv", "crc", "crc32b", "md5", "sha1", "sha224", "sha256",
            "sha384", "sha512", "sha2", "sha3", "blake2b", "sm3",
        };

        if (letter != 'a' || !value)
                return true;

        for (positive at = 0; at < array_count(known); at++)
                if (string_equals(value, known[at]))
                        return true;

        text_flush();
        string_format(writer_stderr,
            "cksum: invalid argument '%s' for '--algorithm'\nValid arguments are:\n",
            value);
        for (positive at = 0; at < array_count(known); at++)
                string_format(writer_stderr, "  - '%s'\n", known[at]);
        return string_report(writer_stderr, false, "Try 'cksum --help' for more information.\n");
}

static b32 cksum_main()
{
        file_taking taking = {
            .program = (string_address) "cksum",
            .options = cksum_options,
            .operand = text_file_add,
            .seen = cksum_option_seen,
            .selection = (p8 address_to)&checksum_selected,
        };


        text_begin("cksum");
        checksum_modes_reset();

        if (!file_take(address_of taking) ||
            (text_files_failed && string_diagnostic(
                address_of text_diagnostic, 1, null, "too many operands")))
                return text_done(1);

        string_address algorithm = file_option_value(address_of taking, 'a');
        string_address length = file_option_value(address_of taking, 'l');
        bool raw = (taking.flags & FILE_FLAG('R')) != 0;
        bool checking = (taking.flags & FILE_FLAG('c')) != 0;
        bool tagged = checksum_selected.style == 'T';
        bool debug = (taking.flags & FILE_FLAG('D')) != 0;
        bool lengthed = (taking.flags & FILE_FLAG('l')) && length;
        bool sha2 = algorithm && string_equals(algorithm, "sha2");
        bool sha3 = algorithm && string_equals(algorithm, "sha3");
        bool blake2b = algorithm && string_equals(algorithm, "blake2b");
        positive bits = 0;

        checksum_zero = (taking.flags & FILE_FLAG('z')) != 0;

        /*
                The reference's own refusals, in the order it makes them. A
                length that is no number is refused as it is read. A length of
                zero is no length at all -- it asks for the algorithm's own
                width -- so it does not reach the second.
        */
        if (lengthed && !checksum_decimal(length, address_of bits))
        {
                text_flush();
                return text_done(string_report(writer_stderr, 1,
                    "cksum: invalid length: '%s'\n", length));
        }
        if (lengthed && bits && !blake2b && !sha2 && !sha3)
        {
                text_flush();
                return text_done(string_report(writer_stderr, 1,
                    "cksum: --length is only supported with --algorithm blake2b, sha2, or sha3\n"));
        }
        if (lengthed && blake2b)
        {
                b32 refused = checksum_blake2b_length((string_address) "cksum",
                                                      length, address_of checksum_length);
                if (refused)
                        return refused;
        }
        if (lengthed && (sha2 || sha3) && !cksum_sha2_width(bits))
        {
                text_flush();
                return text_done(string_report(writer_stderr, 1,
                    "cksum: invalid length: '%s'\n"
                    "cksum: digest length for '%s' must be 224, 256, 384, or 512\n",
                    length, sha2 ? (string_address) "SHA2" : (string_address) "SHA3"));
        }
        // The SHA-2 and SHA-3 families need their width said, before any
        // complaint about the other mode's options.
        if ((sha2 || sha3) && !lengthed && !checking)
        {
                text_flush();
                return text_done(string_report(writer_stderr, 1,
                    "cksum: --algorithm=%s requires specifying --length 224, 256, 384, or 512\n",
                    algorithm));
        }
        if (checking && algorithm &&
            (string_equals(algorithm, "bsd") || string_equals(algorithm, "sysv") ||
             string_equals(algorithm, "crc") || string_equals(algorithm, "crc32b")))
        {
                text_flush();
                return text_done(string_report(writer_stderr, 1,
                    "cksum: --check is not supported with --algorithm={bsd,sysv,crc,crc32b}\n"));
        }
        if (raw && (taking.flags & FILE_FLAG('B')))
                return checksum_usage_error("cksum", "--base64 and --raw are mutually exclusive");
        b32 refused = checksum_refuse_modes("cksum", checking, tagged, taking.flags);
        if (refused)
                return refused;

        if (raw && text_files_count > 1)
                return text_done(string_diagnostic(address_of text_diagnostic, 1, null, "the --raw option is not supported with multiple files"));

        checksum_raw = raw;
        checksum_base64 = (taking.flags & FILE_FLAG('B')) != 0;

        // The digest an --algorithm names; SHA-2 by the width -l gave it.
        const checksum_algorithm address_to digest = null;

        if (sha2)
                digest = bits ? cksum_sha2_width(bits)
                              : checksum_algorithms + CHECKSUM_SHA2_FIRST + 1;
        else if (algorithm && !sha3)
                digest = checksum_algorithm_find(algorithm, true);

        if (checking)
        {
                /*
                        Without --algorithm the reference reads whichever
                        algorithm each tagged line names; with one, that
                        algorithm reads every line, tagged or not. sha2
                        without a width reads each record at the width its
                        digits have.
                */
                if (algorithm && !digest)
                        return text_done(string_diagnostic(address_of text_diagnostic, 1, algorithm, "algorithm is not supported by the available checksum engine"));

                checksum_sha2_family = sha2 && !bits;
                checksum_manifest_files = true;
                checksum_program = (string_address) "cksum";
                checksum_check_label = sha2 ? (string_address) "SHA2"
                                       : digest ? digest->label
                                                : (string_address) "CRC";
                return text_done(checksum_verify(digest, address_of taking));
        }

        if (algorithm && !string_equals(algorithm, "crc"))
        {
                if (string_equals(algorithm, "crc32b"))
                        return cksum_others('c', debug);
                if (string_equals(algorithm, "bsd"))
                        return cksum_others('b', debug);
                if (string_equals(algorithm, "sysv"))
                        return cksum_others('s', debug);

                if (digest)
                        return text_done(checksum_generate(digest, 0, checksum_selected.style != 'U', true));

                return text_done(string_diagnostic(address_of text_diagnostic, 1, algorithm, "algorithm is not supported by the available checksum engine"));
        }

        cksum_crc_prepare();
#if X64
        if (!cksum_crc_pclmul_state)
                cksum_crc_pclmul_state = cksum_crc_hardware();
#endif

        /* --debug names the machinery the CRC is computed with, once. */
        if (debug)
        {
                string_address machinery = (string_address) "generic";
#if X64
                if (cksum_crc_pclmul_state == 3)
                        machinery = (string_address) "avx512";
                else if (cksum_crc_pclmul_state == 2)
                        machinery = (string_address) "pclmul";
#endif
                text_flush();
                string_format(writer_stderr, "cksum: using %s hardware support\n",
                              machinery);
        }

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

                // --raw is the CRC's four bytes, most significant first.
                if (raw)
                {
                        p8 wire[4];

                        network_store_32(wire, crc);
                        text_put(wire, 4);
                }
                else
                        cksum_crc_put(crc, bytes, name, named);
        }

        return text_done(answer);
}
