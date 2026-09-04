/*
        The checksum utilities' optional Linux AF_ALG compatibility backend.

        One AF_ALG transform answers every installed name below.  A regular
        file that fits Linux's one-transfer ceiling goes from the page cache
        into the hash socket in one sendfile call; large regular files stay
        kernel-only through splice.  The shared file-transfer block is the
        uncommon fallback for descriptors a transfer primitive cannot move.

        AF_ALG is deprecated upstream and is not enabled by Moonwater's
        default kernel profiles.  This backend remains useful on distro
        kernels that already expose it and in explicitly controlled images;
        the intended native replacement is a narrow Moonwater fd-hash ABI,
        not the generic unprivileged crypto socket surface.

        There is deliberately no second software MD5, SHA or BLAKE2 stack
        here.  Duplicating those cores would make the multicall image larger
        and give this project two implementations to tune and audit.
*/

#if defined(LINUX)

#define CHECKSUM_AF_ALG 38
#define CHECKSUM_SOCK_SEQPACKET 5
#define CHECKSUM_SPLICE_MOVE 1
#define CHECKSUM_SPLICE_MORE 4
#define CHECKSUM_SPLICE_BLOCK (1 << 20)
#define CHECKSUM_MSG_MORE 0x8000
#define CHECKSUM_ERROR_INTERRUPTED (-4)
#define CHECKSUM_ERROR_IO (-5)

typedef struct
{
        p16 family;
        p8 type[14];
        p32 feature;
        p32 mask;
        p8 name[64];
} checksum_socket_address;

_Static_assert(sizeof(checksum_socket_address) == 88,
               "sockaddr_alg is 88 bytes");

typedef struct
{
        string_address command;
        string_address kernel;
        string_address label;
        positive bytes;
        bool variable_length;
} checksum_algorithm;

static const checksum_algorithm checksum_algorithms[] = {
    {(string_address) "b2sum", (string_address) "blake2b-512",
     (string_address) "BLAKE2", 64, true},
    {(string_address) "md5sum", (string_address) "md5",
     (string_address) "MD5", 16, false},
    {(string_address) "sha1sum", (string_address) "sha1",
     (string_address) "SHA1", 20, false},
    {(string_address) "sha224sum", (string_address) "sha224",
     (string_address) "SHA224", 28, false},
    {(string_address) "sha256sum", (string_address) "sha256",
     (string_address) "SHA256", 32, false},
    {(string_address) "sha384sum", (string_address) "sha384",
     (string_address) "SHA384", 48, false},
    {(string_address) "sha512sum", (string_address) "sha512",
     (string_address) "SHA512", 64, false},
};

static const file_long checksum_longs[] = {
    {(string_address) "binary", 'b'},
    {(string_address) "check", 'c'},
    {(string_address) "ignore-missing", 'i'},
    {(string_address) "quiet", 'q'},
    {(string_address) "status", 's'},
    {(string_address) "strict", 'S'},
    {(string_address) "tag", 'T'},
    {(string_address) "text", 't'},
    {(string_address) "warn", 'w'},
    {(string_address) "zero", 'z'},
    {null, 0},
};

static const file_long checksum_b2_longs[] = {
    {(string_address) "binary", 'b'},
    {(string_address) "check", 'c'},
    {(string_address) "ignore-missing", 'i'},
    {(string_address) "length", 'l'},
    {(string_address) "quiet", 'q'},
    {(string_address) "status", 's'},
    {(string_address) "strict", 'S'},
    {(string_address) "tag", 'T'},
    {(string_address) "text", 't'},
    {(string_address) "warn", 'w'},
    {(string_address) "zero", 'z'},
    {null, 0},
};

static bool checksum_binary;

static bool checksum_option_seen(p8 letter, string_address value)
{
        (void)value;

        if (letter == 'b')
                checksum_binary = true;
        else if (letter == 't')
                checksum_binary = false;

        return true;
}

static string_address checksum_called()
{
        string_address called = program_argument(0);
        string_address slash = called ? string_last_of(called, '/') : null;

        return slash ? slash + 1 : called;
}

