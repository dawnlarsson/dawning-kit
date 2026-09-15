/*
        POSIX cksum.

        The CRC is deliberately separate from the digest engine the named
        sums share: a four-byte serial checksum wants its own folds, not a
        block core's buffering. hash_crc32_msb in library.c is all of it --
        a braided table floor and PCLMULQDQ, VPCLMULQDQ or PMULL folds, over
        tables the assembler built -- so nothing is prepared at run time and
        no processor is asked here. This file keeps the POSIX policy: the
        length folded in after the bytes and the final invert. Input and
        output remain the shell's shared blocks and writers.
*/

/* The length after the bytes, least significant byte first, in as few
   bytes as it takes. build.c marks a build directory the same way. */
static p32 cksum_crc_length(p32 crc, p64 length)
{
        p8 counted[8];
        positive places = 0;

        for (; length; length >>= 8)
                counted[places++] = (p8)length;

        return hash_crc32_msb(crc, counted, places);
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
                crc = hash_crc32_msb(crc, file_transfer, (positive)got);
                bytes += (positive)got;
        }

        if (!standard)
                system_close((positive)input);

        if (got < 0)
                return string_diagnostic(address_of text_diagnostic, 0, path, file_reason(got));

        address_to result = ~cksum_crc_length(crc, bytes);
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

        /* --debug names the machinery the CRC is computed with, once. */
        if (debug)
        {
                string_address machinery = (string_address) "generic";
#if X64 && !defined(KERNEL_MODE)
                if (cpu_has_pclmul && cpu_has_vpclmul && cpu_has_avx512)
                        machinery = (string_address) "avx512";
                else if (cpu_has_pclmul)
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