static const checksum_algorithm address_to checksum_algorithm_find(
    string_address command)
{
        for (positive i = 0; i < array_count(checksum_algorithms); i++)
                if (string_equals(command, checksum_algorithms[i].command))
                        return checksum_algorithms + i;

        return null;
}

static bipolar checksum_kernel_open(const checksum_algorithm address_to algorithm)
{
        checksum_socket_address address;
        bipolar handle;

        memory_fill(address_of address, 0, sizeof(address));
        address.family = CHECKSUM_AF_ALG;
        memory_copy(address.type, "hash", 5);
        memory_copy(address.name, algorithm->kernel,
                    string_length(algorithm->kernel) + 1);

        handle = socket_new(CHECKSUM_AF_ALG,
                            CHECKSUM_SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        if (handle < 0)
                return handle;

        bipolar bound = socket_bind((b32)handle, address_of address,
                                    sizeof(address));

        if (bound < 0)
        {
                system_close((positive)handle);
                return bound;
        }

        return handle;
}

static bipolar checksum_operation_open(bipolar transform)
{
        bipolar answer;

        do
                answer = socket_accept((b32)transform, null, null,
                                       SOCK_CLOEXEC);
        while (answer == CHECKSUM_ERROR_INTERRUPTED);

        return answer;
}

/* All buffered pieces carry MSG_MORE.  Reading the digest is AF_ALG's
   explicit finalisation, so no one-byte sentinel or empty software block is
   inserted into the stream. */
static bipolar checksum_send_more(bipolar operation, address_any bytes,
                                   positive length)
{
        positive sent = 0;

        while (sent < length)
        {
                bipolar wrote = socket_send((b32)operation,
                                             (p8 address_to)bytes + sent,
                                             length - sent, CHECKSUM_MSG_MORE,
                                             null, 0);

                if (wrote == CHECKSUM_ERROR_INTERRUPTED)
                        continue;
                if (wrote <= 0)
                        return wrote ? wrote : CHECKSUM_ERROR_IO;

                sent += (positive)wrote;
        }

        return 0;
}

/* The fallback uses cp's already-resident transfer block and the library's
   EINTR-safe reader.  It exists for procfs, devices and restricted kernels;
   ordinary files never reach it on a kernel with sendfile or splice. */
static bipolar checksum_buffered(bipolar input, bipolar operation)
{
        for (;;)
        {
                bipolar got = system_read_retry((positive)input, file_transfer,
                                                 FILE_TRANSFER_SIZE);

                if (got < 0)
                        return got;
                if (!got)
                        return 0;

                bipolar sent = checksum_send_more(operation, file_transfer,
                                                   (positive)got);
                if (sent < 0)
                        return sent;
        }
}

/* Large regular files remain in the kernel.  At least one side of splice
   must be a pipe, hence the two moves per block. */
static bipolar checksum_splice(bipolar input, bipolar operation)
{
        b32 ends[2];
        bipolar made;

        do
                made = system_call_2(syscall(pipe2), (positive)ends,
                                     O_CLOEXEC);
        while (made == CHECKSUM_ERROR_INTERRUPTED);

        if (made < 0)
                return made;

        bipolar answer = 0;

        for (;;)
        {
                bipolar taken;

                do
                        taken = system_call_6(
                            syscall(splice), (positive)input, 0,
                            (positive)ends[1], 0, CHECKSUM_SPLICE_BLOCK,
                            CHECKSUM_SPLICE_MOVE | CHECKSUM_SPLICE_MORE);
                while (taken == CHECKSUM_ERROR_INTERRUPTED);

                if (taken <= 0)
                {
                        answer = taken;
                        break;
                }

                positive left = (positive)taken;

                while (left)
                {
                        bipolar moved;

                        do
                                moved = system_call_6(
                                    syscall(splice), (positive)ends[0], 0,
                                    (positive)operation, 0, left,
                                    CHECKSUM_SPLICE_MOVE |
                                        CHECKSUM_SPLICE_MORE);
                        while (moved == CHECKSUM_ERROR_INTERRUPTED);

                        if (moved <= 0)
                        {
                                answer = moved ? moved : CHECKSUM_ERROR_IO;
                                goto done;
                        }

                        left -= (positive)moved;
                }
        }

done:
        system_close((positive)ends[0]);
        system_close((positive)ends[1]);
        return answer;
}

static bipolar checksum_digest_read(bipolar operation,
                                     p8 address_to digest,
                                     positive length)
{
        bipolar got = system_read_retry((positive)operation, digest, length);

        if (got == (bipolar)length)
                return 0;

        return got < 0 ? got : CHECKSUM_ERROR_IO;
}

/* One accepted socket is one digest.  A failed fast path is discarded before
   retrying from the descriptor's original position, so a partial send can
   never become the prefix of the fallback digest. */
static bipolar checksum_hash_descriptor(bipolar transform, bipolar input,
                                         p8 address_to digest,
                                         positive digest_length,
                                         bool starts_at_zero)
{
        bipolar operation = checksum_operation_open(transform);

        if (operation < 0)
                return operation;

        file_facts facts;
        bool regular = system_stat_at((b32)input, (string_address) "",
                                      AT_EMPTY_PATH | AT_NO_AUTOMOUNT,
                                      STATX_BASIC, address_of facts) == 0 &&
                       (facts.mode & MODE_FORMAT) == MODE_FILE;
        bipolar start = regular
                            ? starts_at_zero
                                  ? 0
                                  : system_seek(input, 0, FILE_SEEK_CUR)
                            : -1;
        regular = regular && start >= 0;
        bipolar moved = CHECKSUM_ERROR_IO;

        if (regular)
        {
                p64 remaining = facts.size > (p64)start
                                    ? facts.size - (p64)start
                                    : 0;

                if (remaining < FILE_KERNEL_COPY_SIZE)
                {
                        /* Asking for the ceiling, rather than the sampled
                           size, includes a file that grows before this trap.
                           A ceiling-sized answer was not EOF and is retried
                           through the streaming path. */
                        moved = file_send_range_once(
                            input, null, operation, FILE_KERNEL_COPY_SIZE);

                        if (moved >= (bipolar)remaining &&
                            moved < FILE_KERNEL_COPY_SIZE)
                                goto digest;

                        if (moved < 0 &&
                            !file_copy_range_fallback(moved))
                        {
                                system_close((positive)operation);
                                return moved;
                        }
                }

                system_close((positive)operation);
                if (system_seek(input, start, FILE_SEEK_SET) < 0)
                        return CHECKSUM_ERROR_IO;

                operation = checksum_operation_open(transform);
                if (operation < 0)
                        return operation;

                moved = checksum_splice(input, operation);
                if (!moved)
                        goto digest;

                if (!file_copy_range_fallback(moved))
                {
                        system_close((positive)operation);
                        return moved;
                }

                /* splice can be unavailable under seccomp even while AF_ALG
                   is allowed.  Reset both stream and digest before using the
                   existing buffered floor. */
                system_close((positive)operation);
                if (system_seek(input, start, FILE_SEEK_SET) < 0)
                        return moved;

                operation = checksum_operation_open(transform);
                if (operation < 0)
                        return operation;
        }

        moved = checksum_buffered(input, operation);
        if (moved < 0)
        {
                system_close((positive)operation);
                return moved;
        }

digest:
        moved = checksum_digest_read(operation, digest, digest_length);
        system_close((positive)operation);
        return moved;
}

static bipolar checksum_hash_path(bipolar transform, string_address path,
                                   p8 address_to digest,
                                   positive digest_length)
{
        bool standard = !path ||
                        (string_is(path, '-') && !string_get(path + 1));
        bipolar input;

        if (standard)
                input = 0;
        else
        {
                do
                        input = system_open_at(AT_FDCWD, path,
                                               FILE_READ | O_CLOEXEC);
                while (input == CHECKSUM_ERROR_INTERRUPTED);

                if (input < 0)
                        return input;
        }

        bipolar answer = checksum_hash_descriptor(transform, input, digest,
                                                   digest_length, !standard);

        if (!standard)
                system_close((positive)input);

        return answer;
}

static bool checksum_filename_escaped(string_address name)
{
        positive length = string_length(name);

        return memory_first_of(name, '\\', length) ||
               memory_first_of(name, '\n', length);
}

static fn checksum_filename_put(string_address name, bool escaped)
{
        if (!escaped)
        {
                text_put_string(name);
                return;
        }

        string_address from = name;

        while (string_get(from))
        {
                string_address slash = string_first_of(from, '\\');
                string_address newline = string_first_of(from, '\n');
                string_address stop = !slash ? newline
                                      : !newline || slash < newline ? slash
                                                                    : newline;

                if (!stop)
                {
                        text_put_string(from);
                        break;
                }

                text_put(from, (positive)(stop - from));
                text_put_character('\\');
                text_put_character(*stop == '\n' ? 'n' : '\\');
                from = stop + 1;
        }
}

/* The check result is a human-facing shell word in coreutils 9.11, not the
   portable backslash record used when a manifest is written.  Keep the shell
   quoting policy in its existing writer and add only the control-byte islands
   that quotearg spells as $'...'.  The decoded name lives in text_line, so a
   temporary terminator can expose each printable span without allocating or
   building a second output buffer. */
fn shell_quoted(writer write, string_address value);

static fn checksum_check_filename_put(string_address name)
{
        if (!checksum_filename_escaped(name))
        {
                text_put_string(name);
                return;
        }

        p8 address_to step = (p8 address_to)name;

        while (*step)
        {
                p8 address_to stop = step;
                while (*stop && *stop >= ' ' && *stop != 127)
                        stop++;

                if (stop > step)
                {
                        p8 held = *stop;
                        *stop = end;
                        shell_quoted(text_put, step);
                        *stop = held;
                        step = stop;
                }

                if (!*step)
                        break;

                text_put_string("$'");
                do
                {
                        p8 escaped[4];
                        positive length = ls_escape_byte(*step++, escaped,
                                                         false);
                        text_put(escaped, length);
                }
                while (*step && (*step < ' ' || *step == 127));
                text_put_character('\'');
        }
}

static fn checksum_hex_put(p8 address_to digest, positive length)
{
        static p8 alphabet[] = "0123456789abcdef";
        p8 text[128];

        for (positive i = 0; i < length; i++)
        {
                text[i * 2] = alphabet[digest[i] >> 4];
                text[i * 2 + 1] = alphabet[digest[i] & 15];
        }

        text_put(text, length * 2);
}

static fn checksum_line_put(p8 address_to digest, positive length,
                            string_address name)
{
        bool escaped = checksum_filename_escaped(name);

        if (escaped)
                text_put_character('\\');

        checksum_hex_put(digest, length);
        text_put_character(' ');
        text_put_character(checksum_binary ? '*' : ' ');
        checksum_filename_put(name, escaped);
        text_put_character('\n');
}

static fn checksum_check_result_put(string_address name,
                                    string_address result)
{
        checksum_check_filename_put(name);
        text_put_string(": ");
        text_put_string(result);
        text_put_character('\n');
}

static fn checksum_error_number(string_address command,
                                string_address manifest,
                                positive line,
                                string_address label)
{
        p8 digits[24];

        text_flush();
        text_error_raw(command);
        text_error_raw(": ");
        text_error_raw(manifest ? manifest : (string_address) "-");
        text_error_raw(": ");
        positive used = positive_into(digits, line);
        system_write_all(2, digits, used);
        text_error_raw(": improperly formatted ");
        text_error_raw(label);
        text_error_raw(" checksum line\n");
}

static fn checksum_warning(string_address command, positive count,
                           string_address one, string_address many)
{
        p8 digits[24];

        text_flush();
        text_error_raw(command);
        text_error_raw(": WARNING: ");
        positive used = positive_into(digits, count);
        system_write_all(2, digits, used);
        text_error_raw(count == 1 ? one : many);
        text_error_raw("\n");
}

/* Decode a normal GNU checksum record in place.  Tagged and NUL records are
   refused at option parsing, so the only escapes here are the two GNU emits
   for portable newline-delimited output. */
static bool checksum_line_parse(const checksum_algorithm address_to algorithm,
                                p8 address_to expected,
                                string_address address_to filename)
{
        positive at = 0;
        bool escaped = text_line_length && text_line[0] == '\\';

        if (escaped)
                at++;

        positive digits = algorithm->bytes * 2;

        if (text_line_length < at + digits + 2)
                return false;

        for (positive i = 0; i < algorithm->bytes; i++)
        {
                positive high = digit_known(text_line[at + i * 2], 16);
                positive low = digit_known(text_line[at + i * 2 + 1], 16);

                if (high >= 16 || low >= 16)
                        return false;

                expected[i] = (p8)((high << 4) | low);
        }

        at += digits;
        if (text_line[at] != ' ' ||
            (text_line[at + 1] != ' ' && text_line[at + 1] != '*'))
                return false;

        at += 2;
        text_line[text_line_length] = end;
        p8 address_to name = text_line + at;

        if (escaped)
        {
                p8 address_to from = name;
                p8 address_to into = name;

                while (*from)
                {
                        if (*from != '\\')
                        {
                                *into++ = *from++;
                                continue;
                        }

                        from++;
                        if (*from == 'n')
                                *into++ = '\n';
                        else if (*from == '\\')
                                *into++ = '\\';
                        else
                                return false;

                        from++;
                }

                *into = end;
        }

        address_to filename = name;
        return true;
}

static b32 checksum_verify(const checksum_algorithm address_to algorithm,
                           bipolar transform, file_taking address_to taking)
{
        bool quiet = (taking->flags & FILE_FLAG('q')) != 0;
        bool status = (taking->flags & FILE_FLAG('s')) != 0;
        bool strict = (taking->flags & FILE_FLAG('S')) != 0;
        bool warn = (taking->flags & FILE_FLAG('w')) != 0;
        bool ignore_missing = (taking->flags & FILE_FLAG('i')) != 0;
        positive manifests = taking->first < (positive)program_argument_count()
                                 ? (positive)program_argument_count() - taking->first
                                 : 1;
        positive malformed = 0;
        positive formatted = 0;
        positive mismatched = 0;
        positive unreadable = 0;
        positive verified = 0;
        bool failed = false;

        for (positive m = 0; m < manifests; m++)
        {
                string_address manifest = taking->first <
                                                  (positive)program_argument_count()
                                              ? program_argument((b32)(taking->first + m))
                                              : null;

                if (!text_open(manifest))
                {
                        failed = true;
                        continue;
                }

                positive line = 0;

                while (text_line_next())
                {
                        p8 expected[64];
                        p8 digest[64];
                        string_address filename;

                        line++;
                        if (!checksum_line_parse(algorithm, expected,
                                                 address_of filename))
                        {
                                malformed++;
                                if (warn && !status)
                                        checksum_error_number(algorithm->command,
                                                              manifest, line,
                                                              algorithm->label);
                                continue;
                        }

                        formatted++;

                        bipolar hashed = checksum_hash_path(
                            transform, filename, digest, algorithm->bytes);

                        if (hashed == -ERROR_NO_ENTRY && ignore_missing)
                                continue;

                        if (hashed < 0)
                        {
                                unreadable++;
                                failed = true;

                                if (!status)
                                {
                                        text_error(filename,
                                                   file_reason(hashed));
                                        checksum_check_result_put(
                                            filename,
                                            (string_address) "FAILED open or read");
                                }
                                continue;
                        }

                        verified++;
                        if (memory_compare(expected, digest, algorithm->bytes))
                        {
                                mismatched++;
                                failed = true;
                                if (!status)
                                        checksum_check_result_put(
                                            filename, (string_address) "FAILED");
                        }
                        else if (!quiet && !status)
                                checksum_check_result_put(filename,
                                                          (string_address) "OK");
                }

                if (text_input.failed)
                        failed = true;

                text_close();
        }

        if (!status)
        {
                if (malformed && formatted)
                        checksum_warning(algorithm->command, malformed,
                                         (string_address) " line is improperly formatted",
                                         (string_address) " lines are improperly formatted");
                if (unreadable)
                        checksum_warning(algorithm->command, unreadable,
                                         (string_address) " listed file could not be read",
                                         (string_address) " listed files could not be read");
                if (mismatched)
                        checksum_warning(algorithm->command, mismatched,
                                         (string_address) " computed checksum did NOT match",
                                         (string_address) " computed checksums did NOT match");
        }

        if (!verified && !unreadable)
        {
                failed = true;
                if (!status)
                        text_error(taking->first <
                                           (positive)program_argument_count()
                                       ? program_argument((b32)taking->first)
                                       : (string_address) "-",
                                   ignore_missing
                                       ? (string_address) "no file was verified"
                                       : (string_address) "no properly formatted checksum lines found");
        }

        if (strict && malformed)
                failed = true;

        return failed ? 1 : 0;
}

static b32 checksum_main()
{
        string_address command = checksum_called();
        const checksum_algorithm address_to algorithm =
            checksum_algorithm_find(command);

        if (!algorithm)
                return 1;

        text_begin(command);
        checksum_binary = false;

        file_taking taking = {
            .program = command,
            .allowed = algorithm->variable_length
                           ? (string_address) "bctlw"
                           : (string_address) "bctw",
            .valued = algorithm->variable_length
                          ? (string_address) "l"
                          : null,
            .longs = algorithm->variable_length ? checksum_b2_longs
                                                : checksum_longs,
            .seen = checksum_option_seen,
        };

        if (!file_take(address_of taking))
                return text_done(1);

        if (taking.flags & FILE_FLAG('T'))
                return text_refuse(null,
                                   "--tag is not supported by the kernel checksum path",
                                   1);
        if (taking.flags & FILE_FLAG('z'))
                return text_refuse(null,
                                   "--zero is not supported by the line verifier",
                                   1);
        if (taking.flags & FILE_FLAG('l'))
                return text_refuse(null,
                                   "variable BLAKE2 lengths are not supported",
                                   1);

        bool checking = (taking.flags & FILE_FLAG('c')) != 0;
        positive verifying = FILE_FLAG('i') | FILE_FLAG('q') |
                             FILE_FLAG('s') | FILE_FLAG('S') |
                             FILE_FLAG('w');

        if (!checking && (taking.flags & verifying))
                return text_refuse(
                    null,
                    "verification option is meaningful only with --check", 1);
        if (checking && (taking.flags & (FILE_FLAG('b') | FILE_FLAG('t'))))
                return text_refuse(
                    null,
                    "--binary and --text are meaningless with --check", 1);

        bipolar transform = checksum_kernel_open(algorithm);

        if (transform < 0)
                return text_refuse(
                    null,
                    "kernel AF_ALG hash support or requested algorithm is unavailable",
                    1);

        b32 answer = 0;

        if (checking)
                answer = checksum_verify(algorithm, transform,
                                         address_of taking);
        else
        {
                positive count = (positive)program_argument_count();
                positive operands = taking.first < count ? count - taking.first
                                                         : 1;

                for (positive i = 0; i < operands; i++)
                {
                        string_address path = taking.first < count
                                                  ? program_argument((b32)(taking.first + i))
                                                  : (string_address) "-";
                        p8 digest[64];
                        bipolar hashed = checksum_hash_path(
                            transform, path, digest, algorithm->bytes);

                        if (hashed < 0)
                        {
                                text_error(path, file_reason(hashed));
                                answer = 1;
                                continue;
                        }

                        checksum_line_put(digest, algorithm->bytes, path);
                }
        }

        system_close((positive)transform);
        return text_done(answer);
}

#else

/* AF_ALG is a Linux ABI.  Keep non-Linux compilation honest instead of
   substituting an unreviewed software implementation. */
static b32 checksum_main()
{
        return 1;
}

#endif
